#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "espnow_peer";

/* MACs autorizadas del maestro ESP32-S3 */
static const uint8_t EDGE_AGENT_MAC_1[6] = { 0xDC, 0xB4, 0xD9, 0x17, 0x91, 0x04 };
static const uint8_t EDGE_AGENT_MAC_2[6] = { 0x80, 0xB5, 0x4E, 0xDE, 0x45, 0xAC };

/* Canal actual del Wi-Fi / ESP-NOW */
static const uint8_t ESPNOW_CHANNEL = 11;

/* GPIO del rele; ajustar segun el cableado */
static const gpio_num_t RELAY_GPIO = GPIO_NUM_4;
/* GPIO del boton local; activo en LOW con pull-up interno */
static const gpio_num_t RELAY_BUTTON_GPIO = GPIO_NUM_5;
static const TickType_t BUTTON_POLL_INTERVAL = pdMS_TO_TICKS(20);
static const TickType_t BUTTON_DEBOUNCE_TIME = pdMS_TO_TICKS(60);
static bool relay_state = false;

static void log_wifi_radio_state(const char *label)
{
    uint8_t self_mac[6] = {0};
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    int8_t tx_power = 0;

    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    esp_err_t channel_err = esp_wifi_get_channel(&primary, &second);
    esp_err_t tx_err = esp_wifi_get_max_tx_power(&tx_power);

    if (mac_err == ESP_OK && channel_err == ESP_OK && tx_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "%s self=" MACSTR " channel=%u tx_power_raw=%d approx_dbm=%d",
                 label,
                 MAC2STR(self_mac),
                 primary,
                 tx_power,
                 tx_power / 4);
        return;
    }

    ESP_LOGW(TAG,
             "%s mac_err=%s channel_err=%s tx_err=%s",
             label,
             esp_err_to_name(mac_err),
             esp_err_to_name(channel_err),
             esp_err_to_name(tx_err));
}

static bool is_authorized_peer(const uint8_t *mac)
{
    return mac &&
           (memcmp(mac, EDGE_AGENT_MAC_1, sizeof(EDGE_AGENT_MAC_1)) == 0 ||
            memcmp(mac, EDGE_AGENT_MAC_2, sizeof(EDGE_AGENT_MAC_2)) == 0);
}

static void espnow_send_text_reply(const uint8_t *dst_addr, const char *text)
{
    if (!dst_addr || !text) {
        return;
    }

    esp_err_t err = esp_now_send(dst_addr, (const uint8_t *)text, strlen(text));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply failed: %s", esp_err_to_name(err));
    }
}

static void notify_relay_state(void)
{
    const char *message = relay_state ? "event relay=on" : "event relay=off";
    espnow_send_text_reply(EDGE_AGENT_MAC_1, message);
    espnow_send_text_reply(EDGE_AGENT_MAC_2, message);
}

static void relay_apply(bool enabled)
{
    relay_state = enabled;
    ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, enabled ? 1 : 0));
    ESP_LOGI(TAG, "relay=%s gpio=%d", enabled ? "on" : "off", RELAY_GPIO);
}

static void relay_init(void)
{
    const gpio_config_t relay_cfg = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&relay_cfg));
    relay_apply(false);
}

static void relay_button_task(void *arg)
{
    (void)arg;

    bool last_sample = gpio_get_level(RELAY_BUTTON_GPIO) == 0;
    bool stable_state = last_sample;
    TickType_t last_change_tick = xTaskGetTickCount();

    while (1) {
        bool pressed = gpio_get_level(RELAY_BUTTON_GPIO) == 0;
        TickType_t now = xTaskGetTickCount();

        if (pressed != last_sample) {
            last_sample = pressed;
            last_change_tick = now;
        }

        if (pressed != stable_state && (now - last_change_tick) >= BUTTON_DEBOUNCE_TIME) {
            stable_state = pressed;
            if (stable_state) {
                relay_apply(!relay_state);
                notify_relay_state();
                ESP_LOGI(TAG, "relay button pressed gpio=%d", RELAY_BUTTON_GPIO);
            }
        }

        vTaskDelay(BUTTON_POLL_INTERVAL);
    }
}

static void relay_button_init(void)
{
    const gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << RELAY_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_cfg));
    BaseType_t ok = xTaskCreate(relay_button_task, "relay_btn", 2048, NULL, 5, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(80)); // 20 dBm
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    log_wifi_radio_state("wifi ready");
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (!tx_info) {
        return;
    }

    ESP_LOGI(TAG, "tx to " MACSTR " status=%s",
             MAC2STR(tx_info->des_addr),
             status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (!recv_info || !recv_info->src_addr || !data || len <= 0) {
        return;
    }

    if (!is_authorized_peer(recv_info->src_addr)) {
        ESP_LOGW(TAG, "ignoring rx from unauthorized peer " MACSTR,
                 MAC2STR(recv_info->src_addr));
        return;
    }

    int rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : 0;
    ESP_LOGI(TAG, "rx from " MACSTR " len=%d rssi=%d text=%.*s",
             MAC2STR(recv_info->src_addr), len, rssi, len, (const char *)data);

    if (len == 4 && memcmp(data, "hola", 4) == 0) {
        uint8_t self_mac[6] = {0};
        ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, self_mac));
        char reply[32] = {0};
        snprintf(reply, sizeof(reply), "ack desde " MACSTR, MAC2STR(self_mac));
        espnow_send_text_reply(recv_info->src_addr, reply);
        return;
    }

    if ((len == 7 && memcmp(data, "rele on", 7) == 0) ||
        (len == 8 && memcmp(data, "relay on", 8) == 0)) {
        relay_apply(true);
        notify_relay_state();
        espnow_send_text_reply(recv_info->src_addr, "ok relay=on");
        return;
    }

    if ((len == 8 && memcmp(data, "rele off", 8) == 0) ||
        (len == 9 && memcmp(data, "relay off", 9) == 0)) {
        relay_apply(false);
        notify_relay_state();
        espnow_send_text_reply(recv_info->src_addr, "ok relay=off");
        return;
    }

    if ((len == 11 && memcmp(data, "rele toggle", 11) == 0) ||
        (len == 12 && memcmp(data, "relay toggle", 12) == 0)) {
        relay_apply(!relay_state);
        notify_relay_state();
        espnow_send_text_reply(recv_info->src_addr, relay_state ? "ok relay=on" : "ok relay=off");
        return;
    }

    if ((len == 11 && memcmp(data, "rele status", 11) == 0) ||
        (len == 12 && memcmp(data, "relay status", 12) == 0)) {
        espnow_send_text_reply(recv_info->src_addr, relay_state ? "ok relay=on" : "ok relay=off");
        return;
    }

    espnow_send_text_reply(recv_info->src_addr, "err invalid_cmd");
}

static void espnow_init_peer(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, EDGE_AGENT_MAC_1, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(EDGE_AGENT_MAC_1)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }

    memcpy(peer.peer_addr, EDGE_AGENT_MAC_2, 6);
    if (!esp_now_is_peer_exist(EDGE_AGENT_MAC_2)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    relay_init();
    relay_button_init();
    wifi_init();
    espnow_init_peer();

    log_wifi_radio_state("peer ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
