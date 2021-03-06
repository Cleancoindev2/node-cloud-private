cmake_minimum_required(VERSION 3.11)

# �������, ����� �������� ���������� ����� �� � ${CMAKE_CURRENT_SOURCE_DIR}, � �
# ${CMAKE_CURRENT_BINARY_DIR}. �� ���� ��� � ${CMAKE_CURRENT_SOURCE_DIR}, ���� �������
# �������� � .gitignore
set(THRIFT_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR})

# ������ ������ ����� ������ �������. �� ����� ��������� thirft compiler ���
# �������� ����� �� ���� ������. ��� ��������� thrift-����� ������ ���� �����
# �������� �������.
set(THRIFT_GENERATED_FILES
  ${THRIFT_GEN_DIR}/variant_constants.cpp
  ${THRIFT_GEN_DIR}/variant_types.cpp
  ${THRIFT_GEN_DIR}/variant_constants.h
  ${THRIFT_GEN_DIR}/variant_types.h
  )

# ����� thrift compiler ����� ��������� � ���� ������ - �����
# � ������ ��������� thrift-����� �� �� ����� ����������������.
add_custom_command(
  OUTPUT ${THRIFT_GENERATED_FILES}
  COMMAND thrift-compiler -r -gen cpp:no_skeleton,pure_enums,moveable_types
    -out ${THRIFT_GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/thrift-interface-definitions/variant.thrift
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/thrift-interface-definitions/variant.thrift
  )

add_library(variant_gen
    ${THRIFT_GENERATED_FILES}
	variant_aux.cpp
)

target_link_libraries (variant_gen PUBLIC thrift_static)
target_include_directories(variant_gen PUBLIC
  ${THRIFT_GEN_DIR})

set_property(TARGET variant_gen PROPERTY CXX_STANDARD 14)
set_property(TARGET variant_gen PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
