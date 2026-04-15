#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WAV_HEADER_BUFFER_BYTES 512
#define WAV_PCM_HEADER_BYTES 44
#define PCM_SAMPLE_BYTES 2
#define MIC_TEST_DURATION_MS 2000
#define MIC_READ_TIMEOUT_MS 1000
#define MIC_READ_SAMPLE_COUNT 512
#define MIC_SAMPLE_SHIFT CONFIG_ESP_AI_AGENT_MIC_SAMPLE_SHIFT
#define MIC_PIN_PROBE_SAMPLES 20000
#define MIC_PIN_PULL_SETTLE_MS 20
#define MIC_PIN_PROBE_READER_STACK_BYTES 3072
#define PTT_BUTTON_POLL_MS 20
#define PTT_BUTTON_DEBOUNCE_MS 40
#define STATUS_LED_RMT_RESOLUTION_HZ 10000000
#ifndef CONFIG_ESP_AI_AGENT_CHAT_TASK_STACK_BYTES
#define CONFIG_ESP_AI_AGENT_CHAT_TASK_STACK_BYTES 8192
#endif

static const char *TAG = "esp_ai_agent";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;
static i2s_chan_handle_t s_speaker_tx_chan;
#if CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
static i2s_chan_handle_t s_microphone_rx_chan;
static volatile bool s_microphone_probe_reader_running;
#endif

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool truncated;
} http_response_buffer_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t data_size;
} wav_info_t;

typedef struct {
    uint8_t header[WAV_HEADER_BUFFER_BYTES];
    size_t header_len;
    bool header_done;
    bool failed;
    uint8_t pcm_remainder[PCM_SAMPLE_BYTES];
    size_t pcm_remainder_len;
    wav_info_t info;
} audio_stream_context_t;

typedef struct {
    int16_t *samples;
    size_t sample_count;
    size_t capacity_samples;
    bool truncated;
} mic_recording_t;

#if CONFIG_ESP_AI_AGENT_ENABLE_STATUS_LED
static rmt_channel_handle_t s_status_led_chan;
static rmt_encoder_handle_t s_status_led_encoder;
#endif

static void trim_newline(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static bool read_serial_line(char *buffer, size_t buffer_size)
{
    size_t len = 0;

    if (buffer_size == 0) {
        return false;
    }

    while (true) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            buffer[len] = '\0';
            trim_newline(buffer);
            return strlen(buffer) > 0;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (len < buffer_size - 1) {
            buffer[len++] = (char)ch;
        } else {
            buffer[len] = '\0';
            ESP_LOGW(TAG, "serial input too long, sending first %d bytes", (int)len);
            return true;
        }
    }
}

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_wifi_retry_count < CONFIG_ESP_AI_AGENT_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_count++;
            ESP_LOGI(TAG, "retrying Wi-Fi connection (%d/%d)",
                     s_wifi_retry_count, CONFIG_ESP_AI_AGENT_WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Wi-Fi connection failed");
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "failed to create Wi-Fi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s",
             CONFIG_ESP_AI_AGENT_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             CONFIG_ESP_AI_AGENT_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode =
        strlen(CONFIG_ESP_AI_AGENT_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to Wi-Fi SSID: %s", CONFIG_ESP_AI_AGENT_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static bool wifi_wait_connected(TickType_t timeout_ticks)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        timeout_ticks);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool fourcc_equals(const uint8_t *data, const char *tag)
{
    return memcmp(data, tag, 4) == 0;
}

typedef enum {
    WAV_PARSE_NEED_MORE,
    WAV_PARSE_OK,
    WAV_PARSE_INVALID,
} wav_parse_result_t;

static wav_parse_result_t wav_try_parse_header(const uint8_t *data, size_t len, wav_info_t *info, size_t *data_offset)
{
    if (len < 12) {
        return WAV_PARSE_NEED_MORE;
    }

    if (!fourcc_equals(data, "RIFF") || !fourcc_equals(data + 8, "WAVE")) {
        return WAV_PARSE_INVALID;
    }

    size_t pos = 12;
    bool got_fmt = false;
    wav_info_t parsed = {0};

    while (pos + 8 <= len) {
        const uint8_t *chunk = data + pos;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t chunk_data_pos = pos + 8;

        if (fourcc_equals(chunk, "data")) {
            if (!got_fmt) {
                return WAV_PARSE_INVALID;
            }
            parsed.data_size = chunk_size;
            *info = parsed;
            *data_offset = chunk_data_pos;
            return WAV_PARSE_OK;
        }

        if (chunk_data_pos + chunk_size > len) {
            return WAV_PARSE_NEED_MORE;
        }

        if (fourcc_equals(chunk, "fmt ")) {
            if (chunk_size < 16) {
                return WAV_PARSE_INVALID;
            }

            const uint8_t *fmt = data + chunk_data_pos;
            uint16_t audio_format = read_le16(fmt);
            if (audio_format != 1) {
                ESP_LOGE(TAG, "unsupported WAV format: %u (need PCM=1)", audio_format);
                return WAV_PARSE_INVALID;
            }

            parsed.channels = read_le16(fmt + 2);
            parsed.sample_rate = read_le32(fmt + 4);
            parsed.bits_per_sample = read_le16(fmt + 14);
            got_fmt = true;
        }

        size_t next_pos = chunk_data_pos + chunk_size + (chunk_size & 1U);
        if (next_pos <= pos) {
            return WAV_PARSE_INVALID;
        }
        pos = next_pos;
    }

    return WAV_PARSE_NEED_MORE;
}

static bool build_server_url(char *out, size_t out_size, const char *endpoint)
{
    const char *chat_url = CONFIG_ESP_AI_AGENT_CHAT_URL;
    const char *chat_suffix = "/chat";
    size_t chat_url_len = strlen(chat_url);
    size_t suffix_len = strlen(chat_suffix);

    if (out_size == 0) {
        return false;
    }

    if (chat_url_len >= suffix_len &&
        strcmp(chat_url + chat_url_len - suffix_len, chat_suffix) == 0) {
        int written = snprintf(out, out_size, "%.*s%s",
                               (int)(chat_url_len - suffix_len), chat_url, endpoint);
        return written > 0 && (size_t)written < out_size;
    }

    int written = snprintf(out, out_size, "%s%s%s", chat_url,
                           chat_url[chat_url_len - 1] == '/' ? "" : "/",
                           endpoint[0] == '/' ? endpoint + 1 : endpoint);
    return written > 0 && (size_t)written < out_size;
}

static bool build_tts_url(char *out, size_t out_size)
{
    return build_server_url(out, out_size, "/tts");
}

static bool build_voice_chat_url(char *out, size_t out_size)
{
    return build_server_url(out, out_size, "/voice_chat");
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xff);
    data[1] = (uint8_t)((value >> 8) & 0xff);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xff);
    data[1] = (uint8_t)((value >> 8) & 0xff);
    data[2] = (uint8_t)((value >> 16) & 0xff);
    data[3] = (uint8_t)((value >> 24) & 0xff);
}

static void build_wav_header(uint8_t header[WAV_PCM_HEADER_BYTES], uint32_t sample_rate, uint32_t data_size)
{
    uint32_t byte_rate = sample_rate * PCM_SAMPLE_BYTES;
    uint16_t block_align = PCM_SAMPLE_BYTES;

    memcpy(header, "RIFF", 4);
    write_le32(header + 4, data_size + 36);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    write_le32(header + 16, 16);
    write_le16(header + 20, 1);
    write_le16(header + 22, 1);
    write_le32(header + 24, sample_rate);
    write_le32(header + 28, byte_rate);
    write_le16(header + 32, block_align);
    write_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_size);
}

static uint8_t status_led_scale(uint8_t value)
{
#if CONFIG_ESP_AI_AGENT_ENABLE_STATUS_LED
    return (uint8_t)(((uint32_t)value * CONFIG_ESP_AI_AGENT_STATUS_LED_BRIGHTNESS) / 255);
#else
    (void)value;
    return 0;
#endif
}

static esp_err_t status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_STATUS_LED
    (void)red;
    (void)green;
    (void)blue;
    return ESP_OK;
#else
    if (s_status_led_chan == NULL || s_status_led_encoder == NULL) {
        return ESP_OK;
    }

    uint8_t grb[3] = {
        status_led_scale(green),
        status_led_scale(red),
        status_led_scale(blue),
    };
    rmt_symbol_word_t symbols[25] = {0};
    size_t index = 0;

    for (size_t byte_index = 0; byte_index < sizeof(grb); byte_index++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (grb[byte_index] & (1U << bit)) != 0;
            symbols[index++] = (rmt_symbol_word_t) {
                .level0 = 1,
                .duration0 = one ? 9 : 3,
                .level1 = 0,
                .duration1 = one ? 3 : 9,
            };
        }
    }

    symbols[index++] = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = 500,
        .level1 = 0,
        .duration1 = 500,
    };

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(
        s_status_led_chan,
        s_status_led_encoder,
        symbols,
        index * sizeof(symbols[0]),
        &tx_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to update status LED: %s", esp_err_to_name(err));
        return err;
    }

    return rmt_tx_wait_all_done(s_status_led_chan, 100);
#endif
}

static esp_err_t status_led_init(void)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_STATUS_LED
    return ESP_OK;
#else
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_ESP_AI_AGENT_STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = STATUS_LED_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_status_led_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED disabled, failed to create RMT channel on GPIO%d: %s",
                 CONFIG_ESP_AI_AGENT_STATUS_LED_GPIO, esp_err_to_name(err));
        s_status_led_chan = NULL;
        return ESP_OK;
    }

    rmt_copy_encoder_config_t copy_encoder_config = {};
    err = rmt_new_copy_encoder(&copy_encoder_config, &s_status_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED disabled, failed to create RMT encoder: %s", esp_err_to_name(err));
        s_status_led_encoder = NULL;
        return ESP_OK;
    }

    err = rmt_enable(s_status_led_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED disabled, failed to enable RMT channel: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "status RGB LED: WS2812 GPIO%d brightness=%d",
             CONFIG_ESP_AI_AGENT_STATUS_LED_GPIO,
             CONFIG_ESP_AI_AGENT_STATUS_LED_BRIGHTNESS);
    return status_led_set_rgb(0, 0, 0);
#endif
}

static void status_led_idle(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(status_led_set_rgb(0, 0, 0));
}

static void status_led_listening(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(status_led_set_rgb(255, 0, 0));
}

static void status_led_answering(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(status_led_set_rgb(0, 255, 0));
}

static esp_err_t speaker_init(void)
{
#if CONFIG_ESP_AI_AGENT_ENABLE_SPEAKER
    if (s_speaker_tx_chan != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_speaker_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create I2S TX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_ESP_AI_AGENT_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_ESP_AI_AGENT_SPK_BCLK_GPIO,
            .ws = CONFIG_ESP_AI_AGENT_SPK_WS_GPIO,
            .dout = CONFIG_ESP_AI_AGENT_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_speaker_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize I2S speaker: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_speaker_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable I2S speaker: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MAX98357A I2S speaker: port=0 sample_rate=%d BCLK=%d WS=%d DIN=%d",
             CONFIG_ESP_AI_AGENT_AUDIO_SAMPLE_RATE,
             CONFIG_ESP_AI_AGENT_SPK_BCLK_GPIO,
             CONFIG_ESP_AI_AGENT_SPK_WS_GPIO,
             CONFIG_ESP_AI_AGENT_SPK_DOUT_GPIO);
#endif
    return ESP_OK;
}

static esp_err_t microphone_init(void)
{
#if CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    if (s_microphone_rx_chan != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_microphone_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create I2S RX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_ESP_AI_AGENT_MIC_BCLK_GPIO,
            .ws = CONFIG_ESP_AI_AGENT_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(s_microphone_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize I2S microphone: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_microphone_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable I2S microphone: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ICS43434 I2S microphone: port=1 sample_rate=%d BCLK=%d WS=%d SD=%d",
             CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE,
             CONFIG_ESP_AI_AGENT_MIC_BCLK_GPIO,
             CONFIG_ESP_AI_AGENT_MIC_WS_GPIO,
             CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO);
#endif
    return ESP_OK;
}

static int16_t microphone_raw_to_pcm16(int32_t raw_sample)
{
    int32_t sample = raw_sample >> MIC_SAMPLE_SHIFT;
    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }
    return (int16_t)sample;
}

static esp_err_t microphone_run_level_test(uint32_t duration_ms)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    printf("Microphone is disabled. Enable CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE and rebuild.\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_microphone_rx_chan == NULL) {
        ESP_LOGE(TAG, "microphone is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    int32_t samples[MIC_READ_SAMPLE_COUNT];
    int64_t sum_squares[2] = {0};
    int64_t sum_abs[2] = {0};
    uint32_t total_samples[2] = {0};
    uint32_t clipped_samples[2] = {0};
    uint32_t nonzero_samples[2] = {0};
    int32_t peak_abs[2] = {0};
    int32_t raw_peak_abs[2] = {0};
    int32_t first_nonzero_raw[2] = {0};
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(duration_ms);

    ESP_LOGI(TAG, "microphone level test started: %" PRIu32 " ms", duration_ms);
    printf("Speak near the microphone now...\n");

    while ((xTaskGetTickCount() - start_ticks) < duration_ticks) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_microphone_rx_chan,
            samples,
            sizeof(samples),
            &bytes_read,
            MIC_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S microphone read failed: %s", esp_err_to_name(err));
            return err;
        }

        size_t sample_count = bytes_read / sizeof(samples[0]);
        for (size_t i = 0; i < sample_count; i++) {
            int channel = (int)(i & 1U);
            int32_t raw_sample = samples[i];
            int32_t raw_abs = raw_sample < 0 ? -raw_sample : raw_sample;
            if (raw_abs > raw_peak_abs[channel]) {
                raw_peak_abs[channel] = raw_abs;
            }
            if (raw_sample != 0) {
                nonzero_samples[channel]++;
                if (first_nonzero_raw[channel] == 0) {
                    first_nonzero_raw[channel] = raw_sample;
                }
            }

            int32_t sample = microphone_raw_to_pcm16(raw_sample);

            int32_t abs_sample = sample < 0 ? -sample : sample;
            if (abs_sample > peak_abs[channel]) {
                peak_abs[channel] = abs_sample;
            }
            if (abs_sample >= 32000) {
                clipped_samples[channel]++;
            }

            sum_abs[channel] += abs_sample;
            sum_squares[channel] += (int64_t)sample * sample;
            total_samples[channel]++;
        }
    }

    if (total_samples[0] == 0 && total_samples[1] == 0) {
        ESP_LOGE(TAG, "microphone did not return any samples");
        return ESP_FAIL;
    }

    int active_channel = peak_abs[1] > peak_abs[0] ? 1 : 0;
    double mean_abs = total_samples[active_channel] > 0 ?
        (double)sum_abs[active_channel] / (double)total_samples[active_channel] : 0.0;
    double rms = total_samples[active_channel] > 0 ?
        sqrt((double)sum_squares[active_channel] / (double)total_samples[active_channel]) : 0.0;

    printf("Mic raw: left_nonzero=%" PRIu32 ", left_raw_peak=%" PRId32 ", left_first=0x%08" PRIx32 "\n",
           nonzero_samples[0], raw_peak_abs[0], (uint32_t)first_nonzero_raw[0]);
    printf("Mic raw: right_nonzero=%" PRIu32 ", right_raw_peak=%" PRId32 ", right_first=0x%08" PRIx32 "\n",
           nonzero_samples[1], raw_peak_abs[1], (uint32_t)first_nonzero_raw[1]);
    printf(
        "Mic test: active=%s, samples=%" PRIu32 ", peak=%" PRId32 ", rms=%.1f, avg_abs=%.1f, clipped=%" PRIu32 "\n",
        active_channel == 0 ? "left" : "right",
        total_samples[active_channel],
        peak_abs[active_channel],
        rms,
        mean_abs,
        clipped_samples[active_channel]);

    if (raw_peak_abs[0] == 0 && raw_peak_abs[1] == 0) {
        printf("I2S clocks are running, but SD data is all zero. Check microphone SD/DOUT->GPIO16, VDD=3V3, GND, and module type.\n");
    } else if (peak_abs[active_channel] < 200) {
        printf("Mic has raw data but scaled level is low. Keep this output; sample alignment/gain needs tuning.\n");
    } else if (clipped_samples[active_channel] > total_samples[active_channel] / 100) {
        printf("Mic is clipping. Move it farther away or lower gain in the next audio stage.\n");
    } else {
        printf("Mic input looks alive.\n");
    }

    return ESP_OK;
#endif
}

typedef struct {
    int first_level;
    int last_level;
    uint32_t ones;
    uint32_t transitions;
} mic_gpio_activity_t;

static bool microphone_gpio_pin_is_valid(int gpio)
{
    return gpio >= 0 && gpio <= 48;
}

static void microphone_enable_gpio_input(int gpio)
{
    if (microphone_gpio_pin_is_valid(gpio)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_input_enable((gpio_num_t)gpio));
    }
}

static void microphone_probe_reader_task(void *arg)
{
    (void)arg;

#if CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    int32_t samples[128];

    while (s_microphone_probe_reader_running && s_microphone_rx_chan != NULL) {
        size_t bytes_read = 0;
        (void)i2s_channel_read(
            s_microphone_rx_chan,
            samples,
            sizeof(samples),
            &bytes_read,
            100);
    }
#endif

    vTaskDelete(NULL);
}

static esp_err_t microphone_start_probe_reader(void)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_microphone_probe_reader_running = true;
    BaseType_t created = xTaskCreate(
        microphone_probe_reader_task,
        "mic_pin_probe",
        MIC_PIN_PROBE_READER_STACK_BYTES,
        NULL,
        tskIDLE_PRIORITY + 5,
        NULL);
    if (created != pdPASS) {
        s_microphone_probe_reader_running = false;
        ESP_LOGE(TAG, "failed to create microphone pin probe reader task");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
#endif
}

static void microphone_stop_probe_reader(void)
{
#if CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    s_microphone_probe_reader_running = false;
    vTaskDelay(pdMS_TO_TICKS(150));
#endif
}

static mic_gpio_activity_t microphone_sample_gpio_activity(int gpio)
{
    mic_gpio_activity_t activity = {0};

    if (!microphone_gpio_pin_is_valid(gpio)) {
        activity.first_level = -1;
        activity.last_level = -1;
        return activity;
    }

    int previous = gpio_get_level((gpio_num_t)gpio);
    activity.first_level = previous;
    activity.last_level = previous;
    activity.ones = previous ? 1 : 0;

    for (uint32_t i = 1; i < MIC_PIN_PROBE_SAMPLES; i++) {
        int level = gpio_get_level((gpio_num_t)gpio);
        if (level) {
            activity.ones++;
        }
        if (level != previous) {
            activity.transitions++;
        }
        previous = level;
        activity.last_level = level;
    }

    return activity;
}

static void microphone_print_gpio_activity(const char *label, int gpio, mic_gpio_activity_t activity)
{
    if (activity.first_level < 0) {
        printf("%s GPIO%d is not valid.\n", label, gpio);
        return;
    }

    printf(
        "%s GPIO%d while I2S running: first=%d, last=%d, ones=%" PRIu32 "/%d, transitions=%" PRIu32 "\n",
        label,
        gpio,
        activity.first_level,
        activity.last_level,
        activity.ones,
        MIC_PIN_PROBE_SAMPLES,
        activity.transitions);
}

static uint32_t microphone_read_pull_count(int gpio, gpio_pull_mode_t pull_mode)
{
    if (!microphone_gpio_pin_is_valid(gpio)) {
        return 0;
    }

    gpio_num_t pin = (gpio_num_t)gpio;
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(pin, pull_mode));
    vTaskDelay(pdMS_TO_TICKS(MIC_PIN_PULL_SETTLE_MS));

    uint32_t ones = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (gpio_get_level(pin)) {
            ones++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ones;
}

static esp_err_t microphone_release_i2s_for_gpio_probe(void)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_microphone_rx_chan == NULL) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_disable(s_microphone_rx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to disable I2S microphone for GPIO probe: %s", esp_err_to_name(err));
    }

    err = i2s_del_channel(s_microphone_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to delete I2S microphone channel for GPIO probe: %s", esp_err_to_name(err));
        return err;
    }

    s_microphone_rx_chan = NULL;
    return ESP_OK;
#endif
}

static esp_err_t microphone_run_pin_diagnostic(void)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    printf("Microphone is disabled. Enable CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE and rebuild.\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_microphone_rx_chan == NULL) {
        esp_err_t init_err = microphone_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    printf("\nMic pin diagnostic using configured pins: BCLK=GPIO%d, WS=GPIO%d, SD=GPIO%d\n",
           CONFIG_ESP_AI_AGENT_MIC_BCLK_GPIO,
           CONFIG_ESP_AI_AGENT_MIC_WS_GPIO,
           CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO);

    microphone_enable_gpio_input(CONFIG_ESP_AI_AGENT_MIC_BCLK_GPIO);
    microphone_enable_gpio_input(CONFIG_ESP_AI_AGENT_MIC_WS_GPIO);
    microphone_enable_gpio_input(CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO);

    printf("Starting an active I2S RX read while probing the pins...\n");
    esp_err_t read_err = microphone_start_probe_reader();
    if (read_err != ESP_OK) {
        return read_err;
    }

    mic_gpio_activity_t bclk_activity = microphone_sample_gpio_activity(CONFIG_ESP_AI_AGENT_MIC_BCLK_GPIO);
    mic_gpio_activity_t ws_activity = microphone_sample_gpio_activity(CONFIG_ESP_AI_AGENT_MIC_WS_GPIO);
    mic_gpio_activity_t sd_activity = microphone_sample_gpio_activity(CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO);
    microphone_stop_probe_reader();

    microphone_print_gpio_activity("BCLK/SCK", CONFIG_ESP_AI_AGENT_MIC_BCLK_GPIO, bclk_activity);
    microphone_print_gpio_activity("WS", CONFIG_ESP_AI_AGENT_MIC_WS_GPIO, ws_activity);
    microphone_print_gpio_activity("SD/DOUT", CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO, sd_activity);

    printf("Stopping I2S RX briefly to test whether SD/DOUT is floating, pulled low, or pulled high...\n");
    esp_err_t err = microphone_release_i2s_for_gpio_probe();
    if (err != ESP_OK) {
        return err;
    }

    gpio_num_t sd_pin = (gpio_num_t)CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO;
    ESP_ERROR_CHECK(gpio_reset_pin(sd_pin));
    uint32_t floating_ones = microphone_read_pull_count(CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO, GPIO_FLOATING);
    uint32_t pullup_ones = microphone_read_pull_count(CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO, GPIO_PULLUP_ONLY);
    uint32_t pulldown_ones = microphone_read_pull_count(CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO, GPIO_PULLDOWN_ONLY);
    ESP_ERROR_CHECK(gpio_set_pull_mode(sd_pin, GPIO_FLOATING));

    printf("SD/DOUT GPIO%d stopped-clock pull test: floating_high=%" PRIu32 "/32, pullup_high=%" PRIu32 "/32, pulldown_high=%" PRIu32 "/32\n",
           CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO,
           floating_ones,
           pullup_ones,
           pulldown_ones);

    if (sd_activity.transitions == 0 && sd_activity.ones == 0 && pullup_ones >= 28 && pulldown_ones <= 4) {
        printf("Diagnosis: SD/DOUT looks high-Z when stopped and never toggles when clocks run. Check mic power, GND, SCK, WS, SD->GPIO%d, and tie LR/SEL to GND or 3V3.\n",
               CONFIG_ESP_AI_AGENT_MIC_DIN_GPIO);
    } else if (pullup_ones <= 4 && pulldown_ones <= 4) {
        printf("Diagnosis: SD/DOUT is held low even with the internal pull-up. Look for a short to GND, wrong module pin, or SD wired to GND/LR by mistake.\n");
    } else if (pullup_ones >= 28 && pulldown_ones >= 28) {
        printf("Diagnosis: SD/DOUT is held high. Look for a short to 3V3 or wrong module pin.\n");
    } else if (sd_activity.transitions > 0) {
        printf("Diagnosis: SD/DOUT is toggling at the GPIO. If /mic is still all zero, the I2S slot/sample alignment needs tuning next.\n");
    } else {
        printf("Diagnosis: inconclusive. Send this whole /micpins output plus a clear photo of the mic module wiring.\n");
    }

    return microphone_init();
#endif
}

static esp_err_t speaker_write_all(const uint8_t *data, size_t length)
{
    if (s_speaker_tx_chan == NULL || length == 0) {
        return ESP_OK;
    }

    size_t offset = 0;
    while (offset < length) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(
            s_speaker_tx_chan,
            data + offset,
            length - offset,
            &bytes_written,
            10000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            return err;
        }
        if (bytes_written == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        offset += bytes_written;
    }

    return ESP_OK;
}

static esp_err_t speaker_write_pcm(audio_stream_context_t *ctx, const uint8_t *data, size_t length)
{
    if (length == 0) {
        return ESP_OK;
    }

    if (ctx->pcm_remainder_len > 0) {
        while (ctx->pcm_remainder_len < PCM_SAMPLE_BYTES && length > 0) {
            ctx->pcm_remainder[ctx->pcm_remainder_len++] = *data++;
            length--;
        }

        if (ctx->pcm_remainder_len == PCM_SAMPLE_BYTES) {
            esp_err_t err = speaker_write_all(ctx->pcm_remainder, PCM_SAMPLE_BYTES);
            if (err != ESP_OK) {
                return err;
            }
            ctx->pcm_remainder_len = 0;
        }
    }

    size_t aligned_length = length - (length % PCM_SAMPLE_BYTES);
    if (aligned_length > 0) {
        esp_err_t err = speaker_write_all(data, aligned_length);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (aligned_length < length) {
        ctx->pcm_remainder[0] = data[aligned_length];
        ctx->pcm_remainder_len = 1;
    }

    return ESP_OK;
}

static esp_err_t audio_stream_consume(audio_stream_context_t *ctx, const uint8_t *data, size_t length)
{
    if (ctx->failed) {
        return ESP_FAIL;
    }

    if (!ctx->header_done) {
        size_t copy_len = length;
        if (copy_len > sizeof(ctx->header) - ctx->header_len) {
            copy_len = sizeof(ctx->header) - ctx->header_len;
        }

        memcpy(ctx->header + ctx->header_len, data, copy_len);
        ctx->header_len += copy_len;

        size_t data_offset = 0;
        wav_parse_result_t parse_result = wav_try_parse_header(
            ctx->header, ctx->header_len, &ctx->info, &data_offset);

        if (parse_result == WAV_PARSE_INVALID ||
            (parse_result == WAV_PARSE_NEED_MORE && ctx->header_len == sizeof(ctx->header))) {
            ESP_LOGE(TAG, "invalid or unsupported WAV header from /tts");
            ctx->failed = true;
            return ESP_FAIL;
        }

        if (parse_result == WAV_PARSE_NEED_MORE) {
            return ESP_OK;
        }

        if (ctx->info.sample_rate != CONFIG_ESP_AI_AGENT_AUDIO_SAMPLE_RATE ||
            ctx->info.bits_per_sample != 16 ||
            ctx->info.channels != 1) {
            ESP_LOGE(TAG, "unsupported WAV: %" PRIu32 " Hz, %u bit, %u channel(s)",
                     ctx->info.sample_rate, ctx->info.bits_per_sample, ctx->info.channels);
            ctx->failed = true;
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "playing WAV: %" PRIu32 " Hz, %u bit, %u channel(s), %" PRIu32 " bytes",
                 ctx->info.sample_rate, ctx->info.bits_per_sample, ctx->info.channels, ctx->info.data_size);
        ctx->header_done = true;

        if (ctx->header_len > data_offset) {
            esp_err_t err = speaker_write_pcm(ctx, ctx->header + data_offset, ctx->header_len - data_offset);
            if (err != ESP_OK) {
                ctx->failed = true;
                return err;
            }
        }

        if (copy_len == length) {
            return ESP_OK;
        }

        data += copy_len;
        length -= copy_len;
    }

    esp_err_t err = speaker_write_pcm(ctx, data, length);
    if (err != ESP_OK) {
        ctx->failed = true;
    }
    return err;
}

static esp_err_t tts_audio_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    audio_stream_context_t *ctx = (audio_stream_context_t *)evt->user_data;
    if (ctx == NULL) {
        return ESP_OK;
    }

    return audio_stream_consume(ctx, (const uint8_t *)evt->data, (size_t)evt->data_len);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    http_response_buffer_t *response = (http_response_buffer_t *)evt->user_data;
    if (response == NULL || response->data == NULL || response->capacity == 0) {
        return ESP_OK;
    }

    size_t remaining = response->capacity - response->length - 1;
    size_t to_copy = evt->data_len < remaining ? evt->data_len : remaining;

    if (to_copy > 0) {
        memcpy(response->data + response->length, evt->data, to_copy);
        response->length += to_copy;
        response->data[response->length] = '\0';
    }

    if ((size_t)evt->data_len > to_copy) {
        response->truncated = true;
    }

    return ESP_OK;
}

static esp_err_t http_client_write_all(esp_http_client_handle_t client, const uint8_t *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        int written = esp_http_client_write(
            client,
            (const char *)data + offset,
            (int)(length - offset));
        if (written < 0) {
            return ESP_FAIL;
        }
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        offset += (size_t)written;
    }

    return ESP_OK;
}

static esp_err_t allocate_response_buffer(http_response_buffer_t *response,
                                          size_t preferred_capacity,
                                          size_t minimum_capacity,
                                          const char *label)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (preferred_capacity < minimum_capacity) {
        preferred_capacity = minimum_capacity;
    }

    for (size_t capacity = preferred_capacity; capacity >= minimum_capacity; capacity /= 2) {
        char *data = heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
        if (data != NULL) {
            memset(data, 0, capacity);
            response->data = data;
            response->capacity = capacity;
            response->length = 0;
            response->truncated = false;
            return ESP_OK;
        }

        if (capacity == minimum_capacity) {
            break;
        }
    }

    ESP_LOGE(TAG, "failed to allocate %s response buffer; largest free 8-bit block is %u bytes",
             label,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_ERR_NO_MEM;
}

static esp_err_t post_tts_message(const char *text)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_SPEAKER
    (void)text;
    return ESP_OK;
#else
    if (s_speaker_tx_chan == NULL) {
        ESP_LOGW(TAG, "speaker is disabled or not initialized");
        return ESP_OK;
    }

    if (!wifi_wait_connected(pdMS_TO_TICKS(10000))) {
        ESP_LOGE(TAG, "Wi-Fi is not connected");
        return ESP_FAIL;
    }

    char tts_url[256];
    if (!build_tts_url(tts_url, sizeof(tts_url))) {
        ESP_LOGE(TAG, "failed to build /tts URL from %s", CONFIG_ESP_AI_AGENT_CHAT_URL);
        return ESP_FAIL;
    }

    cJSON *request_json = cJSON_CreateObject();
    if (request_json == NULL) {
        ESP_LOGE(TAG, "failed to create TTS request JSON");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request_json, "text", text);

    char *request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "failed to print TTS request JSON");
        return ESP_ERR_NO_MEM;
    }

    audio_stream_context_t audio = {0};
    esp_http_client_config_t config = {
        .url = tts_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_ESP_AI_AGENT_HTTP_TIMEOUT_MS,
        .event_handler = tts_audio_event_handler,
        .user_data = &audio,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cJSON_free(request_body);
        ESP_LOGE(TAG, "failed to initialize TTS HTTP client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_http_client_set_post_field(client, request_body, strlen(request_body)));

    ESP_LOGI(TAG, "POST %s", tts_url);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS HTTP status=%d", status_code);

    cJSON_free(request_body);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS request failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "TTS server returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    if (audio.failed || !audio.header_done) {
        ESP_LOGE(TAG, "TTS audio was not playable");
        return ESP_FAIL;
    }

    return ESP_OK;
#endif
}

static esp_err_t post_voice_chat_recording(const mic_recording_t *recording)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
    (void)recording;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (recording == NULL || recording->samples == NULL || recording->sample_count == 0) {
        ESP_LOGW(TAG, "voice recording is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_wait_connected(pdMS_TO_TICKS(10000))) {
        ESP_LOGE(TAG, "Wi-Fi is not connected");
        return ESP_FAIL;
    }

    char voice_url[256];
    if (!build_voice_chat_url(voice_url, sizeof(voice_url))) {
        ESP_LOGE(TAG, "failed to build /voice_chat URL from %s", CONFIG_ESP_AI_AGENT_CHAT_URL);
        return ESP_FAIL;
    }

    size_t pcm_bytes = recording->sample_count * sizeof(recording->samples[0]);
    size_t body_length = WAV_PCM_HEADER_BYTES + pcm_bytes;
    if (body_length > INT32_MAX) {
        ESP_LOGE(TAG, "voice recording is too large to upload");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t wav_header[WAV_PCM_HEADER_BYTES];
    build_wav_header(wav_header, CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE, (uint32_t)pcm_bytes);

    http_response_buffer_t response = {0};

    esp_http_client_config_t config = {
        .url = voice_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_ESP_AI_AGENT_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "failed to initialize voice HTTP client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "audio/wav"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-Audio-Sample-Rate", "16000"));

    ESP_LOGI(TAG, "POST %s, samples=%u, bytes=%u%s",
             voice_url,
             (unsigned)recording->sample_count,
             (unsigned)pcm_bytes,
             recording->truncated ? " (truncated)" : "");

    esp_err_t err = esp_http_client_open(client, (int)body_length);
    if (err == ESP_OK) {
        err = http_client_write_all(client, wav_header, sizeof(wav_header));
    }
    if (err == ESP_OK) {
        err = http_client_write_all(client, (const uint8_t *)recording->samples, pcm_bytes);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice upload failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    err = allocate_response_buffer(
        &response,
        CONFIG_ESP_AI_AGENT_VOICE_RESPONSE_MAX_BYTES,
        1024,
        "voice");
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int data_read = esp_http_client_read_response(client, response.data, (int)(response.capacity - 1));
    if (data_read >= 0) {
        response.length = (size_t)data_read;
        response.data[response.length] = '\0';
    }

    ESP_LOGI(TAG, "voice HTTP status=%d, content_length=%" PRId64, status_code, content_length);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "voice server returned HTTP %d: %s", status_code, response.data);
        heap_caps_free(response.data);
        return ESP_FAIL;
    }

    cJSON *response_json = cJSON_Parse(response.data);
    if (response_json == NULL) {
        ESP_LOGE(TAG, "failed to parse voice response JSON");
        ESP_LOGE(TAG, "raw response: %s", response.data);
        heap_caps_free(response.data);
        return ESP_FAIL;
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(response_json, "text");
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(response_json, "reply");
    char *reply_copy = NULL;

    if (cJSON_IsString(text) && text->valuestring != NULL) {
        printf("\nYou (voice): %s\n", text->valuestring);
    }

    if (cJSON_IsString(reply) && reply->valuestring != NULL) {
        printf("\nAI: %s\n", reply->valuestring);
        reply_copy = malloc(strlen(reply->valuestring) + 1);
        if (reply_copy != NULL) {
            strcpy(reply_copy, reply->valuestring);
        }
    } else {
        ESP_LOGE(TAG, "voice response JSON does not contain a string field named reply");
        ESP_LOGE(TAG, "raw response: %s", response.data);
    }

    cJSON_Delete(response_json);
    heap_caps_free(response.data);

    if (reply_copy != NULL) {
        esp_err_t tts_err = post_tts_message(reply_copy);
        free(reply_copy);
        if (tts_err != ESP_OK) {
            return tts_err;
        }
    }

    return ESP_OK;
#endif
}

static esp_err_t post_chat_message(const char *text)
{
    if (!wifi_wait_connected(pdMS_TO_TICKS(10000))) {
        ESP_LOGE(TAG, "Wi-Fi is not connected");
        return ESP_FAIL;
    }

    cJSON *request_json = cJSON_CreateObject();
    if (request_json == NULL) {
        ESP_LOGE(TAG, "failed to create request JSON");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request_json, "text", text);

    char *request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "failed to print request JSON");
        return ESP_ERR_NO_MEM;
    }

    http_response_buffer_t response = {
        .capacity = CONFIG_ESP_AI_AGENT_HTTP_RESPONSE_MAX_BYTES,
    };
    response.data = calloc(response.capacity, 1);
    if (response.data == NULL) {
        cJSON_free(request_body);
        ESP_LOGE(TAG, "failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = CONFIG_ESP_AI_AGENT_CHAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_ESP_AI_AGENT_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response.data);
        cJSON_free(request_body);
        ESP_LOGE(TAG, "failed to initialize HTTP client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_http_client_set_post_field(client, request_body, strlen(request_body)));

    ESP_LOGI(TAG, "POST %s", CONFIG_ESP_AI_AGENT_CHAT_URL);
    ESP_LOGI(TAG, "You: %s", text);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%" PRId64, status_code, content_length);

    cJSON_free(request_body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(response.data);
        return err;
    }

    if (response.truncated) {
        ESP_LOGW(TAG, "response was truncated at %d bytes",
                 CONFIG_ESP_AI_AGENT_HTTP_RESPONSE_MAX_BYTES);
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "server returned HTTP %d: %s", status_code, response.data);
        esp_http_client_cleanup(client);
        free(response.data);
        return ESP_FAIL;
    }

    cJSON *response_json = cJSON_Parse(response.data);
    if (response_json == NULL) {
        ESP_LOGE(TAG, "failed to parse response JSON");
        ESP_LOGE(TAG, "raw response: %s", response.data);
        esp_http_client_cleanup(client);
        free(response.data);
        return ESP_FAIL;
    }

    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(response_json, "reply");
    char *reply_copy = NULL;
    if (cJSON_IsString(reply) && reply->valuestring != NULL) {
        printf("\nAI: %s\n", reply->valuestring);
        reply_copy = malloc(strlen(reply->valuestring) + 1);
        if (reply_copy != NULL) {
            strcpy(reply_copy, reply->valuestring);
        } else {
            ESP_LOGW(TAG, "failed to allocate reply copy for TTS");
        }
    } else {
        ESP_LOGE(TAG, "response JSON does not contain a string field named reply");
        ESP_LOGE(TAG, "raw response: %s", response.data);
    }

    cJSON_Delete(response_json);
    esp_http_client_cleanup(client);
    free(response.data);

    if (reply_copy != NULL) {
        esp_err_t tts_err = post_tts_message(reply_copy);
        free(reply_copy);
        if (tts_err != ESP_OK) {
            return tts_err;
        }
    }

    return ESP_OK;
}

static bool ptt_button_is_pressed(void)
{
#if CONFIG_ESP_AI_AGENT_ENABLE_PUSH_TO_TALK
    return gpio_get_level((gpio_num_t)CONFIG_ESP_AI_AGENT_PTT_BUTTON_GPIO) == 0;
#else
    return false;
#endif
}

static esp_err_t ptt_button_init(void)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_PUSH_TO_TALK
    return ESP_OK;
#else
    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << CONFIG_ESP_AI_AGENT_PTT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&button_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure push-to-talk button GPIO%d: %s",
                 CONFIG_ESP_AI_AGENT_PTT_BUTTON_GPIO, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "push-to-talk button: GPIO%d, active low, hold to listen",
             CONFIG_ESP_AI_AGENT_PTT_BUTTON_GPIO);
    return ESP_OK;
#endif
}

static void mic_recording_free(mic_recording_t *recording)
{
    if (recording == NULL) {
        return;
    }
    heap_caps_free(recording->samples);
    recording->samples = NULL;
    recording->sample_count = 0;
    recording->capacity_samples = 0;
    recording->truncated = false;
}

static int16_t *microphone_alloc_recording_buffer(size_t *capacity_samples)
{
    size_t samples = (size_t)CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE *
                     CONFIG_ESP_AI_AGENT_PTT_MAX_RECORD_SECONDS;
    size_t min_samples = (size_t)CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE;

    while (samples >= min_samples) {
        size_t bytes = samples * sizeof(int16_t);
        int16_t *buffer = NULL;

#ifdef CONFIG_SPIRAM
        buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (buffer == NULL) {
            buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }

        if (buffer != NULL) {
            *capacity_samples = samples;
            return buffer;
        }

        samples = (samples * 3) / 4;
    }

    *capacity_samples = 0;
    return NULL;
}

static esp_err_t microphone_record_while_button_held(mic_recording_t *recording)
{
#if !CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE || !CONFIG_ESP_AI_AGENT_ENABLE_PUSH_TO_TALK
    (void)recording;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_microphone_rx_chan == NULL) {
        ESP_LOGE(TAG, "microphone is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memset(recording, 0, sizeof(*recording));
    recording->samples = microphone_alloc_recording_buffer(&recording->capacity_samples);
    if (recording->samples == NULL) {
        ESP_LOGE(TAG, "failed to allocate microphone recording buffer; largest free 8-bit block is %u bytes",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    int32_t raw_samples[MIC_READ_SAMPLE_COUNT];
    TickType_t start_ticks = xTaskGetTickCount();
    status_led_listening();
    ESP_LOGI(TAG, "listening while GPIO%d is held low, buffer=%u bytes (%.2f sec)",
             CONFIG_ESP_AI_AGENT_PTT_BUTTON_GPIO,
             (unsigned)(recording->capacity_samples * sizeof(recording->samples[0])),
             (double)recording->capacity_samples / (double)CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE);
    printf("\nListening... release the button to answer. Buffer %.2f sec.\n",
           (double)recording->capacity_samples / (double)CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE);

    while (ptt_button_is_pressed()) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_microphone_rx_chan,
            raw_samples,
            sizeof(raw_samples),
            &bytes_read,
            MIC_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S microphone read failed while recording: %s", esp_err_to_name(err));
            mic_recording_free(recording);
            status_led_idle();
            return err;
        }

        size_t raw_count = bytes_read / sizeof(raw_samples[0]);
        for (size_t i = 0; i < raw_count; i += 2) {
            if (recording->sample_count >= recording->capacity_samples) {
                recording->truncated = true;
                break;
            }
            recording->samples[recording->sample_count++] = microphone_raw_to_pcm16(raw_samples[i]);
        }

        if (recording->truncated) {
            ESP_LOGW(TAG, "recording reached %d seconds, release the button to answer",
                     CONFIG_ESP_AI_AGENT_PTT_MAX_RECORD_SECONDS);
            break;
        }
    }

    TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
    ESP_LOGI(TAG, "recorded %u samples in %u ms%s",
             (unsigned)recording->sample_count,
             (unsigned)(elapsed_ticks * portTICK_PERIOD_MS),
             recording->truncated ? " (truncated)" : "");
    return ESP_OK;
#endif
}

static void push_to_talk_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!ptt_button_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(PTT_BUTTON_POLL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(PTT_BUTTON_DEBOUNCE_MS));
        if (!ptt_button_is_pressed()) {
            continue;
        }

        mic_recording_t recording = {0};
        esp_err_t err = microphone_record_while_button_held(&recording);
        if (err == ESP_OK && recording.sample_count > CONFIG_ESP_AI_AGENT_MIC_SAMPLE_RATE / 4) {
            status_led_answering();
            err = post_voice_chat_recording(&recording);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "voice chat failed: %s", esp_err_to_name(err));
            }
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "recording too short, ignored");
        }

        mic_recording_free(&recording);
        status_led_idle();

        while (ptt_button_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(PTT_BUTTON_POLL_MS));
        }
    }
}

static void chat_console_task(void *arg)
{
    (void)arg;

    char input[CONFIG_ESP_AI_AGENT_INPUT_MAX_BYTES];

    while (true) {
#if CONFIG_ESP_AI_AGENT_ENABLE_MICROPHONE
        printf("\nHold GPIO%d button to talk. Type /mic to test audio, /micpins to test wiring, or enter text:\n> ",
               CONFIG_ESP_AI_AGENT_PTT_BUTTON_GPIO);
#else
        printf("\nType message and press Enter:\n> ");
#endif

        if (!read_serial_line(input, sizeof(input))) {
            continue;
        }

        if (strcmp(input, "/mic") == 0 || strcmp(input, "mic") == 0) {
            microphone_run_level_test(MIC_TEST_DURATION_MS);
            continue;
        }

        if (strcmp(input, "/micpins") == 0 || strcmp(input, "micpins") == 0) {
            microphone_run_pin_diagnostic();
            continue;
        }

        status_led_answering();
        post_chat_message(input);
        status_led_idle();
    }
}

void app_main(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "ESP32-S3 ESP-IDF chat client");
    ESP_LOGI(TAG, "chat URL: %s", CONFIG_ESP_AI_AGENT_CHAT_URL);

    init_nvs();
    bool connected = wifi_init_sta();
    if (!connected) {
        ESP_LOGE(TAG, "cannot continue without Wi-Fi");
        return;
    }

    ESP_ERROR_CHECK(speaker_init());
    ESP_ERROR_CHECK(microphone_init());
    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(ptt_button_init());

    BaseType_t created = xTaskCreate(
        chat_console_task,
        "chat_console",
        CONFIG_ESP_AI_AGENT_CHAT_TASK_STACK_BYTES,
        NULL,
        tskIDLE_PRIORITY + 4,
        NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create chat console task");
    }

#if CONFIG_ESP_AI_AGENT_ENABLE_PUSH_TO_TALK
    created = xTaskCreate(
        push_to_talk_task,
        "push_to_talk",
        CONFIG_ESP_AI_AGENT_CHAT_TASK_STACK_BYTES,
        NULL,
        tskIDLE_PRIORITY + 5,
        NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create push-to-talk task");
    }
#endif

    vTaskDelete(NULL);
}
