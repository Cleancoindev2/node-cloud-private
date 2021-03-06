/* Send blaming letters to @yrtimd */
#include <csnode/packstream.hpp>
#include <lib/system/allocators.hpp>
#include <lib/system/keys.hpp>

#include "network.hpp"
#include "transport.hpp"

enum RegFlags : uint8_t { UsingIPv6 = 1, RedirectIP = 1 << 1, RedirectPort = 1 << 2 };

enum Platform : uint8_t { Linux, MacOS, Windows };

namespace {
// Packets formation

void addMyOut(const Config& config, OPackStream& stream, const uint8_t initFlagValue = 0) {
  uint8_t regFlag = 0;
  if (!config.isSymmetric()) {
    if (config.getAddressEndpoint().ipSpecified) {
      regFlag |= RegFlags::RedirectIP;
      if (config.getAddressEndpoint().ip.is_v6())
        regFlag |= RegFlags::UsingIPv6;
    }

    regFlag |= RegFlags::RedirectPort;
  } else if (config.hasTwoSockets())
    regFlag |= RegFlags::RedirectPort;

  uint8_t* flagChar = stream.getCurrPtr();

  if (!config.isSymmetric()) {
    if (config.getAddressEndpoint().ipSpecified)
      stream << config.getAddressEndpoint().ip;
    else
      stream << (uint8_t)0;

    stream << config.getAddressEndpoint().port;
  } else if (config.hasTwoSockets())
    stream << (uint8_t)0 << config.getInputEndpoint().port;
  else
    stream << (uint8_t)0;

  *flagChar |= initFlagValue | regFlag;
}

void formRegPack(const Config& config, OPackStream& stream, uint64_t** regPackConnId, const PublicKey& pk) {
  stream.init(BaseFlags::NetworkMsg);

  stream << NetworkCommand::Registration << NODE_VERSION;

  addMyOut(config, stream);
  *regPackConnId = (uint64_t*)stream.getCurrPtr();

  stream << (ConnectionId)0 << pk;
}

void formSSConnectPack(const Config& config, OPackStream& stream, const PublicKey& pk) {
  stream.init(BaseFlags::NetworkMsg);
  stream << NetworkCommand::SSRegistration
#ifdef _WIN32
         << Platform::Windows
#elif __APPLE__
         << Platform::MacOS
#else
         << Platform::Linux
#endif
         << NODE_VERSION;

  addMyOut(config, stream, (uint8_t)(config.getNodeType() == NodeType::Router ? 8 : 0));

  stream << pk;
}
}  // namespace

void Transport::run() {
  acceptRegistrations_ = config_.getNodeType() == NodeType::Router;

  {
    SpinLock l(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    formRegPack(config_, oPackStream_, &regPackConnId_, myPublicKey_);
    regPack_ = *(oPackStream_.getPackets());
    oPackStream_.clear();
  }

  refillNeighbourhood();

  // Okay, now let's get to business

  uint32_t ctr = 0;
  for (;;) {
    ++ctr;
    bool askMissing    = true;
    bool resendPacks   = ctr % 2 == 0;
    bool sendPing      = ctr % 20 == 0;
    bool refreshLimits = ctr % 20 == 0;
    bool checkPending  = ctr % 100 == 0;
    bool checkSilent   = ctr % 150 == 0;

    if (askMissing)
      askForMissingPackages();

    if (checkPending)
      nh_.checkPending();

    if (checkSilent)
      nh_.checkSilent();

    if (resendPacks)
      nh_.resendPackets();

    if (sendPing)
      nh_.pingNeighbours();

    if (refreshLimits)
      nh_.refreshLimits();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

template <>
uint16_t getHashIndex(const ip::udp::endpoint& ep) {
  uint16_t result = ep.port();

  if (ep.protocol() == ip::udp::v4()) {
    uint32_t addr = ep.address().to_v4().to_uint();
    result ^= *(uint16_t*)&addr;
    result ^= *((uint16_t*)&addr + 1);
  } else {
    auto bytes    = ep.address().to_v6().to_bytes();
    auto ptr      = (uint8_t*)&result;
    auto bytesPtr = bytes.data();
    for (size_t i = 0; i < 8; ++i)
      *ptr ^= *(bytesPtr++);
    ++ptr;
    for (size_t i = 8; i < 16; ++i)
      *ptr ^= *(bytesPtr++);
  }

  return result;
}

RemoteNodePtr Transport::getPackSenderEntry(const ip::udp::endpoint& ep) {
  auto& rn = remoteNodesMap_.tryStore(ep);

  if (!rn)  // Newcomer
    rn = remoteNodes_.emplace();

  rn->packets.fetch_add(1, std::memory_order_relaxed);
  return rn;
}

void Transport::sendDirect(const Packet* pack, const Connection& conn) {
  uint32_t nextBytesCount = conn.lastBytesCount.load(std::memory_order_relaxed) + pack->size();
  if (nextBytesCount <= conn.BytesLimit) {
    conn.lastBytesCount.fetch_add(pack->size(), std::memory_order_relaxed);
    net_->sendDirect(*pack, conn.getOut());
  }
}

// Processing network packages

void Transport::processNetworkTask(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
  iPackStream_.init(task->pack.getMsgData(), task->pack.getMsgSize());

  NetworkCommand cmd;
  iPackStream_ >> cmd;

  if (!iPackStream_.good())
    return sender->addStrike();

  bool result = true;
  switch (cmd) {
    case NetworkCommand::Registration:
      result = gotRegistrationRequest(task, sender);
      break;
    case NetworkCommand::ConfirmationRequest:
      break;
    case NetworkCommand::ConfirmationResponse:
      break;
    case NetworkCommand::RegistrationConfirmed:
      result = gotRegistrationConfirmation(task, sender);
      break;
    case NetworkCommand::RegistrationRefused:
      result = gotRegistrationRefusal(task, sender);
      break;
    case NetworkCommand::Ping:
      gotPing(task, sender);
      break;
    case NetworkCommand::SSRegistration:
      gotSSRegistration(task, sender);
      break;
    case NetworkCommand::SSFirstRound:
      gotSSDispatch(task);
      break;
    case NetworkCommand::SSRegistrationRefused:
      gotSSRefusal(task);
      break;
    case NetworkCommand::SSPingWhiteNode:
      gotSSPingWhiteNode(task);
      break;
    case NetworkCommand::SSLastBlock:
      gotSSLastBlock(task, node_->getBlockChain().getLastWrittenSequence());
      break;
    case NetworkCommand::PackInform:
      gotPackInform(task, sender);
      break;
    case NetworkCommand::PackRenounce:
      gotPackRenounce(task, sender);
      break;
    case NetworkCommand::PackRequest:
      gotPackRequest(task, sender);
      break;
    default:
      result = false;
      LOG_WARN("Unexpected network command");
  }

  if (!result)
    sender->addStrike();
}

void Transport::refillNeighbourhood() {
  if (config_.getBootstrapType() == BootstrapType::IpList) {
    for (auto& ep : config_.getIpList()) {
      if (!nh_.canHaveNewConnection()) {
        LOG_WARN("Connections limit reached");
        break;
      }

      LOG_EVENT("Creating connection to " << ep.ip);
      nh_.establishConnection(net_->resolve(ep));
    }
  }

  if (config_.getBootstrapType() == BootstrapType::SignalServer || config_.getNodeType() == NodeType::Router) {
    // Connect to SS logic
    ssEp_ = net_->resolve(config_.getSignalServerEndpoint());
    LOG_EVENT("Connecting to Signal Server on " << ssEp_);

    {
      SpinLock l(oLock_);
      formSSConnectPack(config_, oPackStream_, myPublicKey_);
      ssStatus_ = SSBootstrapStatus::Requested;
      net_->sendDirect(*(oPackStream_.getPackets()), ssEp_);
    }
  }
}

bool Transport::parseSSSignal(const TaskPtr<IPacMan>& task) {
  iPackStream_.init(task->pack.getMsgData(), task->pack.getMsgSize());
  iPackStream_.safeSkip<uint8_t>(1);

  RoundNum rNum;
  iPackStream_ >> rNum;

  auto trStart = iPackStream_.getCurrPtr();
  // iPackStream_.safeSkip<uint32_t>();

  uint8_t numConf;
  iPackStream_ >> numConf;
  if (!iPackStream_.good())
    return false;

  iPackStream_.safeSkip<PublicKey>(numConf + 1);

  auto trFinish = iPackStream_.getCurrPtr();
  node_->getRoundTable(trStart, (trFinish - trStart), rNum);

  uint8_t numCirc;
  iPackStream_ >> numCirc;
  if (!iPackStream_.good())
    return false;

  if (config_.getBootstrapType() == BootstrapType::SignalServer) {
    for (uint8_t i = 0; i < numCirc; ++i) {
      EndpointData ep;
      ep.ipSpecified = true;

      iPackStream_ >> ep.ip >> ep.port;
      if (!iPackStream_.good())
        return false;

      nh_.establishConnection(net_->resolve(ep));

      iPackStream_.safeSkip<PublicKey>();
      if (!iPackStream_.good())
        return false;

      if (!nh_.canHaveNewConnection())
        break;
    }
  }

  ssStatus_ = SSBootstrapStatus::Complete;

  return true;
}

constexpr const uint32_t StrippedDataSize = sizeof(RoundNum) + sizeof(MsgTypes);
void                     Transport::processNodeMessage(const Message& msg) {
  auto type = msg.getFirstPack().getType();
  auto rNum = msg.getFirstPack().getRoundNum();

  switch (node_->chooseMessageAction(rNum, type)) {
    case Node::MessageActions::Process:
      return dispatchNodeMessage(type, rNum, msg.getFirstPack(), msg.getFullData() + StrippedDataSize,
                                 msg.getFullSize() - StrippedDataSize);
    case Node::MessageActions::Postpone:
      return postponePacket(rNum, type, msg.extractData());
    case Node::MessageActions::Drop:
      return;
  }
}

bool Transport::shouldSendPacket(const Packet& pack) {
  if (pack.isNetwork()) return false;
  const auto rLim = std::max(node_->getRoundNumber(), (RoundNum)1) - 1;

  if (!pack.isFragmented()) return pack.getRoundNum() >= rLim;
  auto& rn = fragOnRound_.tryStore(pack.getHeaderHash());

  if (pack.getFragmentId() == 0)
    rn = pack.getRoundNum();

  return !rn || rn >= rLim;
}

void Transport::processNodeMessage(const Packet& pack) {
  auto type = pack.getType();
  auto rNum = pack.getRoundNum();

  switch (node_->chooseMessageAction(rNum, type)) {
    case Node::MessageActions::Process:
      return dispatchNodeMessage(type, rNum, pack, pack.getMsgData() + StrippedDataSize,
                                 pack.getMsgSize() - StrippedDataSize);
    case Node::MessageActions::Postpone:
      return postponePacket(rNum, type, pack);
    case Node::MessageActions::Drop:
      return;
  }
}

inline void Transport::postponePacket(const RoundNum rNum, const MsgTypes type, const Packet& pack) {
  (*postponed_)->emplace(rNum, type, pack);
}

void Transport::processPostponed(const RoundNum rNum) {
  auto& ppBuf = *postponed_[1];
  for (auto& pp : **postponed_) {
    if (pp.round > rNum)
      ppBuf.emplace(std::move(pp));
    else if (pp.round == rNum)
      dispatchNodeMessage(pp.type, pp.round, pp.pack, pp.pack.getMsgData() + StrippedDataSize,
                          pp.pack.getMsgSize() - StrippedDataSize);
  }

  (*postponed_)->clear();

  postponed_[1] = *postponed_;
  postponed_[0] = &ppBuf;

  std::cout << "TRANSPORT> POSTPHONED finish" << std::endl;
}

void Transport::dispatchNodeMessage(const MsgTypes type, const RoundNum rNum, const Packet& firstPack,
                                    const uint8_t* data, size_t size) {
  if (!size) {
    LOG_ERROR("Bad packet size, why is it zero?");
    return;
  }
  // std::cout << __func__ << std::endl;
  switch (type) {
    case MsgTypes::RoundTable:
      return node_->getRoundTable(data, size, rNum);
    case MsgTypes::Transactions:
      return node_->getTransaction(data, size);
    case MsgTypes::FirstTransaction:
      return node_->getFirstTransaction(data, size);
    case MsgTypes::TransactionList:
      return node_->getTransactionsList(data, size);
    case MsgTypes::ConsVector:
      return node_->getVector(data, size, firstPack.getSender());
    case MsgTypes::ConsMatrix:
      return node_->getMatrix(data, size, firstPack.getSender());
    case MsgTypes::NewBlock:
      return node_->getBlock(data, size, firstPack.getSender());
    case MsgTypes::BlockHash:
      return node_->getHash(data, size, firstPack.getSender());
    case MsgTypes::BlockRequest:
      return node_->getBlockRequest(data, size, firstPack.getSender());
    case MsgTypes::RequestedBlock:
      return node_->getBlockReply(data, size);
    case MsgTypes::ConsVectorRequest:
      return node_->getVectorRequest(data, size);
    case MsgTypes::ConsMatrixRequest:
      return node_->getMatrixRequest(data, size);
    case MsgTypes::RoundTableRequest:
      return node_->getRoundTableRequest(data, size, firstPack.getSender());
    case MsgTypes::ConsTLRequest:
      return node_->getTlRequest(data, size, firstPack.getSender());
    case MsgTypes::NewBadBlock:
      return node_->getBadBlock(data, size, firstPack.getSender());
    case MsgTypes::BigBang:
      return node_->getBigBang(data, size, rNum, type);
    default:
      LOG_ERROR("Unknown type");
      break;
  }
}

void Transport::registerTask(Packet* pack, const uint32_t packNum, const bool incrementWhenResend) {
  auto end = pack + packNum;

  for (auto ptr = pack; ptr != end; ++ptr) {
    SpinLock     l(sendPacksFlag_);
    PackSendTask pst;
    pst.pack        = *ptr;
    pst.incrementId = incrementWhenResend;
    sendPacks_.emplace(pst);
  }
}

void Transport::addTask(Packet* pack, const uint32_t packNum, bool incrementWhenResend, bool sendToNeighbours) {
  if (sendToNeighbours)
    nh_.pourByNeighbours(pack, packNum);
  if (packNum > 1) {
    net_->registerMessage(pack, packNum);
    registerTask(pack, 1, incrementWhenResend);
  } else if (sendToNeighbours)
    registerTask(pack, packNum, incrementWhenResend);
}

void Transport::clearTasks() {
  SpinLock l(sendPacksFlag_);
  sendPacks_.clear();
}

/* Sending network tasks */
void Transport::sendRegistrationRequest(Connection& conn) {
  LOG_EVENT("Sending registration request to " << (conn.specialOut ? conn.out : conn.in));
  Packet req(netPacksAllocator_.allocateNext(regPack_.size()));
  *regPackConnId_ = conn.id;
  memcpy(req.data(), regPack_.data(), regPack_.size());

  ++(conn.attempts);
  sendDirect(&req, conn);
}

void Transport::sendRegistrationConfirmation(const Connection& conn, const Connection::Id requestedId) {
  LOG_EVENT("Confirming registration with " << conn.getOut());

  SpinLock l(oLock_);
  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::RegistrationConfirmed << requestedId << conn.id << myPublicKey_;

  sendDirect(oPackStream_.getPackets(), conn);
  oPackStream_.clear();
}

void Transport::sendRegistrationRefusal(const Connection& conn, const RegistrationRefuseReasons reason) {
  LOG_EVENT("Refusing registration with " << conn.in);

  SpinLock l(oLock_);
  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::RegistrationRefused << conn.id << reason;

  sendDirect(oPackStream_.getPackets(), conn);
  oPackStream_.clear();
}

// Requests processing

bool Transport::gotRegistrationRequest(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
  LOG_EVENT("Got registration request from " << task->sender);

  NodeVersion vers;
  iPackStream_ >> vers;
  if (!iPackStream_.good())
    return false;

  Connection conn;
  conn.in     = task->sender;
  auto& flags = iPackStream_.peek<uint8_t>();

  if (flags & RegFlags::RedirectIP) {
    boost::asio::ip::address addr;
    iPackStream_ >> addr;

    conn.out.address(addr);
    conn.specialOut = true;
  } else {
    conn.specialOut = false;
    iPackStream_.skip<uint8_t>();
  }

  if (flags & RegFlags::RedirectPort) {
    Port port;
    iPackStream_ >> port;

    if (!conn.specialOut) {
      conn.specialOut = true;
      conn.out.address(task->sender.address());
    }

    conn.out.port(port);
  } else if (conn.specialOut)
    conn.out.port(task->sender.port());

  if (vers != NODE_VERSION) {
    sendRegistrationRefusal(conn, RegistrationRefuseReasons::BadClientVersion);
    return true;
  }

  iPackStream_ >> conn.id;
  iPackStream_ >> conn.key;

  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  nh_.gotRegistration(std::move(conn), sender);
  return true;
}

bool Transport::gotRegistrationConfirmation(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
  LOG_EVENT("Got registration confirmation from " << task->sender);

  ConnectionId myCId;
  ConnectionId realCId;
  PublicKey    key;
  iPackStream_ >> myCId >> realCId >> key;

  if (!iPackStream_.good())
    return false;

  nh_.gotConfirmation(myCId, realCId, task->sender, key, sender);
  return true;
}

bool Transport::gotRegistrationRefusal(const TaskPtr<IPacMan>& task, RemoteNodePtr&) {
  LOG_EVENT("Got registration refusal from " << task->sender);

  RegistrationRefuseReasons reason;
  Connection::Id            id;
  iPackStream_ >> id >> reason;

  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  nh_.gotRefusal(id);

  LOG_EVENT("Registration to " << task->sender << " refused. Reason: " << (int)reason);

  return true;
}

bool Transport::gotSSRegistration(const TaskPtr<IPacMan>& task, RemoteNodePtr& rNode) {
  if (ssStatus_ != SSBootstrapStatus::Requested) {
    LOG_WARN("Unexpected Signal Server response");
    return false;
  }

  LOG_EVENT("Connection to the Signal Server has been established");
  nh_.addSignalServer(task->sender, ssEp_, rNode);

  if (task->pack.getMsgSize() > 2) {
    if (!parseSSSignal(task))
      LOG_WARN("Bad Signal Server response");
  } else
    ssStatus_ = SSBootstrapStatus::RegisteredWait;

  return true;
}

bool Transport::gotSSDispatch(const TaskPtr<IPacMan>& task) {
  if (ssStatus_ != SSBootstrapStatus::RegisteredWait)
    LOG_WARN("Unexpected Signal Server response");

  if (!parseSSSignal(task))
    LOG_WARN("Bad Signal Server response");

  return true;
}

bool Transport::gotSSRefusal(const TaskPtr<IPacMan>&) {
  uint16_t expectedVersion;
  iPackStream_ >> expectedVersion;

  LOG_ERROR("The Signal Server has refused the registration due to your bad client version. The expected version is "
            << expectedVersion);

  return true;
}

bool Transport::gotSSPingWhiteNode(const TaskPtr<IPacMan>& task) {
  Connection conn;
  conn.in         = task->sender;
  conn.specialOut = false;
  sendDirect(&task->pack, conn);
  return true;
}

bool Transport::gotSSLastBlock(const TaskPtr<IPacMan>& task, uint32_t lastBlock) {
  Connection conn;
  conn.in         = net_->resolve(config_.getSignalServerEndpoint());
  conn.specialOut = false;

  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::SSLastBlock << NODE_VERSION;
  oPackStream_ << lastBlock << myPublicKey_;

  sendDirect(oPackStream_.getPackets(), conn);
  return true;
}

void Transport::gotPacket(const Packet& pack, RemoteNodePtr& sender) {
  if (!pack.isFragmented())
    return;

  nh_.neighbourSentPacket(sender, pack.getHeaderHash());
}

void Transport::redirectPacket(const Packet& pack, RemoteNodePtr& sender) {
  auto conn = sender->connection.load(std::memory_order_relaxed);
  if (!conn)
    return;

  sendPackInform(pack, *conn);

  if (pack.isFragmented() && pack.getFragmentsNum() > Packet::SmartRedirectTreshold) {
    nh_.redirectByNeighbours(&pack);
  }
  else {
    nh_.neighbourHasPacket(sender, pack.getHash());
    sendBroadcast(&pack);
  }
}

void Transport::sendPackInform(const Packet& pack, const Connection& addr) {
  SpinLock l(oLock_);
  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::PackInform << pack.getHash();
  sendDirect(oPackStream_.getPackets(), addr);
  oPackStream_.clear();
}

bool Transport::gotPackInform(const TaskPtr<IPacMan>&, RemoteNodePtr& sender) {
  Hash hHash;
  iPackStream_ >> hHash;
  if (!iPackStream_.good() || !iPackStream_.end()) return false;

  nh_.neighbourHasPacket(sender, hHash);
  return true;
}

void Transport::sendPackRenounce(const Hash& hash, const Connection& addr) {
  SpinLock l(oLock_);
  oPackStream_.init(BaseFlags::NetworkMsg);

  oPackStream_ << NetworkCommand::PackRenounce << hash;

  sendDirect(oPackStream_.getPackets(), addr);
  oPackStream_.clear();
}

bool Transport::gotPackRenounce(const TaskPtr<IPacMan>&, RemoteNodePtr& sender) {
  Hash hHash;

  iPackStream_ >> hHash;
  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  nh_.neighbourSentRenounce(sender, hHash);

  return true;
}

void Transport::askForMissingPackages() {
  typename decltype(uncollected_)::const_iterator ptr;
  MessagePtr                                      msg;
  uint32_t                                        i = 0;

  const uint64_t maxMask = (uint64_t)1 << 63;

  for (;;) {
    {
      SpinLock l(uLock_);
      if (i >= uncollected_.size())
        break;

      ptr = uncollected_.begin();
      for (uint32_t j = 0; j < i; ++j)
        ++ptr;

      msg = *ptr;
      ++i;
    }

    {
      SpinLock   l(msg->pLock_);
      const auto end = msg->packets_ + msg->packetsTotal_;

      uint16_t start;
      uint64_t mask = 0;
      uint64_t req  = 0;

      for (auto s = msg->packets_; s != end; ++s) {
        if (!*s) {
          if (!mask) {
            mask  = 1;
            start = s - msg->packets_;
          }
          req |= mask;
        }

        if (mask == maxMask) {
          requestMissing(msg->headerHash_, start, req);

          if (s > (msg->packets_ + msg->maxFragment_) && (end - s) > 128)
            break;

          mask = 0;
        } else
          mask <<= 1;
      }

      if (mask)
        requestMissing(msg->headerHash_, start, req);
    }
  }
}

void Transport::requestMissing(const Hash& hash, const uint16_t start, const uint64_t req) {
  Packet p;

  {
    SpinLock l(oLock_);
    oPackStream_.init(BaseFlags::NetworkMsg);
    oPackStream_ << NetworkCommand::PackRequest << hash << start << req;
    p = *(oPackStream_.getPackets());
    oPackStream_.clear();
  }

  ConnectionPtr requestee = nh_.getNextRequestee(hash);
  if (requestee)
    sendDirect(&p, **requestee);
}

void Transport::registerMessage(MessagePtr msg) {
  SpinLock l(uLock_);
  uncollected_.emplace(msg);
}

bool Transport::gotPackRequest(const TaskPtr<IPacMan>&, RemoteNodePtr& sender) {
  Connection* conn = sender->connection.load(std::memory_order_acquire);
  if (!conn)
    return false;
  auto ep = conn->specialOut ? conn->out : conn->in;

  Hash     hHash;
  uint16_t start;
  uint64_t req;

  iPackStream_ >> hHash >> start >> req;
  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  uint32_t reqd = 0, snt = 0;
  uint64_t mask = 1;
  while (mask) {
    if (mask & req) {
      ++reqd;
      if (net_->resendFragment(hHash, start, ep))
        ++snt;
    }
    ++start;
    mask <<= 1;
  }

  return true;
}

void Transport::sendPingPack(const Connection& conn) {
  SpinLock l(oLock_);
  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::Ping << conn.id;
  sendDirect(oPackStream_.getPackets(), conn);
  oPackStream_.clear();
}

bool Transport::gotPing(const TaskPtr<IPacMan>& task, RemoteNodePtr& sender) {
  Connection::Id id;

  iPackStream_ >> id;
  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  nh_.validateConnectionId(sender, id, task->sender);

  return true;
}
