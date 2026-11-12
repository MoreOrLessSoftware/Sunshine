# unix specific compile definitions
# put anything here that applies to both linux and macos

# zlib extracts plugins imported from a URL
find_package(ZLIB REQUIRED)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${CURL_LIBRARIES}
        ZLIB::ZLIB)

# add install prefix to assets path if not already there
if(NOT APPLE AND NOT SUNSHINE_ASSETS_DIR MATCHES "^${CMAKE_INSTALL_PREFIX}")
    set(SUNSHINE_ASSETS_DIR "${CMAKE_INSTALL_PREFIX}/${SUNSHINE_ASSETS_DIR}")
endif()
