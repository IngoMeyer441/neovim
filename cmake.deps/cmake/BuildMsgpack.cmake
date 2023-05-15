ExternalProject_Add(msgpack
  URL ${MSGPACK_URL}
  URL_HASH SHA256=${MSGPACK_SHA256}
  DOWNLOAD_NO_PROGRESS TRUE
  DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}/msgpack
  CMAKE_ARGS ${DEPS_CMAKE_ARGS}
    -D MSGPACK_BUILD_TESTS=OFF
    -D MSGPACK_BUILD_EXAMPLES=OFF
  CMAKE_CACHE_ARGS ${DEPS_CMAKE_CACHE_ARGS})
