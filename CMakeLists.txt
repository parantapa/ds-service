cmake_minimum_required(VERSION 4.0)

project(
  ds-service
  VERSION 0.0.1
  LANGUAGES CXX)

find_package(OpenMP REQUIRED)

find_package(spdlog REQUIRED)
find_package(argparse REQUIRED)
find_package(phmap REQUIRED)
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

add_executable(ds-server cpp/ds-server.cpp)
target_sources(ds-server PRIVATE FILE_SET HEADERS BASE_DIRS cpp)
target_link_libraries(ds-server PRIVATE spdlog::spdlog argparse::argparse phmap
                                        ds-service-grpc OpenMP::OpenMP_CXX)
target_compile_features(ds-server PRIVATE cxx_std_23)

install(TARGETS ds-service-grpc FILE_SET HEADERS)
install(TARGETS ds-server)
