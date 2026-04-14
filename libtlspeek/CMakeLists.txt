cmake_minimum_required(VERSION 3.16)
project(libtlspeek C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -g")

# ─── wolfSSL ───────────────────────────────────────────────────────────────────
# Use a wolfSSL build that contains the custom migration symbols.
# Default to the wolfssl/ directory adjacent to this repo, but allow overrides.
set(WOLFSSL_ROOT "${CMAKE_SOURCE_DIR}/../wolfssl" CACHE PATH "Path to wolfSSL source tree")
set(WOLFSSL_INCLUDE "${WOLFSSL_ROOT}" CACHE PATH "Path to wolfSSL headers")
set(WOLFSSL_LIB "${WOLFSSL_ROOT}/src/.libs/libwolfssl.so" CACHE FILEPATH "Path to libwolfssl shared library")

if(NOT EXISTS "${WOLFSSL_LIB}")
    message(FATAL_ERROR "WOLFSSL_LIB not found at: ${WOLFSSL_LIB}. Build wolfSSL first (see build_pi.sh), or set -DWOLFSSL_LIB=...")
endif()

if(NOT EXISTS "${WOLFSSL_INCLUDE}/wolfssl/ssl.h")
    message(FATAL_ERROR "WOLFSSL_INCLUDE does not look like a wolfSSL source tree: ${WOLFSSL_INCLUDE}. Set -DWOLFSSL_INCLUDE=...")
endif()

include_directories(${WOLFSSL_INCLUDE})

# Ensure the linker adds the correct directory to the RPATH for runtime loading
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "/usr/local/lib")

# Include our own header directories
include_directories(${CMAKE_SOURCE_DIR}/lib)
include_directories(${CMAKE_SOURCE_DIR}/common)

# Point to local wolfSSL source if find_path didn't find the custom fork headers
# include_directories(/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl)

# ─── Core library ──────────────────────────────────────────────────────────────
add_library(tlspeek STATIC
    lib/tlspeek.c
    lib/tlspeek_crypto.c
    lib/tlspeek_serial.c
    common/sendfd.c
    common/unix_socket.c
)
target_link_libraries(tlspeek ${WOLFSSL_LIB})

# ─── Gateway binary ────────────────────────────────────────────────────────────
add_executable(gateway
    gateway/gateway.c
    gateway/router.c
)
target_include_directories(gateway PRIVATE
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/gateway
)
target_link_libraries(gateway tlspeek ${WOLFSSL_LIB} pthread)

# ─── Worker binary ─────────────────────────────────────────────────────────────
add_executable(worker
    worker/worker.c
    worker/handler.c
)
target_include_directories(worker PRIVATE
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/worker
)
target_link_libraries(worker tlspeek ${WOLFSSL_LIB} pthread)

# ─── Unit tests ────────────────────────────────────────────────────────────────
add_executable(test_tlspeek test/test_tlspeek.c)
target_include_directories(test_tlspeek PRIVATE
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(test_tlspeek tlspeek ${WOLFSSL_LIB})
