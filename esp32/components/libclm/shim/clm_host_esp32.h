// SPDX-License-Identifier: ISC
/*
 * ESP-IDF clm_host: a blocking esp_http_client transport with SSE streaming and
 * no timers. This is the embedded counterpart to the desktop libuv+curl host
 * (clm/host_uv.h); the firmware creates one and hands it to clm_agent_new.
 */
#ifndef CLM_HOST_ESP32_H
#define CLM_HOST_ESP32_H

#include "clm/host.h"

/*
 * Allocate a clm_host backed by esp_http_client. Requests run synchronously on
 * the calling task: http_post blocks, streaming body chunks to data_cb as they
 * arrive, then fires success/error before returning. timer_set is left NULL, so
 * the core disables per-tool timeouts. Returns 0 and stores the host in *out, or
 * a negative errno. Free with clm_host_esp32_free.
 */
int clm_host_esp32_new(struct clm_host **out);

void clm_host_esp32_free(struct clm_host *host);

#endif /* CLM_HOST_ESP32_H */
