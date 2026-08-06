/**
 * 美的(Midea)空调红外控制 —— 编码 + RMT 发射
 *
 * 协议（经手机实测 + 开源资料确认）：
 *   引导 4480/4480us；数据位 mark 560us + space(1=1680us / 0=560us)。
 *   48bit 帧 = [A, ~A, B, ~B, C, ~C]（每字节带反码），发送两次相同帧。
 *   - A = 0xB2 固定用户码
 *   - B = 功能字节（风速）：B = 风速码<<5 | 0x1F
 *         Auto=0xBF, Low=0x9F, Med=0x5F, High=0x3F
 *   - C = 模式/温度：C = 温度编码 & 模式码
 *         温度编码 temp_code = (温度+1)*8 + 7（26°C→0xDF, 24°C→0xCF）
 *         模式码：制冷=0xF0，C = temp_code & 0xF0
 *   - 关机特殊命令：B=0x7B, C=0xE0（开源资料）
 */
#include <stdio.h>
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_types.h"
#include "ir_control.h"

#define IR_TX_GPIO         GPIO_NUM_22
#define RMT_RESOLUTION_HZ  1000000u   /* 1 tick = 1us */
#define IR_CARRIER_HZ      38000u

/* Midea 时序 */
#define MHDR_MARK          4480
#define MHDR_SPACE         4480
#define MBIT_MARK          560
#define MONE_SPACE         1680
#define MZERO_SPACE        560
#define MFOOTER_GAP        5600

/* 每帧符号数：引导 + 48bit + 尾部；两帧 */
#define FRAME_SYMS         (1 + 48 + 1)
#define SYM_MAX            (2 * FRAME_SYMS)

#define MIDEA_USER_CODE    0xB2
#define MIDEA_POWER_OFF_B  0x7B
#define MIDEA_POWER_OFF_C  0xE0
#define MIDEA_MODE_CODE_COOL  0xF0

/* 风速码（对应 MIDEA_FAN_AUTO/LOW/MED/HIGH） */
static const uint8_t s_fan_code[4] = { 5, 4, 2, 1 };

static const char *TAG = "ir_control";
static rmt_channel_handle_t s_tx_chan;
static rmt_encoder_handle_t s_tx_enc;

/* 生成 48bit 状态：[A,~A,B,~B,C,~C]，A 为最高字节（最先发送） */
static uint64_t midea_encode(bool power, uint8_t mode, uint8_t fan, uint8_t temp)
{
    uint8_t A = MIDEA_USER_CODE;
    uint8_t B, C;

    if (!power) {
        B = MIDEA_POWER_OFF_B;
        C = MIDEA_POWER_OFF_C;
    } else {
        B = (uint8_t)((s_fan_code[fan & 3] << 5) | 0x1F);
        uint8_t temp_code = (uint8_t)(((temp + 1) << 3) | 0x07);
        uint8_t mode_code = MIDEA_MODE_CODE_COOL;   /* 当前固定制冷 */
        C = (uint8_t)(temp_code & mode_code);
    }

    return ((uint64_t)A << 40) | ((uint64_t)(uint8_t)~A << 32)
         | ((uint64_t)B << 24) | ((uint64_t)(uint8_t)~B << 16)
         | ((uint64_t)C << 8)  | ((uint64_t)(uint8_t)~C);
}

/* 构建一帧符号：引导 + 48bit(MSB-first) + 尾部 */
static int midea_build_frame(uint64_t data, rmt_symbol_word_t *sym)
{
    int n = 0;
    sym[n++] = (rmt_symbol_word_t){
        .level0 = 1, .duration0 = MHDR_MARK,
        .level1 = 0, .duration1 = MHDR_SPACE,
    };
    for (int i = 8; i <= 48; i += 8) {
        uint8_t seg = (uint8_t)((data >> (48 - i)) & 0xFF);
        for (int j = 7; j >= 0; j--) {
            bool one = (seg >> j) & 1;
            sym[n++] = (rmt_symbol_word_t){
                .level0 = 1, .duration0 = MBIT_MARK,
                .level1 = 0, .duration1 = one ? MONE_SPACE : MZERO_SPACE,
            };
        }
    }
    sym[n++] = (rmt_symbol_word_t){
        .level0 = 1, .duration0 = MBIT_MARK,
        .level1 = 0, .duration1 = MFOOTER_GAP,
    };
    return n;
}

void ir_control_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = SYM_MAX,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_tx_chan));

    rmt_carrier_config_t carrier = {
        .frequency_hz = IR_CARRIER_HZ,
        .duty_cycle = 0.33,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(s_tx_chan, &carrier));
    ESP_ERROR_CHECK(rmt_enable(s_tx_chan));

    ESP_ERROR_CHECK(rmt_new_copy_encoder(&(rmt_copy_encoder_config_t){0}, &s_tx_enc));

    ESP_LOGI(TAG, "Midea IR control init OK (TX=GPIO%d, %uHz carrier)",
             IR_TX_GPIO, IR_CARRIER_HZ);
}

void ir_control_send(bool power, uint8_t mode, uint8_t fan, uint8_t temp)
{
    if (temp < MIDEA_TEMP_MIN) temp = MIDEA_TEMP_MIN;
    if (temp > MIDEA_TEMP_MAX) temp = MIDEA_TEMP_MAX;

    uint64_t data = midea_encode(power, mode, fan, temp);

    rmt_symbol_word_t syms[SYM_MAX];
    int n = midea_build_frame(data, syms);
    n += midea_build_frame(data, syms + n);   /* 两帧相同（手机实测格式） */

    rmt_transmit_config_t tx_conf = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(s_tx_chan, s_tx_enc, syms,
                                 n * sizeof(rmt_symbol_word_t), &tx_conf));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_tx_chan, 2000));

    ESP_LOGI(TAG, "IR send: power=%d fan=%d temp=%d (0x%012llX)",
             power, fan, temp, (unsigned long long)data);
}
