cmake_minimum_required(VERSION 3.31)

include(cmake/conan-install.cmake)

project(
  ds-service
  VERSION 6.1.0
  LANGUAGES CXX)

# The Conan toolchain sets defaults for the first three from the with_* options
# in conanfile.py, and a -D on the command line overrides it.
option(DS_SERVICE_BUILD_SERVER "Build the ds-service server" ON)
option(DS_SERVICE_BUILD_CLIENT "Build the C++ client libraries" ON)
option(DS_SERVICE_BUILD_PYTHON "Build the Python extension module ds_service_client._ext" OFF)
option(DS_SERVICE_CONAN_INSTALL "Run conan install at configure time. See cmake/conan-install.cmake" OFF)
# The test suite imports ds_service_client from python/,
# so a development build copies the module and its stub there.
# A wheel build installs them instead.
option(DS_SERVICE_PYTHON_INPLACE "Copy the built Python module into python/ds_service_client" ON)

if(DS_SERVICE_BUILD_PYTHON AND NOT DS_SERVICE_BUILD_CLIENT)
  message(FATAL_ERROR "DS_SERVICE_BUILD_PYTHON needs DS_SERVICE_BUILD_CLIENT.")
endif()

# The Python module links every static library below into one shared object,
# so they must be position independent.
# Hidden visibility keeps their symbols out of the module's exports.
if(DS_SERVICE_BUILD_PYTHON)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

if(DS_SERVICE_BUILD_SERVER)
  find_package(spdlog REQUIRED)
  find_package(argparse REQUIRED)
  find_package(phmap REQUIRED)
  find_package(re2 REQUIRED)
endif()
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
# ds-service-common, ds-service-core and ds-service-client
# never link gRPC or protobuf,
# so a gRPC include in any of them fails to compile.
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

# Plain messages <-> protobuf messages, and the gRPC channel settings.
# Shared by the server transport and the client transport.
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

if(DS_SERVICE_BUILD_SERVER)
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
endif()

if(DS_SERVICE_BUILD_CLIENT)
  # The C++ client, over the ClientTransport interface.
  add_library(ds-service-client STATIC cpp/client/client.cpp)
  target_sources(
    ds-service-client
    PUBLIC FILE_SET
           HEADERS
           BASE_DIRS
           cpp/client/include
           FILES
           cpp/client/include/ds-service/client.hpp
           cpp/client/include/ds-service/client-transport.hpp)
  target_link_libraries(ds-service-client PUBLIC ds-service-common)
  target_compile_features(ds-service-client PUBLIC cxx_std_23)

  # The gRPC client transport.
  add_library(ds-service-grpc-client STATIC
              cpp/grpc/client/grpc-client-transport.cpp)
  target_sources(
    ds-service-grpc-client
    PUBLIC FILE_SET
           HEADERS
           BASE_DIRS
           cpp/grpc
           FILES
           cpp/grpc/client/grpc-client-transport.hpp)
  target_link_libraries(
    ds-service-grpc-client
    PUBLIC ds-service-client
    PRIVATE ds-service-grpc-codec)
  target_compile_features(ds-service-grpc-client PUBLIC cxx_std_23)

  # ds::connect(), which picks a transport from an address.
  # It links every client transport,
  # so a C++ program that uses the client links this target.
  add_library(ds-service-connect STATIC cpp/client/connect.cpp)
  target_sources(
    ds-service-connect
    PUBLIC FILE_SET
           HEADERS
           BASE_DIRS
           cpp/client/include
           FILES
           cpp/client/include/ds-service/connect.hpp)
  target_link_libraries(
    ds-service-connect
    PUBLIC ds-service-client
    PRIVATE ds-service-grpc-client)
  target_compile_features(ds-service-connect PUBLIC cxx_std_23)
endif()

# The smoke test runs the C++ client against the server,
# so it needs both.
if(DS_SERVICE_BUILD_SERVER AND DS_SERVICE_BUILD_CLIENT)
  enable_testing()
  add_executable(client-smoke cpp/tests/client-smoke.cpp)
  target_link_libraries(client-smoke PRIVATE ds-service-connect)
  target_compile_features(client-smoke PRIVATE cxx_std_23)
  add_test(NAME client-smoke COMMAND client-smoke $<TARGET_FILE:ds-service>)
endif()

if(DS_SERVICE_BUILD_PYTHON)
  # scikit-build-core sets SKBUILD_SABI_COMPONENT for a stable ABI wheel.
  # A development build asks for the same component itself.
  if(NOT DEFINED SKBUILD_SABI_COMPONENT)
    set(SKBUILD_SABI_COMPONENT Development.SABIModule)
  endif()
  find_package(
    Python 3.12 REQUIRED
    COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT})
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE nanobind_ROOT COMMAND_ERROR_IS_FATAL ANY)
  find_package(nanobind CONFIG REQUIRED)

  # STABLE_ABI builds one module for every Python from 3.12 on.
  nanobind_add_module(_ext STABLE_ABI cpp/python/bindings.cpp)
  target_link_libraries(_ext PRIVATE ds-service-connect)
  target_compile_features(_ext PRIVATE cxx_std_23)
  # Keep the symbols of every static library, gRPC, protobuf and Abseil included,
  # out of the module's dynamic symbol table.
  # A process can then load another copy of gRPC, such as grpcio's, beside this one.
  target_link_options(_ext PRIVATE "LINKER:--exclude-libs,ALL")

  nanobind_add_stub(
    _ext_stub
    MODULE _ext
    OUTPUT _ext.pyi
    PYTHON_PATH $<TARGET_FILE_DIR:_ext>
    DEPENDS _ext)

  install(TARGETS _ext LIBRARY DESTINATION ds_service_client COMPONENT python)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/_ext.pyi
          DESTINATION ds_service_client COMPONENT python)

  if(DS_SERVICE_PYTHON_INPLACE)
    set(_ds_service_package_dir ${CMAKE_CURRENT_SOURCE_DIR}/python/ds_service_client)
    add_custom_command(
      TARGET _ext
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:_ext>
              ${_ds_service_package_dir}/
      VERBATIM)
    add_custom_command(
      TARGET _ext_stub
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/_ext.pyi
              ${_ds_service_package_dir}/
      VERBATIM)
  endif()
endif()
