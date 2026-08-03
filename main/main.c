/**
 * ESP32 远程空调遥控器 — 第一步：WiFi + MQTT 远程控制（仅串口日志）
 *
 * 功能：
 *   1. STA 模式连接路由器（WiFi 参数在 menuconfig 中配置）
 *   2. 连接 MQTT broker（默认公共免费服务器 broker.emqx.io）
 *   3. 用 MAC 地址后 3 字节生成唯一设备 ID，订阅 {prefix}/{id}/cmd 收命令
 *   4. 收到 JSON 命令 → 解析 → app_command_execute() 执行（当前仅打印，
 *      红外模块到位后在此接入）
 *
 * 配置方式（VSCode）：
 *   Ctrl+E, O  打开 menuconfig → "AC Remote Configuration" 填写 WiFi/MQTT
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "driver/gpio.h"

#define AC_WIFI_SSID       CONFIG_AC_WIFI_SSID
#define AC_WIFI_PASS       CONFIG_AC_WIFI_PASSWORD
#define AC_MQTT_URI        CONFIG_AC_MQTT_BROKER_URI
#define AC_MQTT_USER       CONFIG_AC_MQTT_USERNAME
#define AC_MQTT_PWD        CONFIG_AC_MQTT_PASSWORD
#define AC_TOPIC_PREFIX    CONFIG_AC_MQTT_TOPIC_PREFIX
#define AC_LED_GPIO        ((gpio_num_t)CONFIG_AC_LED_GPIO)

static const char *TAG = "ac_remote";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* 设备标识与主题（全局，由 app_build_identity() 初始化） */
static char s_device_id[7];          /* 如 "3c71bf"（MAC 后 3 字节 hex） */
static char s_cmd_topic[64];         /* {prefix}/{device_id}/cmd   收命令 */
static char s_status_topic[64];      /* {prefix}/{device_id}/status 发状态 */

/* ==================== 设备标识 ==================== */

static void app_build_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "%s/%s/cmd",
             AC_TOPIC_PREFIX, s_device_id);
    snprintf(s_status_topic, sizeof(s_status_topic), "%s/%s/status",
             AC_TOPIC_PREFIX, s_device_id);
    ESP_LOGI(TAG, "device_id=%s", s_device_id);
    ESP_LOGI(TAG, "cmd topic    = %s", s_cmd_topic);
    ESP_LOGI(TAG, "status topic = %s", s_status_topic);
}

/* ==================== 电源指示灯（红外模块未到前的临时效果） ==================== */

static void app_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << AC_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(AC_LED_GPIO, 0);   /* 初始熄灭 */
    ESP_LOGI(TAG, "LED on GPIO%d initialized (off)", AC_LED_GPIO);
}

static void app_led_set(bool on)
{
    gpio_set_level(AC_LED_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "[LED] power %s", on ? "ON" : "OFF");
}

/* ==================== 命令执行 ==================== */

/**
 * 命令分发入口：解析后的 JSON 命令在此执行。
 * 当前阶段：on/off 绑定电源指示灯 GPIO（LED 正极接 AC_LED_GPIO）。
 * TODO(红外模块): 将 cmd 映射为空调红外码并发射，替换/叠加到 LED 控制。
 */
static void app_command_execute(const cJSON *root)
{
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cmd == NULL || !cJSON_IsString(cmd)) {
        ESP_LOGW(TAG, "[EXEC] command has no string field 'cmd'");
        return;
    }

    ESP_LOGI(TAG, "[EXEC] >>> executing command: %s", cmd->valuestring);

    if (!strcmp(cmd->valuestring, "on")) {
        app_led_set(true);
        /* TODO(红外): ir_send(IR_POWER); */
    } else if (!strcmp(cmd->valuestring, "off")) {
        app_led_set(false);
        /* TODO(红外): ir_send(IR_POWER); */
    } else {
        /* mode/temp/fan 等参数后续红外模块使用时再解析执行 */
        ESP_LOGI(TAG, "[EXEC] command ignored (IR module not ready yet): %s",
                 cmd->valuestring);
    }
}

/* ==================== MQTT 事件 ==================== */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to %s", AC_MQTT_URI);

        /* 订阅命令主题，收到即远程指令 */
        msg_id = esp_mqtt_client_subscribe(client, s_cmd_topic, 1);
        ESP_LOGI(TAG, "subscribed %s (msg_id=%d)", s_cmd_topic, msg_id);

        /* 发布在线状态（保留消息，便于控制端上线即可见） */
        char status[128];
        snprintf(status, sizeof(status),
                 "{\"state\":\"online\",\"device_id\":\"%s\"}", s_device_id);
        msg_id = esp_mqtt_client_publish(client, s_status_topic, status,
                                         0, 1, 1);   /* qos1, retain */
        ESP_LOGI(TAG, "published %s (msg_id=%d)", status, msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected, client will auto reconnect...");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "subscribe OK (msg_id=%d)", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "received topic=%.*s data=%.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);

        cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
        if (root != NULL) {
            app_command_execute(root);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "payload is not valid JSON");
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error, error_type=%d",
                 event->error_handle ? event->error_handle->error_type : -1);
        break;

    default:
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = AC_MQTT_URI,
        .credentials.username = AC_MQTT_USER,
        .credentials.client_id = s_device_id,   /* 用设备 ID 作客户端 ID */
        .session.keepalive = 15,
    };
    /* 密码非空才填，否则留空 */
    if (strlen(AC_MQTT_PWD) > 0) {
        mqtt_cfg.credentials.authentication.password = AC_MQTT_PWD;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

/* ==================== WiFi 事件 ==================== */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting to %s...", AC_WIFI_SSID);
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();   /* 断线自动重连 */

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        mqtt_app_start();     /* 拿到 IP 后再启动 MQTT */
    }
}

static void wifi_init_sta(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = AC_WIFI_SSID,
            .password = AC_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init finished.");
}

/* ==================== 入口 ==================== */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());   /* WiFi/MQTT 依赖 NVS */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_build_identity();
    app_led_init();      /* 初始化电源指示灯 */
    wifi_init_sta();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
