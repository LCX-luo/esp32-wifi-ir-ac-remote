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
#define IR_RX_BUF_MAX      256        /* Midea 双重帧约 100 符号，留余量防截断 */
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

/* 把 48bit state 解码为可读命令（电源/模式/风速/温度），供人眼对比 */
static void ir_print_command(uint64_t state)
{
    uint8_t b3 = (state >> 24) & 0xFF;   /* Temp:5 + useFahrenheit:1 + :0 */
    uint8_t b4 = (state >> 32) & 0xFF;   /* Mode:3 + Fan:2 + :1 + Sleep:1 + Power:1 */
    uint8_t b5 = (state >> 40) & 0xFF;   /* Type:3 + Header:5 */

    bool    power    = (b4 >> 7) & 1;
    bool    sleep    = (b4 >> 6) & 1;
    uint8_t fan      = (b4 >> 3) & 0x3;
    uint8_t mode     = b4 & 0x7;
    uint8_t temp_raw = b3 & 0x1F;
    bool    useF     = (b3 >> 5) & 1;
    int     temp     = useF ? (temp_raw + 62) : (temp_raw + 17);
    uint8_t type     = b5 & 0x7;
    uint8_t header   = (b5 >> 3) & 0x1F;

    const char *mode_s = mode == 0 ? "制冷" : mode == 1 ? "除湿" : mode == 2 ? "自动" :
                         mode == 3 ? "制热" : mode == 4 ? "送风" : "未知";
    const char *fan_s  = fan == 0 ? "自动" : fan == 1 ? "低" : fan == 2 ? "中" : "高";

    ESP_LOGI(TAG, "[CMD] 电源=%s 模式=%s(%d) 风速=%s(%d) 温度=%d%s 睡眠=%s Type=%d Header=%d",
             power ? "开" : "关", mode_s, mode, fan_s, fan, temp, useF ? "F" : "C",
             sleep ? "开" : "关", type, header);
}

/* 解析一段（引导码后到帧间隔前）的 48 bit 数据为 A/B/C，并做反码校验 */
static void ir_parse_segment(int start, int end)
{
    int nbits = 0;
    char bits[64] = {0};
    for (int i = start + 1; i < end && nbits < 48; i++) {
        if (s_rx_buf[i].duration1 < 300) {
            break;
        }
        bits[nbits++] = (s_rx_buf[i].duration1 > 1000) ? '1' : '0';
    }
    if (nbits < 48) {
        ESP_LOGW(TAG, "[MIDEA] 段数据不足(%d bit, 期望48)", nbits);
        return;
    }

    /* 48 bit = 6 字节 [A,~A,B,~B,C,~C]，MSB first */
    uint8_t bytes[6] = {0};
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 8; j++) {
            if (bits[i * 8 + j] == '1') {
                bytes[i] |= (1u << (7 - j));
            }
        }
    }

    bool ok = (bytes[1] == (uint8_t)~bytes[0]) &&
              (bytes[3] == (uint8_t)~bytes[2]) &&
              (bytes[5] == (uint8_t)~bytes[4]);
    ESP_LOGI(TAG, "[MIDEA] A=0x%02X B=0x%02X C=0x%02X  反码校验:%s",
             bytes[0], bytes[2], bytes[4], ok ? "OK" : "FAIL");
    /* 重建 remote_state（发送顺序 byte5..byte0，字节0 在最低位），供与发射端对比 */
    uint64_t state = 0;
    for (int i = 0; i < 6; i++) {
        state |= (uint64_t)bytes[5 - i] << (8 * i);
    }
    ESP_LOGI(TAG, "[MIDEA] state=0x%012llX (%s)",
             (unsigned long long)state, ok ? "valid" : "invalid");
    if (ok) {
        ir_print_command(state);
    }
}

/* 分段解析：识别 Midea 双重帧（L + 48bit + S间隔 + L + 48bit）。
 * 引导码特征：H 长脉冲(>3000us)，且引导自身 L(~4480)也>2000 —— 因此
 * 段起始只看 H，帧分隔看"数据符号之后出现的 L>2000"。 */
static void ir_midea_parse(void)
{
    ESP_LOGI(TAG, "[MIDEA] 分段解析:");
    int seg_start = -1;
    for (int i = 0; i < (int)s_rx_num; i++) {
        bool is_hdr = (s_rx_buf[i].duration0 > 3000);   /* 引导码：H 长脉冲 */
        bool is_gap = (s_rx_buf[i].duration1 > 2000);   /* 长间隔：帧分隔/帧尾 */

        if (seg_start >= 0) {
            if (is_gap && i > seg_start + 1) {          /* 段内有数据后遇长间隔 → 段结束 */
                ir_parse_segment(seg_start, i);
                seg_start = -1;
            } else if (is_hdr) {                        /* 新引导出现，收尾上一段 */
                ir_parse_segment(seg_start, i);
                seg_start = i;
            }
        }
        if (seg_start < 0 && is_hdr) {
            seg_start = i;
        }
    }
    if (seg_start >= 0) {
        ir_parse_segment(seg_start, (int)s_rx_num);
    }
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

    ESP_LOGI(TAG, "红外监听模式启动：接收模块(GPIO%d)常开；发射时对准可回看，手机红外对着可捕获解析",
             IR_RX_GPIO);

    int cnt = 0;
    uint32_t evt;
    while (1) {
        ESP_ERROR_CHECK(rmt_receive(rx_chan, s_rx_buf, sizeof(s_rx_buf), &rx_recv_cfg));

        if (xQueueReceive(s_ir_queue, &evt, pdMS_TO_TICKS(IR_RX_TIMEOUT_MS)) == pdTRUE) {
            cnt++;
            ESP_LOGI(TAG, "===== 第 %d 次捕获 =====", cnt);
            ir_print_raw();
            ir_midea_parse();
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void ir_test_start(void)
{
    xTaskCreate(ir_test_task, "ir_test", 4096, NULL, 6, NULL);
}
