cmake_minimum_required(VERSION 3.10)

project(lib)

add_library(lib
  src/lib/system/logger.cpp
  include/lib/system/hash.hpp
  include/lib/system/queues.hpp
  include/lib/system/structures.hpp
  include/lib/system/keys.hpp
  include/lib/system/logger.hpp
  include/lib/system/allocators.hpp
)
target_include_directories(lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../lib/include)
