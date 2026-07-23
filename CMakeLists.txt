cmake_minimum_required(VERSION 4.0)

project(
  ds-service
  VERSION 0.0.1
  LANGUAGES CXX)

find_package(spdlog REQUIRED)
find_package(argparse REQUIRED)
find_package(phmap REQUIRED)
find_package(re2 REQUIRED)
find_package(gRPC REQUIRED)
find_package(protobuf REQUIRED)

find_program(PROTOC_EXE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXE grpc_cpp_plugin REQUIRED)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ds-service.pb.h
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.pb.cc
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.grpc.pb.h
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.grpc.pb.cc
  COMMAND
    ${PROTOC_EXE} --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXE}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/misc
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/ds-service.proto
  DEPENDS misc/ds-service.proto
  VERBATIM)

add_library(ds-service-grpc ${CMAKE_CURRENT_BINARY_DIR}/ds-service.pb.cc
                            ${CMAKE_CURRENT_BINARY_DIR}/ds-service.grpc.pb.cc)
target_sources(
  ds-service-grpc
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         ${CMAKE_CURRENT_BINARY_DIR}
         FILES
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.pb.h
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.grpc.pb.h)
target_link_libraries(ds-service-grpc PUBLIC protobuf::protobuf grpc::grpc)
target_compile_features(ds-service-grpc PRIVATE cxx_std_17)

add_executable(ds-service cpp/ds-service.cpp)
target_sources(ds-service PRIVATE FILE_SET HEADERS BASE_DIRS cpp)
target_link_libraries(
  ds-service
  PRIVATE spdlog::spdlog
          argparse::argparse
          re2::re2
          phmap
          ds-service-grpc)
target_compile_features(ds-service PRIVATE cxx_std_23)

install(TARGETS ds-service)
