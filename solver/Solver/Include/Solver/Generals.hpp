////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                    Created by Analytical Solytions Core Team 07.09.2018                                //
////////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <blake2-impl.h>
#include <blake2.h>
#include <string>
#include <vector>

#include <csdb/csdb.h>
#include <csdb/pool.h>
#include <map>

#include <csnode/node.hpp>
#include <lib/system/keys.hpp>

namespace Credits {

class Solver;

class Generals {
 public:
  Generals();
  ~Generals();

  Generals(const Generals&) = delete;
  Generals& operator=(const Generals&) = delete;

  // Rewrite method//
  void chooseHeadAndTrusted(std::map<std::string, std::string>);
  void chooseHeadAndTrustedFake(std::vector<std::string>& hashes);

  Hash_ buildvector(csdb::Pool& _pool, csdb::Pool& new_pool, size_t num_of_trusted, csdb::Pool& new_bpool);

  void addvector(const HashVector& vector);
  void addmatrix(const HashMatrix& matrix, const std::vector<PublicKey>& confidantNodes);

  // take desision
  uint8_t take_decision(const std::vector<PublicKey>&, const uint8_t myConfNum, const csdb::PoolHash& lasthash);

  const HashMatrix& getMatrix() const;

  void addSenderToMatrix(uint8_t myConfNum);

  void fake_block(std::string);

 private:
  csdb::Amount countFee(csdb::Transaction& transation, size_t numOfTrustedNodesInRound,
                        size_t numOfTransactionsInRound);

  struct hash_weight {
    char    a_hash[32];
    uint8_t a_weight;
  };
  // unsigned char hash_vector[97];
  // unsigned char hash_matrix[9700];
  // unsigned char got_matrix[9700];
  HashMatrix  hMatrix;
  uint8_t     find_untrusted[10000];
  uint8_t     new_trusted[100];
  hash_weight hw_total[100];

  // void encrypt_vector(std::string& vector_string
  //  ,std::vector<int64_t>& vector_data);

  // int decode_matrix(std::string& matrix);

  // std::vector<std::string> vector_datas;
  // std::vector<std::string> matrix_data;
};
}  // namespace Credits
