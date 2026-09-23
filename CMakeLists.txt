cmake_minimum_required(VERSION 3.31)

project(
  ds-service
  VERSION 6.1.0
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
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/cpp/grpc
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp/grpc/ds-service.proto
  DEPENDS cpp/grpc/ds-service.proto
  VERBATIM)

# Targets, from the bottom up.
# ds-service-common and ds-service-core never link gRPC or protobuf,
# so a gRPC include in either one fails to compile.
# All gRPC code lives in the ds-service-grpc-* targets, under cpp/grpc/.

# The code protoc generates from cpp/grpc/ds-service.proto.
add_library(
  ds-service-grpc-stubs STATIC ${CMAKE_CURRENT_BINARY_DIR}/ds-service.pb.cc
                               ${CMAKE_CURRENT_BINARY_DIR}/ds-service.grpc.pb.cc)
target_sources(
  ds-service-grpc-stubs
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         ${CMAKE_CURRENT_BINARY_DIR}
         FILES
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.pb.h
         ${CMAKE_CURRENT_BINARY_DIR}/ds-service.grpc.pb.h)
target_link_libraries(ds-service-grpc-stubs PUBLIC protobuf::protobuf grpc::grpc)
target_compile_features(ds-service-grpc-stubs PRIVATE cxx_std_17)

# The plain message types, TaskState and the error types.
add_library(ds-service-common INTERFACE)
target_sources(
  ds-service-common
  INTERFACE FILE_SET
            HEADERS
            BASE_DIRS
            cpp/common/include
            FILES
            cpp/common/include/ds-service/error.hpp
            cpp/common/include/ds-service/messages.hpp
            cpp/common/include/ds-service/task-state.hpp)
target_compile_features(ds-service-common INTERFACE cxx_std_23)

# The server's data structures, and the interface every server transport implements.
add_library(
  ds-service-core STATIC
  cpp/server/core/counters.cpp
  cpp/server/core/journal-map.cpp
  cpp/server/core/map.cpp
  cpp/server/core/mutexes.cpp
  cpp/server/core/task-manager.cpp
  cpp/server/core/time-series.cpp)
target_sources(
  ds-service-core
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         cpp/server
         FILES
         cpp/server/transport.hpp
         cpp/server/core/data-structures.hpp
         cpp/server/core/system-state.hpp)
target_link_libraries(
  ds-service-core
  PUBLIC ds-service-common phmap
  PRIVATE spdlog::spdlog re2::re2)
target_compile_features(ds-service-core PUBLIC cxx_std_23)

# Plain messages <-> protobuf messages, and the gRPC channel settings.
# Shared by the server transport and, later, the client transport.
add_library(ds-service-grpc-codec STATIC cpp/grpc/codec.cpp)
target_sources(
  ds-service-grpc-codec
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         cpp/grpc
         FILES
         cpp/grpc/channel-settings.hpp
         cpp/grpc/codec.hpp)
target_link_libraries(ds-service-grpc-codec PUBLIC ds-service-common
                                                   ds-service-grpc-stubs)
target_compile_features(ds-service-grpc-codec PUBLIC cxx_std_23)

# The gRPC server transport.
add_library(ds-service-grpc-server STATIC
            cpp/grpc/server/grpc-server-transport.cpp)
target_sources(
  ds-service-grpc-server
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         cpp/grpc
         FILES
         cpp/grpc/server/grpc-server-transport.hpp)
target_link_libraries(
  ds-service-grpc-server
  PUBLIC ds-service-core
  PRIVATE ds-service-grpc-codec spdlog::spdlog)
target_compile_features(ds-service-grpc-server PUBLIC cxx_std_23)

add_executable(ds-service cpp/server/main.cpp)
target_link_libraries(ds-service PRIVATE ds-service-core ds-service-grpc-server
                                         spdlog::spdlog argparse::argparse)
target_compile_features(ds-service PRIVATE cxx_std_23)

install(TARGETS ds-service)
