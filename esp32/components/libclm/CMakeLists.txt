# libclm core as an ESP-IDF component.
#
# Builds the portable core of libclm (OpenAI-compatible client + tool-calling
# agent loop) against ESP-IDF's built-in cJSON ("json" component, providing
# <cJSON.h> -- the same header clm's own code includes on the desktop side, so
# no shim/rename is needed here) and a synchronous esp_http_client transport
# that implements the clm_host interface. The desktop tree's Lua plugin
# support (libclmlua/) and libuv/libcurl host layer (libclmuv/) now live in
# their own directories entirely (see the top-level meson.build split, one
# lib per directory) -- so unlike the old single lib/ tree, there's no
# filtering needed here at all: every libclm/*.c source is core, replaced
# on-device by src/clm_host_esp32.c for transport.
set(CLM_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

file(GLOB CLM_CORE_SRCS "${CLM_ROOT}/libclm/*.c")

idf_component_register(
    SRCS ${CLM_CORE_SRCS}
         "src/clm_host_esp32.c"
         "src/compat_esp32.c"
    INCLUDE_DIRS "${CLM_ROOT}/include" "shim"
    PRIV_INCLUDE_DIRS "${CLM_ROOT}" "${CLM_ROOT}/libclm"
    REQUIRES json esp_http_client esp-tls mbedtls
)

# The core no longer references libuv/libcurl (transport + shell live in the
# excluded files above), so no CLM_NO_SUBPROCESS flag is needed anymore.
# _GNU_SOURCE exposes getline/asprintf/strdup in newlib's stdio/string headers.
target_compile_definitions(${COMPONENT_LIB} PRIVATE _GNU_SOURCE)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    # Force-include the compat prototypes (getline/getdelim) into every source.
    "-include${CMAKE_CURRENT_LIST_DIR}/shim/clm_compat.h"
)
