/**
 * 红外收发模块数据传输测试（临时）
 *
 * 硬件连接：
 *   - 发射模块 HX-53   DAT -> GPIO22（ESP32 输出，RMT TX）
 *   - 接收模块 HX-M121 DAT -> GPIO21（注意：DAT 输出 5V！见下方适配说明）
 *   - 两模块共用 VIN(5V) 与 GND
 *
 * 5V 输入适配说明：
 *   - ESP32 GPIO 最大输入 3.6V，5V 属物理超压，纯软件无法完全消除风险；
 *   - 本代码将 GPIO21 配置为"输入 + 内部下拉"：对开漏型接收头可将空闲 5V
 *     分压降低（较安全）；若模块为推挽 5V 输出，内部下拉无效仍有风险；
 *   - 强烈建议串接红色 LED：DAT -> LED阳极 -> LED阴极 -> GPIO21，
 *     利用 LED ~1.8V 压降使 GPIO 端约 3.2V，最稳妥。
 *
 * 测试内容：每 2 秒发射一帧「引导码 + 8bit 数据(0xA5)」，RMT RX 捕获后
 *   解码成字节并与发射数据比对，打印 MATCH / MISMATCH，验证数据传输可行性。
 *
 * RMT 原理：TX 硬件自动产生 38kHz 载波+脉冲；RX 硬件自动捕获时序(DMA)，
 *   CPU 仅在收完一帧后的回调里解析。学习(只收)/回放(只发)本就分时，无需同时收发。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_types.h"
#include "ir_test.h"

#define IR_TX_GPIO         GPIO_NUM_22
#define IR_RX_GPIO         GPIO_NUM_21
#define IR_CARRIER_HZ      38000u
#define RMT_RESOLUTION_HZ  1000000u   /* 1 tick = 1us，duration 单位即 us */
#define IR_RX_BUF_MAX      64         /* 最多捕获 64 个符号 */
#define IR_RX_TIMEOUT_MS   4000

#define IR_TX_DATA         0xA5       /* 测试数据字节：1010 0101 (LSB first) */

static const char *TAG = "ir_test";

static QueueHandle_t s_ir_queue;
static rmt_symbol_word_t s_rx_buf[IR_RX_BUF_MAX];
static volatile size_t s_rx_num = 0;

/* RX 完成回调：运行在中断上下文，只拷贝计数并通过队列通知任务解析 */
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

/* 从引导码后的符号解码 8 bit（LSB first）：低电平时长 >1000us 视为 1，否则 0 */
static uint8_t ir_decode_data(void)
{
    uint8_t val = 0;
    if (s_rx_num < 1 + 8) {
        return 0xFF;   /* 数据不足 */
    }
    for (int i = 0; i < 8; i++) {
        if (s_rx_buf[1 + i].duration1 > 1000) {
            val |= (1u << i);
        }
    }
    return val;
}

static void ir_test_task(void *arg)
{
    /* ---- 发射通道 ---- */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    rmt_channel_handle_t tx_chan = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &tx_chan));

    /* 38kHz 载波，占空比 1/3 */
    rmt_carrier_config_t carrier = {
        .frequency_hz = IR_CARRIER_HZ,
        .duty_cycle = 0.33,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_chan, &carrier));
    ESP_ERROR_CHECK(rmt_enable(tx_chan));   /* TX 通道必须先 enable 才能 transmit */

    rmt_encoder_handle_t copy_enc = NULL;
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&(rmt_copy_encoder_config_t){0}, &copy_enc));

    /* 测试帧：NEC 引导码 + 8bit 数据(LSB first) + 结束符号(560/0)
     * 结束符号的作用：让最后一段数据位的低电平后面紧跟一个高边沿，
     * 否则 RMT RX 会把帧尾长低电平当成"空闲超时"截断（LOW=0），丢失最后一位。 */
    #define TX_SYM_NUM  (1 + 8 + 1)
    rmt_symbol_word_t tx_syms[TX_SYM_NUM];
    tx_syms[0] = (rmt_symbol_word_t){
        .level0 = 1, .duration0 = 9000,
        .level1 = 0, .duration1 = 4500,
    };
    for (int i = 0; i < 8; i++) {
        int bit = (IR_TX_DATA >> i) & 1;
        tx_syms[1 + i] = (rmt_symbol_word_t){
            .level0 = 1, .duration0 = 560,
            .level1 = 0, .duration1 = bit ? 1690 : 560,
        };
    }
    tx_syms[TX_SYM_NUM - 1] = (rmt_symbol_word_t){
        .level0 = 1, .duration0 = 560,
        .level1 = 0, .duration1 = 0,
    };
    rmt_transmit_config_t tx_conf = { .loop_count = 0 };

    /* ---- 接收通道 ---- */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_BUF_MAX,
    };
    rmt_channel_handle_t rx_chan = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx_chan));
    ESP_ERROR_CHECK(rmt_enable(rx_chan));

    /* 适配 5V DAT：GPIO 输入 + 内部下拉（对开漏型接收头分压降低空闲电平）。
     * 若为推挽 5V 输出，建议串接红色 LED 限压后再接本引脚。 */
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
        .signal_range_min_ns = 1250,     /* 忽略 <1.25us 毛刺 */
        .signal_range_max_ns = 12000000, /* 空闲 >12ms 认为一帧结束 */
    };

    ESP_LOGI(TAG, "IR data test: TX=GPIO%d RX=GPIO%d carrier=%uHz, data=0x%02X",
             IR_TX_GPIO, IR_RX_GPIO, IR_CARRIER_HZ, IR_TX_DATA);
    ESP_LOGI(TAG, "发射模块对准接收模块(<30cm)，观察是否 MATCH");

    int cnt = 0;
    uint32_t evt;
    while (1) {
        /* 重新开启接收 */
        ESP_ERROR_CHECK(rmt_receive(rx_chan, s_rx_buf, sizeof(s_rx_buf), &rx_recv_cfg));

        /* 发射一帧 */
        cnt++;
        ESP_ERROR_CHECK(rmt_transmit(tx_chan, copy_enc, tx_syms, sizeof(tx_syms), &tx_conf));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_chan, 1000));

        /* 同时打印发射帧（便于人眼对比） */
        ESP_LOGI(TAG, "[TX] #%d frame (%d syms):", cnt, TX_SYM_NUM);
        for (int i = 0; i < TX_SYM_NUM; i++) {
            ESP_LOGI(TAG, "  [%02d] %u/%u", i,
                     tx_syms[i].duration0, tx_syms[i].duration1);
        }

        /* 等待接收结果，按阶段诊断 */
        if (xQueueReceive(s_ir_queue, &evt, pdMS_TO_TICKS(IR_RX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "[DIAG] 未收到信号：检查发射/接收模块是否对准、供电、DAT 接线");
        } else {
            ESP_LOGI(TAG, "[RX] #%d got %d syms:", cnt, (int)s_rx_num);
            for (int i = 0; i < (int)s_rx_num; i++) {
                ESP_LOGI(TAG, "  [%02d] %u/%u",
                         i, s_rx_buf[i].duration0, s_rx_buf[i].duration1);
            }

            if (s_rx_num < 1 + 8) {
                ESP_LOGW(TAG, "[DIAG] 帧不完整(仅 %d 个符号)：信号弱/距离太远/被截断",
                         (int)s_rx_num);
            } else if (s_rx_num > TX_SYM_NUM + 3) {
                ESP_LOGW(TAG, "[DIAG] 符号数异常(%d) 疑似杂音过多：环境红外干扰/自串扰/对准不佳",
                         (int)s_rx_num);
            } else {
                uint8_t got = ir_decode_data();
                bool match = (got == IR_TX_DATA);
                ESP_LOGI(TAG, "[RESULT] decoded=0x%02X expect=0x%02X -> %s",
                         got, IR_TX_DATA, match ? "MATCH" : "MISMATCH");
                if (!match) {
                    ESP_LOGW(TAG, "[DIAG] 数据不一致：请对比上方 [TX] 与 [RX] 符号序列定位差异位");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void ir_test_start(void)
{
    xTaskCreate(ir_test_task, "ir_test", 4096, NULL, 6, NULL);
}
