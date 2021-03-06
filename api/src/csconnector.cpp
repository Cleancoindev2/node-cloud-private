//#define TRACE_ENABLER

#include "csconnector/csconnector.h"
#include <csdb/currency.h>
#include <thrift/protocol/TJSONProtocol.h>
#include <thrift/transport/THttpServer.h>

namespace csconnector {

using ::apache::thrift::TProcessorFactory;

using namespace ::apache::thrift::stdcxx;
using namespace ::apache::thrift::server;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::protocol;

connector::connector(BlockChain& m_blockchain,
                     Credits::Solver* solver,
                     const Config& config)
  : api_handler(make_shared<api::APIHandler>(m_blockchain, *solver))
  , api_processor(api_handler)
  , p_api_processor_factory(new api::SequentialProcessorFactory(api_processor))
#ifdef BINARY_TCP_API
  , server(p_api_processor_factory,
           make_shared<TServerSocket>(config.port),
           make_shared<TBufferedTransportFactory>(),
           make_shared<TBinaryProtocolFactory>())
#endif
#ifdef AJAX_IFACE
  , ajax_server(p_api_processor_factory,
                make_shared<TServerSocket>(config.ajax_port),
                make_shared<THttpServerTransportFactory>(),
                make_shared<TJSONProtocolFactory>())
#endif
{
#ifdef BINARY_TCP_API
  thread = std::thread([this, config]() {
    try {
      // TRACE("csconnector started on port " << config.port);
      server.run();
    } catch (...) {
      std::cerr << "Oh no! I'm dead :'-(" << std::endl;
    }
  });
#endif
#ifdef AJAX_IFACE
  ajax_server.setConcurrentClientLimit(AJAX_CONCURRENT_API_CLIENTS);
  ajax_thread = std::thread([this, config]() {
    try {
      //  TRACE("csconnector for AJAX started on port " << config.ajax_port);
      ajax_server.run();
    } catch (...) {
      std::cerr << "Oh no! I'm dead in AJAX :'-(" << std::endl;
    }
  });
#endif
}

connector::~connector()
{

#ifdef BINARY_TCP_API
  server.stop();
  if (thread.joinable()) {
    thread.join();
  }
#endif

#ifdef AJAX_IFACE
  ajax_server.stop();
  if (ajax_thread.joinable()) {
    ajax_thread.join();
  }
#endif

  // TRACE("csconnector stopped");
}
}
