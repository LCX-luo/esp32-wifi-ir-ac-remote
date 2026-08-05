/**
 * 红外收发模块测试（临时）
 *
 * 硬件连接：
 *   - 发射模块 HX-53   DAT -> GPIO22（ESP32 输出，RMT TX）
 *   - 接收模块 HX-M121 DAT -> GPIO21（注意需分压：5V -> 3.3V，勿直连！）
 *   - 两模块共用 VIN(5V) 与 GND
 *
 * 工作原理（回答"能否同时收发 / 用 DMA"问题）：
 *   RMT TX 硬件自动产生 38kHz 载波 + 脉冲序列，CPU 只准备数据；
 *   RMT RX 硬件自动捕获边沿时序（底层 DMA），CPU 只在收完一帧后的回调里解析。
 *   "学习"(只收) 与 "回放"(只发) 本就分时，无需同时收发。
 *
 * 测试流程：每约 2 秒发射一帧 NEC 引导码测试信号，同时 RMT RX 持续监听；
 *   将发射模块对准接收模块（<30cm），串口应打印捕获到的脉冲时序。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_attr.h"
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

    /* 测试帧：NEC 引导码 + 几个数据位（任意时序，仅用于验证链路） */
    rmt_symbol_word_t tx_syms[] = {
        { .level0 = 1, .duration0 = 9000, .level1 = 0, .duration1 = 4500 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560 },
    };
    rmt_transmit_config_t tx_conf = {
        .loop_count = 0,
    };

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

    s_ir_queue = xQueueCreate(8, sizeof(uint32_t));
    rmt_rx_event_callbacks_t rx_cbs = { .on_recv_done = ir_rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &rx_cbs, NULL));

    rmt_receive_config_t rx_recv_cfg = {
        .signal_range_min_ns = 1250,     /* 忽略 <1.25us 毛刺 */
        .signal_range_max_ns = 12000000, /* 空闲 >12ms 认为一帧结束 */
    };

    ESP_LOGI(TAG, "IR test started: TX=GPIO%d RX=GPIO%d, carrier=%uHz",
             IR_TX_GPIO, IR_RX_GPIO, IR_CARRIER_HZ);
    ESP_LOGI(TAG, "请将发射模块对准接收模块(<30cm)，并观察发射模块板载LED");

    int cnt = 0;
    uint32_t evt;
    while (1) {
        /* 重新开启接收 */
        ESP_ERROR_CHECK(rmt_receive(rx_chan, s_rx_buf, sizeof(s_rx_buf), &rx_recv_cfg));

        /* 发射一帧测试信号 */
        cnt++;
        ESP_LOGI(TAG, "TX #%d: sending test frame...", cnt);
        ESP_ERROR_CHECK(rmt_transmit(tx_chan, copy_enc, tx_syms, sizeof(tx_syms), &tx_conf));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_chan, 1000));

        /* 等待接收结果 */
        if (xQueueReceive(s_ir_queue, &evt, pdMS_TO_TICKS(IR_RX_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "RX #%d: captured %d symbols:", cnt, (int)s_rx_num);
            for (int i = 0; i < (int)s_rx_num; i++) {
                ESP_LOGI(TAG, "  [%02d] HIGH=%u us  LOW=%u us",
                         i, s_rx_buf[i].duration0, s_rx_buf[i].duration1);
            }
        } else {
            ESP_LOGW(TAG, "RX #%d: no signal within %dms (check alignment/wiring)",
                     cnt, IR_RX_TIMEOUT_MS);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void ir_test_start(void)
{
    xTaskCreate(ir_test_task, "ir_test", 4096, NULL, 6, NULL);
}
