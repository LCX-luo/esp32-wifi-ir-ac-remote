/**
 * 红外学习/捕获模式（临时）：接收空调遥控器信号，打印原始时序 + 按美的(Midea)时序猜测解析。
 * 用途：确认小天鹅 RN12B1 遥控器的真实协议，为正式红外控制做准备。
 *
 * 硬件：接收模块 HX-M121 DAT -> GPIO21（注意 5V 适配，见 docs/error-fixes.md 问题8）
 * 操作：烧录后打开串口监视器，将空调遥控器（或手机红外 App 的"美的"模式）对准接收模块，
 *       分别按"开关、制冷、制热、温度+、风速"等常用键，每次按键串口会打印一帧。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_types.h"
#include "ir_test.h"

#define IR_RX_GPIO         GPIO_NUM_21
#define RMT_RESOLUTION_HZ  1000000u   /* 1 tick = 1us */
#define IR_RX_BUF_MAX      128        /* 空调帧可能含反码+双重发送，符号数较多 */
#define IR_RX_TIMEOUT_MS   3000

static const char *TAG = "ir_test";

static QueueHandle_t s_ir_queue;
static rmt_symbol_word_t s_rx_buf[IR_RX_BUF_MAX];
static volatile size_t s_rx_num = 0;

/* RX 完成回调：中断上下文，仅拷贝+通知任务 */
static bool IRAM_ATTR ir_rx_done_cb(rmt_channel_handle_t chan,
                                    const rmt_rx_done_event_data_t *edata,
                                    void *user_ctx)
{
    size_t n = edata->num_symbols;
    if (n > IR_RX_BUF_MAX) {
        n = IR_RX_BUF_MAX;
    }
    memcpy(s_rx_buf, edata->received_symbols, n * sizeof(rmt_symbol_word_t));
    s_rx_num = n;

    BaseType_t high_task_woken = pdFALSE;
    uint32_t evt = 1;
    xQueueSendFromISR(s_ir_queue, &evt, &high_task_woken);
    return high_task_woken == pdTRUE;
}

/* 打印原始符号时序（人眼/工具分析用） */
static void ir_print_raw(void)
{
    ESP_LOGI(TAG, "[RAW] 捕获 %d 个符号:", (int)s_rx_num);
    for (int i = 0; i < (int)s_rx_num; i++) {
        ESP_LOGI(TAG, "  [%02d] H=%u L=%u",
                 i, s_rx_buf[i].duration0, s_rx_buf[i].duration1);
    }
}

/* 按 Midea 时序猜测解析：
 * 符号[0] 视为引导码；其后每个符号 [mark~560us, space]，space>1000us=1 否则 0。
 * 输出 bit 串 + 按 8bit 分组 hex（先按 MSB-first 打印，供人工判断位序）。 */
static void ir_midea_guess(void)
{
    int nbits = 0;
    char bits[IR_RX_BUF_MAX + 1] = {0};

    for (int i = 1; i < (int)s_rx_num; i++) {
        uint32_t sp = s_rx_buf[i].duration1;
        if (sp < 300) {
            break;   /* 尾部标记或分隔，结束 */
        }
        bits[nbits++] = (sp > 1000) ? '1' : '0';
    }
    bits[nbits] = '\0';

    if (nbits == 0) {
        ESP_LOGW(TAG, "[GUESS] 无有效数据位（引导后没有数据符号？）");
        return;
    }

    ESP_LOGI(TAG, "[GUESS] 引导H=%u L=%u, 数据bit=%d: %s",
             s_rx_buf[0].duration0, s_rx_buf[0].duration1, nbits, bits);

    /* 按 8bit 一组打印 hex（MSB first） */
    char hex_line[128] = {0};
    size_t off = 0;
    for (int i = 0; i + 8 <= nbits; i += 8) {
        uint8_t b = 0;
        for (int j = 0; j < 8; j++) {
            if (bits[i + j] == '1') {
                b |= (1u << (7 - j));
            }
        }
        off += snprintf(hex_line + off, sizeof(hex_line) - off, "0x%02X ", b);
    }
    ESP_LOGI(TAG, "[GUESS] hex: %s", hex_line);
}

static void ir_test_task(void *arg)
{
    /* ---- 接收通道（含 5V DAT 适配：输入 + 内部下拉） ---- */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_BUF_MAX,
    };
    rmt_channel_handle_t rx_chan = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx_chan));
    ESP_ERROR_CHECK(rmt_enable(rx_chan));

    gpio_config_t rx_io = {
        .pin_bit_mask = (1ULL << IR_RX_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rx_io));

    s_ir_queue = xQueueCreate(8, sizeof(uint32_t));
    rmt_rx_event_callbacks_t rx_cbs = { .on_recv_done = ir_rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &rx_cbs, NULL));

    rmt_receive_config_t rx_recv_cfg = {
        .signal_range_min_ns = 1250,
        .signal_range_max_ns = 12000000,
    };

    ESP_LOGI(TAG, "红外学习模式启动：请将空调遥控器/手机红外App(美的)对准接收模块(GPIO%d)按按键",
             IR_RX_GPIO);

    int cnt = 0;
    uint32_t evt;
    while (1) {
        ESP_ERROR_CHECK(rmt_receive(rx_chan, s_rx_buf, sizeof(s_rx_buf), &rx_recv_cfg));

        if (xQueueReceive(s_ir_queue, &evt, pdMS_TO_TICKS(IR_RX_TIMEOUT_MS)) == pdTRUE) {
            cnt++;
            ESP_LOGI(TAG, "===== 第 %d 次捕获 =====", cnt);
            ir_print_raw();
            ir_midea_guess();
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void ir_test_start(void)
{
    xTaskCreate(ir_test_task, "ir_test", 4096, NULL, 6, NULL);
}
