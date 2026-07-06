# libclm core as an ESP-IDF component.
#
# Builds the portable core of libclm (OpenAI-compatible client + tool-calling
# agent loop) against the vendored json-c and a synchronous esp_http_client
# transport that implements the clm_host interface. Excluded from the desktop
# tree -- these are the libuv/libcurl layer (libclmuv), replaced by the ESP-IDF
# host in src/clm_host_esp32.c:
#   - lua_*.c       (plugin system: not ported)
#   - http_async.c  (libcurl+libuv async engine)
#   - host_uv.c     (libuv/curl clm_host adapter)
#   - tool_shell.c  (shell_exec via uv_spawn -- no subprocesses on-device)
#   - mcp_client.c  (MCP client: built on http_async's uv-based engine, not
#                    ported)
set(CLM_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

file(GLOB CLM_CORE_SRCS "${CLM_ROOT}/lib/*.c")
list(FILTER CLM_CORE_SRCS EXCLUDE REGEX "/lua_[^/]*\\.c$")
list(FILTER CLM_CORE_SRCS EXCLUDE REGEX "/http_async\\.c$")
list(FILTER CLM_CORE_SRCS EXCLUDE REGEX "/host_uv\\.c$")
list(FILTER CLM_CORE_SRCS EXCLUDE REGEX "/tool_shell\\.c$")
list(FILTER CLM_CORE_SRCS EXCLUDE REGEX "/mcp_client\\.c$")

idf_component_register(
    SRCS ${CLM_CORE_SRCS}
         "src/clm_host_esp32.c"
         "src/compat_esp32.c"
    INCLUDE_DIRS "${CLM_ROOT}/include" "shim"
    PRIV_INCLUDE_DIRS "${CLM_ROOT}" "${CLM_ROOT}/lib"
    REQUIRES json-c esp_http_client esp-tls mbedtls
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
