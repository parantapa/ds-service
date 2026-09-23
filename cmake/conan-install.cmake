# Run conan install at configure time, and build with the toolchain it generates.
#
# A build that starts from pip goes through scikit-build-core,
# which runs CMake with no Conan toolchain,
# so pyproject.toml sets DS_SERVICE_CONAN_INSTALL.
# Every other build runs conan install itself
# and passes the toolchain on the command line,
# as docs/how-to-guides/build-the-server.md describes,
# and this file then does nothing.
#
# CMakeLists.txt includes this file before project(),
# because the toolchain file must be set before project() reads it.
# The DS_SERVICE_BUILD_* options are not declared yet at that point,
# so this file reads the values given with -D,
# and passes each one to Conan as the matching with_* option.
#
# See "Pip builds run conan install from CMake" in docs/developer-notes.md.

if(NOT DS_SERVICE_CONAN_INSTALL OR CMAKE_TOOLCHAIN_FILE)
  return()
endif()

find_program(DS_SERVICE_CONAN_EXE conan REQUIRED)

set(_ds_service_conan_options)
foreach(_ds_service_part SERVER CLIENT PYTHON)
  if(DEFINED DS_SERVICE_BUILD_${_ds_service_part})
    string(TOLOWER "${_ds_service_part}" _ds_service_name)
    if(DS_SERVICE_BUILD_${_ds_service_part})
      set(_ds_service_value True)
    else()
      set(_ds_service_value False)
    endif()
    list(APPEND _ds_service_conan_options -o
         "&:with_${_ds_service_name}=${_ds_service_value}")
  endif()
endforeach()

set(_ds_service_conan_dir "${CMAKE_BINARY_DIR}/conan")

# A fresh machine, such as a wheel build container, has no default profile.
execute_process(COMMAND "${DS_SERVICE_CONAN_EXE}" profile detect --exist-ok
                        OUTPUT_QUIET ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)

# user_presets is emptied so that Conan leaves CMakeUserPresets.json
# in the source tree alone.
# build_type is Release whatever CMAKE_BUILD_TYPE is,
# because the toolchain path set below names the Release layout.
execute_process(
  COMMAND
    "${DS_SERVICE_CONAN_EXE}" install "${CMAKE_SOURCE_DIR}" --build=missing
    "--output-folder=${_ds_service_conan_dir}" -s build_type=Release -c
    tools.cmake.cmaketoolchain:user_presets= ${_ds_service_conan_options}
    COMMAND_ERROR_IS_FATAL ANY)

set(CMAKE_TOOLCHAIN_FILE
    "${_ds_service_conan_dir}/build/Release/generators/conan_toolchain.cmake")
