#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"

#if defined(CONFIG_TINYUSB_NET_MODE_NCM) || defined(CONFIG_TINYUSB_NET_MODE_ECM_RNDIS)
#define USB_NCM_ENABLED 1
#else
#define USB_NCM_ENABLED 0
#endif

#if USB_NCM_ENABLED
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#endif

#include "lwip/esp_netif_net_stack.h"
#include "dhcpserver/dhcpserver_options.h"

#include "led_strip.h"

static const char *TAG = "ESPARTNET";

#define ARTNET_PORT                 6454
#define ARTNET_DMX_OPCODE           0x5000
#define ARTNET_HEADER_LEN           18

#define ARTNET_DEBUG_PRESET_SILENT  0
#define ARTNET_DEBUG_PRESET_NORMAL  1
#define ARTNET_DEBUG_PRESET_VERBOSE 2
#define ARTNET_DEBUG_PRESET         ARTNET_DEBUG_PRESET_NORMAL

#if ARTNET_DEBUG_PRESET == ARTNET_DEBUG_PRESET_SILENT
#define ARTNET_ENABLE_STATS_LOG      0
#define ARTNET_ENABLE_DMX_SAMPLE_LOG 0
#elif ARTNET_DEBUG_PRESET == ARTNET_DEBUG_PRESET_NORMAL
#define ARTNET_ENABLE_STATS_LOG      1
#define ARTNET_ENABLE_DMX_SAMPLE_LOG 0
#elif ARTNET_DEBUG_PRESET == ARTNET_DEBUG_PRESET_VERBOSE
#define ARTNET_ENABLE_STATS_LOG      1
#define ARTNET_ENABLE_DMX_SAMPLE_LOG 1
#else
#error "Invalid ARTNET_DEBUG_PRESET"
#endif

#define ARTNET_STATS_LOG_INTERVAL_MS      1000
#define ARTNET_DMX_SAMPLE_LOG_INTERVAL_MS 200

#define OUTPUT_LED_COUNT            12
#define OUTPUT_LED_MODEL            LED_MODEL_WS2812
#define OUTPUT_LED_COLOR_FORMAT     LED_STRIP_COLOR_COMPONENT_FMT_GRB
#define OUTPUT_LED_SIGNAL_INVERT    0
#define OUTPUT_LED_SELFTEST_MS      350
#define LED_REFRESH_MS              20

#define STATUS_LED_ENABLED          1
#define STATUS_LED_COUNT            1
#define STATUS_LED_BRIGHTNESS       16

#if CONFIG_IDF_TARGET_ESP32S2
#define APP_TARGET_PROFILE_ESP32S2  1
#define APP_TARGET_PROFILE_NAME      "esp32s2"
#define OUTPUT_LED_STRIP_GPIO        16
#define STATUS_LED_GPIO              15
#define STATUS_LED_MONO_MODE         1
#define STATUS_LED_USE_GPIO          1
#elif CONFIG_IDF_TARGET_ESP32S3
#define APP_TARGET_PROFILE_ESP32S3  1
#define APP_TARGET_PROFILE_NAME      "esp32s3"
#define OUTPUT_LED_STRIP_GPIO        18
#define STATUS_LED_GPIO              48
#define STATUS_LED_MONO_MODE         0
#define STATUS_LED_USE_GPIO          0
#else
#error "Unsupported target: only esp32s2/esp32s3 profiles are defined"
#endif

#define ARTNET_START_UNIVERSE       0
#define CHANNELS_PER_UNIVERSE       512
#define TOTAL_CHANNELS              (OUTPUT_LED_COUNT * 3)
#define UNIVERSE_COUNT              ((TOTAL_CHANNELS + CHANNELS_PER_UNIVERSE - 1) / CHANNELS_PER_UNIVERSE)

#define IDLE_MODE_ENABLED            1
#define IDLE_MODE_TIMEOUT_MS         (30 * 1000)
#define IDLE_MODE_STEP_INTERVAL_MS   50
#define IDLE_MODE_BRIGHTNESS         40

#if USB_NCM_ENABLED
static esp_netif_t *s_usb_netif;
#endif
static led_strip_handle_t s_output_led_strip;
#if STATUS_LED_ENABLED
#if !STATUS_LED_USE_GPIO
static led_strip_handle_t s_status_led_strip;
#endif
#endif
static SemaphoreHandle_t s_led_mutex;

static uint8_t s_dmx_buffer[TOTAL_CHANNELS];
static volatile bool s_frame_dirty;
static volatile bool s_idle_mode_armed;
static volatile bool s_last_frame_was_off;
static volatile bool s_idle_mode_force_start;

typedef struct {
	uint32_t rx_total;
	uint32_t rx_valid_total;
	uint32_t rx_in_range_total;
	uint32_t rx_total_1s;
	uint32_t rx_valid_1s;
	uint32_t rx_in_range_1s;
	uint16_t last_universe;
	uint16_t last_dmx_len;
} artnet_stats_t;

static artnet_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile TickType_t s_last_mapped_packet_tick;
static volatile TickType_t s_idle_mode_armed_tick;
#if ARTNET_ENABLE_DMX_SAMPLE_LOG
static TickType_t s_last_dmx_debug_log_tick;
#endif

#if USB_NCM_ENABLED
static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
	(void)h;
	if (tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100)) != ESP_OK) {
		ESP_LOGW(TAG, "tinyusb_net_send_sync failed");
	}
	return ESP_OK;
}

static void netif_free_rx_buffer(void *h, void *buffer)
{
	(void)h;
	free(buffer);
}

static esp_err_t usb_net_rx_callback(void *buffer, uint16_t len, void *ctx)
{
	(void)ctx;
	if (s_usb_netif == NULL) {
		return ESP_OK;
	}

	void *copy = malloc(len);
	if (copy == NULL) {
		return ESP_ERR_NO_MEM;
	}

	memcpy(copy, buffer, len);
	return esp_netif_receive(s_usb_netif, copy, len, NULL);
}

static esp_err_t init_usb_ncm_network(void)
{
	const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
	ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install failed");

	const tinyusb_net_config_t net_cfg = {
		.mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
		.on_recv_callback = usb_net_rx_callback,
	};
	ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_cfg), TAG, "tinyusb_net_init failed");

	uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

	esp_netif_inherent_config_t base_cfg = {
		.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
		.ip_info = &_g_esp_netif_soft_ap_ip,
		.if_key = "usb",
		.if_desc = "usb ncm artnet",
		.route_prio = 10,
	};

	esp_netif_driver_ifconfig_t driver_cfg = {
		.handle = (void *)1,
		.transmit = netif_transmit,
		.driver_free_rx_buffer = netif_free_rx_buffer,
	};

	struct esp_netif_netstack_config lwip_cfg = {
		.lwip = {
			.init_fn = ethernetif_init,
			.input_fn = ethernetif_input,
		}
	};

	esp_netif_config_t cfg = {
		.base = &base_cfg,
		.driver = &driver_cfg,
		.stack = &lwip_cfg,
	};

	s_usb_netif = esp_netif_new(&cfg);
	ESP_RETURN_ON_FALSE(s_usb_netif != NULL, ESP_FAIL, TAG, "esp_netif_new failed");

	ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_usb_netif, lwip_mac), TAG, "esp_netif_set_mac failed");

	uint32_t lease_time_minutes = 120;
	esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease_time_minutes, sizeof(lease_time_minutes));

	esp_netif_action_start(s_usb_netif, NULL, 0, NULL);

	esp_netif_ip_info_t ip_info;
	ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_usb_netif, &ip_info), TAG, "esp_netif_get_ip_info failed");
	ESP_LOGI(TAG, "USB-NCM ready IP:" IPSTR " GW:" IPSTR " MASK:" IPSTR,
			 IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));

	return ESP_OK;
}
#else
static esp_err_t init_usb_ncm_network(void)
{
	ESP_LOGW(TAG, "USB-NCM is not enabled for target %s", APP_TARGET_PROFILE_NAME);
	return ESP_OK;
}
#endif

static esp_err_t init_output_led_strip(void)
{
	led_strip_config_t strip_cfg = {
		.strip_gpio_num = OUTPUT_LED_STRIP_GPIO,
		.max_leds = OUTPUT_LED_COUNT,
		.led_model = OUTPUT_LED_MODEL,
		.color_component_format = OUTPUT_LED_COLOR_FORMAT,
		.flags.invert_out = OUTPUT_LED_SIGNAL_INVERT,
	};
	led_strip_rmt_config_t rmt_cfg = {
		.resolution_hz = 10 * 1000 * 1000,
		.flags.with_dma = false,
	};

	ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_output_led_strip), TAG, "output led_strip init failed");
	ESP_RETURN_ON_ERROR(gpio_set_drive_capability(OUTPUT_LED_STRIP_GPIO, GPIO_DRIVE_CAP_3), TAG, "set output drive strength failed");
	ESP_RETURN_ON_ERROR(led_strip_clear(s_output_led_strip), TAG, "output led_strip clear failed");
	return ESP_OK;
}

static void run_output_led_selftest(void)
{
	const uint8_t test_colors[][3] = {
		{32, 0, 0},
		{0, 32, 0},
		{0, 0, 32},
		{32, 32, 32},
		{0, 0, 0},
	};

	for (size_t c = 0; c < sizeof(test_colors) / sizeof(test_colors[0]); c++) {
		for (size_t i = 0; i < OUTPUT_LED_COUNT; i++) {
			led_strip_set_pixel(s_output_led_strip, i, test_colors[c][0], test_colors[c][1], test_colors[c][2]);
		}
		led_strip_refresh(s_output_led_strip);
		vTaskDelay(pdMS_TO_TICKS(OUTPUT_LED_SELFTEST_MS));
	}

	for (size_t i = 0; i < OUTPUT_LED_COUNT; i++) {
		for (size_t j = 0; j < OUTPUT_LED_COUNT; j++) {
			if (j == i) {
				led_strip_set_pixel(s_output_led_strip, j, 32, 32, 32);
			} else {
				led_strip_set_pixel(s_output_led_strip, j, 0, 0, 0);
			}
		}
		led_strip_refresh(s_output_led_strip);
		vTaskDelay(pdMS_TO_TICKS(OUTPUT_LED_SELFTEST_MS));
	}

	led_strip_clear(s_output_led_strip);
}

#if STATUS_LED_ENABLED
static esp_err_t init_status_led_strip(void)
{
	#if STATUS_LED_USE_GPIO
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << STATUS_LED_GPIO),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "status led gpio config failed");
	ESP_RETURN_ON_ERROR(gpio_set_level(STATUS_LED_GPIO, 0), TAG, "status led gpio init level failed");
	return ESP_OK;
	#else
	led_strip_config_t strip_cfg = {
		.strip_gpio_num = STATUS_LED_GPIO,
		.max_leds = STATUS_LED_COUNT,
		.led_model = LED_MODEL_WS2812,
		.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
	};
	led_strip_rmt_config_t rmt_cfg = {
		.resolution_hz = 10 * 1000 * 1000,
		.flags.with_dma = false,
	};

	ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_status_led_strip), TAG, "status led_strip init failed");
	ESP_RETURN_ON_ERROR(led_strip_clear(s_status_led_strip), TAG, "status led_strip clear failed");
	return ESP_OK;
	#endif
}
#endif

static bool parse_artnet_dmx(const uint8_t *packet, ssize_t len, uint16_t *universe, const uint8_t **dmx_data, uint16_t *dmx_len)
{
	if (len < ARTNET_HEADER_LEN) {
		return false;
	}

	if (memcmp(packet, "Art-Net\0", 8) != 0) {
		return false;
	}

	uint16_t opcode = (uint16_t)packet[8] | ((uint16_t)packet[9] << 8);
	if (opcode != ARTNET_DMX_OPCODE) {
		return false;
	}

	uint16_t proto_ver = ((uint16_t)packet[10] << 8) | packet[11];
	if (proto_ver < 14) {
		return false;
	}

	uint8_t subuni = packet[14];
	uint8_t net = packet[15];
	*universe = ((uint16_t)net << 8) | subuni;

	*dmx_len = ((uint16_t)packet[16] << 8) | packet[17];
	if (*dmx_len > 512) {
		return false;
	}

	if ((ssize_t)(ARTNET_HEADER_LEN + *dmx_len) > len) {
		return false;
	}

	*dmx_data = &packet[ARTNET_HEADER_LEN];
	return true;
}

static void apply_artnet_dmx_to_frame(uint16_t universe, const uint8_t *dmx, uint16_t dmx_len)
{
	if (universe >= (ARTNET_START_UNIVERSE + UNIVERSE_COUNT)) {
		return;
	}

	size_t universe_index = universe - ARTNET_START_UNIVERSE;
	size_t offset = universe_index * CHANNELS_PER_UNIVERSE;
	if (offset >= TOTAL_CHANNELS) {
		return;
	}

	size_t bytes_to_copy = dmx_len;
	if (offset + bytes_to_copy > TOTAL_CHANNELS) {
		bytes_to_copy = TOTAL_CHANNELS - offset;
	}

	memcpy(&s_dmx_buffer[offset], dmx, bytes_to_copy);

	size_t universe_capacity = CHANNELS_PER_UNIVERSE;
	if (offset + universe_capacity > TOTAL_CHANNELS) {
		universe_capacity = TOTAL_CHANNELS - offset;
	}

	if (bytes_to_copy < universe_capacity) {
		memset(&s_dmx_buffer[offset + bytes_to_copy], 0, universe_capacity - bytes_to_copy);
	}

	s_frame_dirty = true;
}

static bool is_frame_all_off(void)
{
	for (size_t i = 0; i < TOTAL_CHANNELS; i++) {
		if (s_dmx_buffer[i] != 0) {
			return false;
		}
	}
	return true;
}

#if IDLE_MODE_ENABLED
static void color_wheel(uint8_t wheel_pos, uint8_t *red, uint8_t *green, uint8_t *blue)
{
	wheel_pos = 255 - wheel_pos;
	if (wheel_pos < 85) {
		*red = 255 - wheel_pos * 3;
		*green = 0;
		*blue = wheel_pos * 3;
		return;
	}

	if (wheel_pos < 170) {
		wheel_pos -= 85;
		*red = 0;
		*green = wheel_pos * 3;
		*blue = 255 - wheel_pos * 3;
		return;
	}

	wheel_pos -= 170;
	*red = wheel_pos * 3;
	*green = 255 - wheel_pos * 3;
	*blue = 0;
}

static void render_idle_gradient(uint8_t phase)
{
	for (size_t index = 0; index < OUTPUT_LED_COUNT; index++) {
		uint8_t position = (uint8_t)(((index * 256U) / OUTPUT_LED_COUNT + phase) & 0xFF);
		uint8_t red = 0;
		uint8_t green = 0;
		uint8_t blue = 0;

		color_wheel(position, &red, &green, &blue);
		red = (uint8_t)((red * IDLE_MODE_BRIGHTNESS) / 255U);
		green = (uint8_t)((green * IDLE_MODE_BRIGHTNESS) / 255U);
		blue = (uint8_t)((blue * IDLE_MODE_BRIGHTNESS) / 255U);

		led_strip_set_pixel(s_output_led_strip, index, red, green, blue);
	}
}
#endif

static void artnet_stats_task(void *arg)
{
	(void)arg;

	while (1) {
		vTaskDelay(pdMS_TO_TICKS(ARTNET_STATS_LOG_INTERVAL_MS));

		uint32_t rx_total = 0;
		uint32_t rx_valid_total = 0;
		uint32_t rx_in_range_total = 0;
		uint32_t rx_total_1s = 0;
		uint32_t rx_valid_1s = 0;
		uint32_t rx_in_range_1s = 0;
		uint16_t last_universe = 0;
		uint16_t last_dmx_len = 0;

		portENTER_CRITICAL(&s_stats_lock);
		rx_total = s_stats.rx_total;
		rx_valid_total = s_stats.rx_valid_total;
		rx_in_range_total = s_stats.rx_in_range_total;
		rx_total_1s = s_stats.rx_total_1s;
		rx_valid_1s = s_stats.rx_valid_1s;
		rx_in_range_1s = s_stats.rx_in_range_1s;
		last_universe = s_stats.last_universe;
		last_dmx_len = s_stats.last_dmx_len;
		s_stats.rx_total_1s = 0;
		s_stats.rx_valid_1s = 0;
		s_stats.rx_in_range_1s = 0;
		portEXIT_CRITICAL(&s_stats_lock);

		#if ARTNET_ENABLE_STATS_LOG
		ESP_LOGI(TAG,
			"ArtNet stats: rx/s=%lu valid/s=%lu mapped/s=%lu total=%lu valid=%lu mapped=%lu last_u=%u last_len=%u",
			(unsigned long)rx_total_1s,
			(unsigned long)rx_valid_1s,
			(unsigned long)rx_in_range_1s,
			(unsigned long)rx_total,
			(unsigned long)rx_valid_total,
			(unsigned long)rx_in_range_total,
			(unsigned)last_universe,
			(unsigned)last_dmx_len);
		#endif
	}
}

#if USB_NCM_ENABLED
static void artnet_udp_task(void *arg)
{
	(void)arg;

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0) {
		ESP_LOGE(TAG, "Failed to create UDP socket");
		vTaskDelete(NULL);
		return;
	}

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(ARTNET_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};

	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		ESP_LOGE(TAG, "Failed to bind UDP socket on port %d", ARTNET_PORT);
		close(sock);
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "Listening for Art-Net on UDP %d", ARTNET_PORT);

	uint8_t rx_buf[600];
	while (1) {
		ssize_t rx_len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, NULL, NULL);
		if (rx_len <= 0) {
			continue;
		}

		portENTER_CRITICAL(&s_stats_lock);
		s_stats.rx_total++;
		s_stats.rx_total_1s++;
		portEXIT_CRITICAL(&s_stats_lock);

		uint16_t universe = 0;
		const uint8_t *dmx_data = NULL;
		uint16_t dmx_len = 0;
		if (!parse_artnet_dmx(rx_buf, rx_len, &universe, &dmx_data, &dmx_len)) {
			continue;
		}

#if ARTNET_ENABLE_DMX_SAMPLE_LOG
		TickType_t now = xTaskGetTickCount();
		if ((now - s_last_dmx_debug_log_tick) >= pdMS_TO_TICKS(ARTNET_DMX_SAMPLE_LOG_INTERVAL_MS)) {
			s_last_dmx_debug_log_tick = now;
			char values_buf[192];
			size_t used = 0;
			uint16_t sample_len = dmx_len < 24 ? dmx_len : 24;
			uint16_t first_nonzero_ch = 0;
			uint16_t nonzero_count = 0;
			uint8_t first_nonzero_val = 0;
			uint8_t max_val = 0;

			for (uint16_t i = 0; i < dmx_len; i++) {
				uint8_t value = dmx_data[i];
				if (value > max_val) {
					max_val = value;
				}
				if (value != 0) {
					nonzero_count++;
					if (first_nonzero_ch == 0) {
						first_nonzero_ch = i + 1;
						first_nonzero_val = value;
					}
				}
			}

			for (uint16_t i = 0; i < sample_len; i++) {
				int written = snprintf(values_buf + used, sizeof(values_buf) - used,
					(i == 0) ? "%u" : ",%u", (unsigned)dmx_data[i]);
				if (written <= 0 || (size_t)written >= (sizeof(values_buf) - used)) {
					break;
				}
				used += (size_t)written;
			}
			ESP_LOGI(TAG, "ArtNet DMX u=%u len=%u nz=%u first_nz_ch=%u first_nz_val=%u max=%u first[%u]=%s",
				(unsigned)universe,
				(unsigned)dmx_len,
				(unsigned)nonzero_count,
				(unsigned)first_nonzero_ch,
				(unsigned)first_nonzero_val,
				(unsigned)max_val,
				(unsigned)sample_len,
				used ? values_buf : "<empty>");
		}
#endif

		portENTER_CRITICAL(&s_stats_lock);
		s_stats.rx_valid_total++;
		s_stats.rx_valid_1s++;
		s_stats.last_universe = universe;
		s_stats.last_dmx_len = dmx_len;
		portEXIT_CRITICAL(&s_stats_lock);

		if (universe >= (ARTNET_START_UNIVERSE + UNIVERSE_COUNT)) {
			continue;
		}

		portENTER_CRITICAL(&s_stats_lock);
		s_stats.rx_in_range_total++;
		s_stats.rx_in_range_1s++;
		portEXIT_CRITICAL(&s_stats_lock);

		s_last_mapped_packet_tick = xTaskGetTickCount();

		if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
			apply_artnet_dmx_to_frame(universe, dmx_data, dmx_len);
			bool frame_is_off = is_frame_all_off();
			if (frame_is_off) {
				if (!s_last_frame_was_off) {
					ESP_LOGI(TAG, "Off frame received");
				}
				s_idle_mode_armed = true;
				s_idle_mode_armed_tick = xTaskGetTickCount();
				s_last_frame_was_off = true;
			} else {
				s_idle_mode_force_start = false;
				s_idle_mode_armed = false;
				s_idle_mode_armed_tick = 0;
				s_last_frame_was_off = false;
			}
			xSemaphoreGive(s_led_mutex);
		}
	}
}
#endif

static void led_output_task(void *arg)
{
	(void)arg;
	bool was_idle = false;
#if IDLE_MODE_ENABLED
	uint8_t idle_phase = 0;
	TickType_t last_idle_step_tick = 0;
#endif

	while (1) {
		bool do_refresh = false;

		if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
			TickType_t now = xTaskGetTickCount();
			bool idle_active = false;
			bool frame_is_off = is_frame_all_off();
#if IDLE_MODE_ENABLED
			idle_active = s_idle_mode_force_start || (s_idle_mode_armed && (s_idle_mode_armed_tick != 0) && ((now - s_idle_mode_armed_tick) >= pdMS_TO_TICKS(IDLE_MODE_TIMEOUT_MS)));
#endif

			if (s_frame_dirty) {
				if (idle_active && frame_is_off) {
					s_frame_dirty = false;
				} else {
					for (size_t i = 0; i < OUTPUT_LED_COUNT; i++) {
						size_t base = i * 3;
						uint8_t r = s_dmx_buffer[base + 0];
						uint8_t g = s_dmx_buffer[base + 1];
						uint8_t b = s_dmx_buffer[base + 2];
						led_strip_set_pixel(s_output_led_strip, i, r, g, b);
					}
					s_frame_dirty = false;
					do_refresh = true;
					was_idle = false;
				}
			} else if (idle_active) {
#if IDLE_MODE_ENABLED
				if (!was_idle) {
					idle_phase = 0;
					last_idle_step_tick = 0;
					if (s_idle_mode_force_start) {
						ESP_LOGI(TAG, "Idle mode started (boot force-start)");
					} else {
						ESP_LOGI(TAG, "Idle mode started (armed + no non-off frame for %d ms)", IDLE_MODE_TIMEOUT_MS);
					}
					was_idle = true;
				}

				if ((last_idle_step_tick == 0) || ((now - last_idle_step_tick) >= pdMS_TO_TICKS(IDLE_MODE_STEP_INTERVAL_MS))) {
					render_idle_gradient(idle_phase);
					idle_phase++;
					last_idle_step_tick = now;
					do_refresh = true;
				}
#endif
			} else {
				if (was_idle) {
					ESP_LOGI(TAG, "Idle mode stopped");
				}
				was_idle = false;
			}
			xSemaphoreGive(s_led_mutex);
		}

		if (do_refresh) {
			led_strip_refresh(s_output_led_strip);
		}

		vTaskDelay(pdMS_TO_TICKS(LED_REFRESH_MS));
	}
}

#if STATUS_LED_ENABLED
static void status_led_task(void *arg)
{
	(void)arg;
	TickType_t tick = 0;

	while (1) {
		TickType_t now = xTaskGetTickCount();
		TickType_t elapsed = now - s_last_mapped_packet_tick;
		bool active_stream = elapsed <= pdMS_TO_TICKS(1500);

		#if !STATUS_LED_USE_GPIO
		uint8_t r = 0;
		uint8_t g = 0;
		uint8_t b = 0;
		#endif

		#if STATUS_LED_MONO_MODE
		#if STATUS_LED_USE_GPIO
		int level = 0;
		if (active_stream) {
			level = 1;
		} else if ((tick % 5) < 2) {
			level = 1;
		}
		gpio_set_level(STATUS_LED_GPIO, level);
		#else
		uint8_t mono = 0;
		if (active_stream) {
			mono = STATUS_LED_BRIGHTNESS;
		} else if ((tick % 5) < 2) {
			mono = STATUS_LED_BRIGHTNESS;
		}
		g = mono;
		#endif
		#else
		if (active_stream) {
			g = STATUS_LED_BRIGHTNESS;
		} else {
			if ((tick % 5) < 2) {
				b = STATUS_LED_BRIGHTNESS;
			}
		}
		#endif

		#if !STATUS_LED_USE_GPIO
		if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
			led_strip_set_pixel(s_status_led_strip, 0, r, g, b);
			led_strip_refresh(s_status_led_strip);
			xSemaphoreGive(s_led_mutex);
		}
		#endif

		tick++;
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}
#endif

void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	s_led_mutex = xSemaphoreCreateMutex();
	ESP_ERROR_CHECK(s_led_mutex == NULL ? ESP_FAIL : ESP_OK);

	memset(s_dmx_buffer, 0, sizeof(s_dmx_buffer));
	s_idle_mode_force_start = true;
	s_idle_mode_armed = true;
	s_idle_mode_armed_tick = xTaskGetTickCount() - pdMS_TO_TICKS(IDLE_MODE_TIMEOUT_MS);
	s_last_frame_was_off = false;
	s_last_mapped_packet_tick = xTaskGetTickCount();

	ESP_ERROR_CHECK(init_output_led_strip());
	run_output_led_selftest();
#if STATUS_LED_ENABLED
	ESP_ERROR_CHECK(init_status_led_strip());
#endif
	ESP_ERROR_CHECK(init_usb_ncm_network());

#if USB_NCM_ENABLED
	xTaskCreate(artnet_udp_task, "artnet_udp", 4096, NULL, 5, NULL);
#else
	ESP_LOGW(TAG, "Art-Net UDP task disabled on this target profile");
#endif
	xTaskCreate(led_output_task, "led_output", 4096, NULL, 5, NULL);
#if ARTNET_ENABLE_STATS_LOG
	xTaskCreate(artnet_stats_task, "artnet_stats", 3072, NULL, 4, NULL);
#endif
#if STATUS_LED_ENABLED
	xTaskCreate(status_led_task, "status_led", 3072, NULL, 3, NULL);
#endif

	ESP_LOGI(TAG, "System started: target=%s output GPIO=%d LEDS=%d", APP_TARGET_PROFILE_NAME, OUTPUT_LED_STRIP_GPIO, OUTPUT_LED_COUNT);
#if USB_NCM_ENABLED
	ESP_LOGI(TAG, "Networking: USB-NCM Art-Net enabled");
#else
	ESP_LOGI(TAG, "Networking: USB-NCM Art-Net disabled");
#endif
#if STATUS_LED_ENABLED
	ESP_LOGI(TAG, "Status LED: GPIO=%d mode=%s", STATUS_LED_GPIO, STATUS_LED_MONO_MODE ? "mono" : "rgb");
#endif
}
