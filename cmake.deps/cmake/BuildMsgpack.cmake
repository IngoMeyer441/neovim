# BuildMsgpack(CONFIGURE_COMMAND ... BUILD_COMMAND ... INSTALL_COMMAND ...)
# Reusable function to build msgpack, wraps ExternalProject_Add.
# Failing to pass a command argument will result in no command being run
function(BuildMsgpack)
  cmake_parse_arguments(_msgpack
    ""
    ""
    "CONFIGURE_COMMAND;BUILD_COMMAND;INSTALL_COMMAND"
    ${ARGN})

  if(NOT _msgpack_CONFIGURE_COMMAND AND NOT _msgpack_BUILD_COMMAND
       AND NOT _msgpack_INSTALL_COMMAND)
    message(FATAL_ERROR "Must pass at least one of CONFIGURE_COMMAND, BUILD_COMMAND, INSTALL_COMMAND")
  endif()

  ExternalProject_Add(msgpack
    PREFIX ${DEPS_BUILD_DIR}
    URL ${MSGPACK_URL}
    DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}/msgpack
    DOWNLOAD_COMMAND ${CMAKE_COMMAND}
      -DPREFIX=${DEPS_BUILD_DIR}
      -DDOWNLOAD_DIR=${DEPS_DOWNLOAD_DIR}/msgpack
      -DURL=${MSGPACK_URL}
      -DEXPECTED_SHA256=${MSGPACK_SHA256}
      -DTARGET=msgpack
      -DUSE_EXISTING_SRC_DIR=${USE_EXISTING_SRC_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadAndExtractFile.cmake
    CONFIGURE_COMMAND "${_msgpack_CONFIGURE_COMMAND}"
    BUILD_COMMAND "${_msgpack_BUILD_COMMAND}"
    INSTALL_COMMAND "${_msgpack_INSTALL_COMMAND}"
    LIST_SEPARATOR |)
endfunction()

set(MSGPACK_CONFIGURE_COMMAND ${CMAKE_COMMAND} ${DEPS_BUILD_DIR}/src/msgpack
  -DMSGPACK_BUILD_TESTS=OFF
  -DMSGPACK_BUILD_EXAMPLES=OFF
  -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES_ALT_SEP}
  "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_COMPILER_ARG1} -fPIC"
  -DCMAKE_GENERATOR=${CMAKE_GENERATOR})

set(MSGPACK_BUILD_COMMAND ${CMAKE_COMMAND} --build . --config ${CMAKE_BUILD_TYPE})
set(MSGPACK_INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config ${CMAKE_BUILD_TYPE})

if(MSVC)
  # Same as Unix without fPIC
  set(MSGPACK_CONFIGURE_COMMAND ${CMAKE_COMMAND} ${DEPS_BUILD_DIR}/src/msgpack
    -DMSGPACK_BUILD_TESTS=OFF
    -DMSGPACK_BUILD_EXAMPLES=OFF
    -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
    ${BUILD_TYPE_STRING}
    "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_COMPILER_ARG1}"
    # Make sure we use the same generator, otherwise we may
    # accidentally end up using different MSVC runtimes
    -DCMAKE_GENERATOR=${CMAKE_GENERATOR})
endif()

BuildMsgpack(CONFIGURE_COMMAND ${MSGPACK_CONFIGURE_COMMAND}
  BUILD_COMMAND ${MSGPACK_BUILD_COMMAND}
  INSTALL_COMMAND ${MSGPACK_INSTALL_COMMAND})

list(APPEND THIRD_PARTY_DEPS msgpack)
