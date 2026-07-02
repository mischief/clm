// SPDX-License-Identifier: ISC
/*
 * ESP32 demo firmware for the libclm core: connects to WiFi, points the agent
 * at an OpenAI-compatible endpoint (e.g. llama.cpp server), registers a native
 * tool, and runs one tool-calling turn synchronously.
 *
 * Copy clm_config.h.example to clm_config.h and fill in WiFi + endpoint.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/gpio.h"

static const char *TAG = "clm-app";

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm_host_esp32.h"
#include "board_config.h"
#include "board.h"
#include "display.h"

#include "clm_config.h"

/* ------------------------------------------------------------------ */
/* WiFi station bring-up                                               */
/* ------------------------------------------------------------------ */

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void
wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		esp_wifi_connect();
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *e = data;
		ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
		xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
	}
}

static void
wifi_init(void)
{
	s_wifi_events = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
	    wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
	    wifi_event_handler, NULL));

	wifi_config_t wc = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASS,
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "connecting to %s ...", WIFI_SSID);
	xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
	    portMAX_DELAY);
}

/* ------------------------------------------------------------------ */
/* A native demo tool                                                  */
/* ------------------------------------------------------------------ */

static void
tool_device_info(struct clm_tool_invocation *inv, void *user)
{
	(void)user;
	esp_chip_info_t info;
	char out[160];

	esp_chip_info(&info);
	(void)snprintf(out, sizeof(out),
	    "{\"chip\":\"ESP32-S3\",\"cores\":%d,\"revision\":%d,"
	    "\"total_heap\":%u,\"free_heap\":%u,\"min_free_heap\":%u}",
	    info.cores, info.revision,
	    (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
	    (unsigned)esp_get_free_heap_size(),
	    (unsigned)esp_get_minimum_free_heap_size());
	clm_tool_complete(inv, out);
}

/* ------------------------------------------------------------------ */
/* Line input: the T-Deck keyboard, with the USB serial line as a       */
/* fallback (handy for scripted testing over /dev/ttyACM).              */
/* ------------------------------------------------------------------ */

static void
console_init(void)
{
	usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
	usb_serial_jtag_driver_install(&cfg);
	usb_serial_jtag_vfs_use_driver();
}

/* Non-blocking: return a pressed key from the keyboard or serial, else 0. */
static uint8_t
input_poll(void)
{
	uint8_t c = keyboard_read();
	if (c != 0)
		return c;
	if (usb_serial_jtag_read_bytes(&c, 1, 0) > 0)
		return c;
	return 0;
}

/* Read one line, echoing to the display (green) and the serial log. Returns
 * the line length (excluding the terminator). */
static int
console_read_line(char *buf, size_t max)
{
	size_t len = 0;
	console_set_color(DISP_GREEN);
	console_puts("> ");
	for (;;) {
		uint8_t c = input_poll();
		if (c == 0) {
			vTaskDelay(pdMS_TO_TICKS(15));
			continue;
		}
		if (c == '\r' || c == '\n') {
			printf("\r\n");
			fflush(stdout);
			console_putc('\n');
			console_flush();
			buf[len] = '\0';
			return (int)len;
		}
		if (c == 0x08 || c == 0x7f) { /* backspace / delete */
			if (len > 0) {
				len--;
				printf("\b \b");
				fflush(stdout);
				console_backspace();
			}
			continue;
		}
		if (c >= 0x20 && len + 1 < max) {
			buf[len++] = (char)c;
			putchar(c);
			fflush(stdout);
			console_set_color(DISP_GREEN);
			console_putc((char)c);
			console_flush();
		}
	}
}

/* ------------------------------------------------------------------ */
/* Agent callbacks                                                     */
/* ------------------------------------------------------------------ */

static volatile int s_turn_done;

static void
on_assistant_text(const char *text, void *user)
{
	(void)user;
	printf("%s", text);
	fflush(stdout);
	console_set_color(DISP_WHITE);
	console_puts(text);
}

static void
on_reasoning(const char *text, void *user)
{
	(void)user;
	printf("%s", text);
	fflush(stdout);
	console_set_color(DISP_GRAY); /* dim: chain-of-thought, not the answer */
	console_puts(text);
}

static void
on_tool_begin(const char *name, const char *args, void *user)
{
	(void)user;
	ESP_LOGI(TAG, "tool call: %s %s", name, args ? args : "");
	console_set_color(DISP_YELLOW);
	console_puts("[tool: ");
	console_puts(name);
	console_puts("]\n");
}

static void
on_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	(void)user;
	ESP_LOGI(TAG, "tool %s -> [%d] %s", name, (int)outcome,
	    content ? content : "");
}

static struct clm_agent *s_agent; /* for on_permission to respond */

static void
on_permission(const struct clm_permission_req *req, void *user)
{
	(void)user;
	/* Single-user device: auto-approve gated tools. */
	clm_tool_permission_respond(s_agent, req, CLM_PERM_ALLOW_ALWAYS);
}

static void
on_turn_done(int status, void *user)
{
	(void)user;
	printf("\n");
	console_putc('\n');
	ESP_LOGI(TAG, "turn done (status=%d) heap: %u free / %u total, %u min free",
	    status, (unsigned)esp_get_free_heap_size(),
	    (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
	    (unsigned)esp_get_minimum_free_heap_size());
	s_turn_done = 1;
}

/* ------------------------------------------------------------------ */
/* Agent task                                                          */
/* ------------------------------------------------------------------ */

static void
agent_task(void *arg)
{
	(void)arg;
	struct clm_agent *agent = NULL;
	struct clm_host *host = NULL;

	struct clm_cfg cfg = {
		.api_key = CLM_API_KEY,
		.base_url = CLM_BASE_URL,
		.provider = CLM_PROVIDER_OPENAI,
		.model = CLM_MODEL,
		.max_iterations = 5,
		.stream = true, /* SSE: the esp32 host streams body chunks to data_cb */
		.system_prompt = NULL,
	};

	struct clm_callbacks cb = {
		.on_assistant_text = on_assistant_text,
		.on_reasoning = on_reasoning,
		.on_tool_begin = on_tool_begin,
		.on_tool_result = on_tool_result,
		.on_permission = on_permission,
		.on_turn_done = on_turn_done,
	};

	int r = clm_host_esp32_new(&host);
	if (r < 0 || host == NULL) {
		ESP_LOGE(TAG, "clm_host_esp32_new failed: %d", r);
		vTaskDelete(NULL);
		return;
	}

	r = clm_agent_new(&cfg, host, &cb, NULL, &agent);
	if (r < 0 || agent == NULL) {
		ESP_LOGE(TAG, "clm_agent_new failed: %d", r);
		clm_host_esp32_free(host);
		vTaskDelete(NULL);
		return;
	}
	s_agent = agent;

	clm_tools_register_builtins(agent);

	struct clm_tool_def dev = {
		.name = "device_info",
		.description = "Return this device's chip and memory information",
		.params_schema = "{\"type\":\"object\",\"properties\":{}}",
		.invoke = tool_device_info,
		.flags = CLM_TOOL_NO_PROMPT,
	};
	clm_tool_add(agent, &dev);

	/* Interactive chat REPL over the serial console. The agent keeps its
	 * conversation history across turns, so this is a real multi-turn chat.
	 * Type '/quit' to reset the session. */
	console_init();
	printf("\n=== clm serial chat ===\n");
	printf("model: %s @ %s\n", CLM_MODEL, CLM_BASE_URL);
	printf("type a message and press enter. '/quit' resets the session.\n\n");

	static char line[1024];
	for (;;) {
		printf("you> ");
		fflush(stdout);
		int n = console_read_line(line, sizeof(line));
		if (n <= 0)
			continue;

		if (strcmp(line, "/shot") == 0) {
			console_screenshot();
			continue;
		}

		if (strcmp(line, "/quit") == 0 || strcmp(line, "/reset") == 0) {
			clm_agent_free(agent);
			agent = NULL;
			s_agent = NULL;
			if (clm_agent_new(&cfg, host, &cb, NULL, &agent) < 0) {
				ESP_LOGE(TAG, "re-init failed");
				break;
			}
			s_agent = agent;
			clm_tools_register_builtins(agent);
			clm_tool_add(agent, &dev);
			printf("(session reset)\n\n");
			continue;
		}

		printf("clm> ");
		fflush(stdout);
		s_turn_done = 0;
		r = clm_agent_submit(agent, line);
		if (r < 0)
			printf("[submit failed: %d]\n", r);

		printf("\n");
	}

	clm_agent_free(agent);
	clm_host_esp32_free(host);
	vTaskDelete(NULL);
}

void
app_main(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}

	/* Board bring-up (power/SPI/SD/keyboard), then the display on that bus. */
	board_init();
	if (display_init() == 0) {
		console_set_color(DISP_CYAN);
		console_puts("clm on " BOARD_NAME "\n");
		console_set_color(DISP_GRAY);
		console_puts("connecting wifi...\n");
	}

	wifi_init();

	/* Large stack: the port drives the agent (HTTP + tool loop) synchronously
	 * and recursively across turns. */
	xTaskCreate(agent_task, "clm", 32768, NULL, 5, NULL);
}
