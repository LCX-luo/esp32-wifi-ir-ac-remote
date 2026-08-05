/**
 * 美的(Midea)空调红外控制 —— 编码 + RMT 发射
 *
 * 48bit 状态布局（字节 0 为最低字节，校验和除外，见注释）：
 *   byte0 = Sum       校验和
 *   byte1 = SensorTemp:7 | disableSensor:1      （禁用：0xFF）
 *   byte2 = :1 | OffTimer:6 | BeepDisable:1     （关：0xFF）
 *   byte3 = Temp:5 | useFahrenheit:1 | :0       （Celsius: useFahrenheit=0）
 *   byte4 = Mode:3 | Fan:2 | :1 | Sleep:1 | Power:1
 *   byte5 = Type:3 | Header:5                   （Type=Command(1), Header=0b10100）
 *
 * 发送时序（IRremoteESP8266 sendMidea）：
 *   [引导 4480/4480] + 48bit(byte5..byte0, MSB-first) + [尾部 560/5600]
 *   然后 data=~data 再发一次相同结构（双重），帧末大间隔。
 */
#include <stdio.h>
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_types.h"
#include "ir_control.h"

#define IR_TX_GPIO         GPIO_NUM_22
#define RMT_RESOLUTION_HZ  1000000u   /* 1 tick = 1us */
#define IR_CARRIER_HZ      38000u

/* Midea 时序（tick=80us 换算） */
#define MHDR_MARK          4480
#define MHDR_SPACE         4480
#define MBIT_MARK          560
#define MONE_SPACE         1680
#define MZERO_SPACE        560
#define MFOOTER_GAP        5600

#define MIDA_HEADER        (0b10100)  /* 固定头部 */
#define MIDA_TYPE_COMMAND  (0b001)

/* 每帧符号数：引导 + 48bit + 尾部；双帧 */
#define FRAME_SYMS         (1 + 48 + 1)
#define SYM_MAX            (2 * FRAME_SYMS)

static const char *TAG = "ir_control";

static rmt_channel_handle_t  s_tx_chan;
static rmt_encoder_handle_t  s_tx_enc;

static uint8_t reverse_bits(uint8_t v)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        r = (uint8_t)((r << 1) | (v & 1));
        v >>= 1;
    }
    return r;
}

/* 由 power/mode/fan/temp 编码 48bit 状态 */
static uint64_t midea_encode(bool power, uint8_t mode, uint8_t fan, uint8_t temp)
{
    uint8_t b[6] = {0};
    b[1] = 0xFF;                                   /* SensorTemp 禁用 */
    b[2] = 0xFF;                                   /* OffTimer 关 */
    b[3] = (uint8_t)((temp - MIDEA_TEMP_MIN) & 0x1F);  /* Celsius */
    b[4] = (uint8_t)((power ? 0x80 : 0) | ((fan & 0x3) << 3) | (mode & 0x7));
    b[5] = (uint8_t)((MIDA_HEADER << 3) | MIDA_TYPE_COMMAND);

    /* 校验和：后 5 字节 reverse-bits 求和，256-sum 后 reverse-bits */
    uint8_t sum = 0;
    for (int i = 5; i >= 1; i--) {
        sum = (uint8_t)(sum + reverse_bits(b[i]));
    }
    sum = (uint8_t)(256 - sum);
    b[0] = reverse_bits(sum);

    uint64_t state = 0;
    for (int i = 0; i < 6; i++) {
        state |= (uint64_t)b[i] << (8 * i);
    }
    return state;
}

/* 把一帧（data 的 48bit）转成 RMT 符号：引导 + 48bit(MSB-first) + 尾部 */
static int midea_build_frame(uint64_t data, rmt_symbol_word_t *sym)
{
    int n = 0;
    sym[n++] = (rmt_symbol_word_t){
        .level0 = 1, .duration0 = MHDR_MARK,
        .level1 = 0, .duration1 = MHDR_SPACE,
    };
    /* 从最高字节到最低字节，每字节 MSB-first */
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
    n += midea_build_frame(~data, syms + n);

    rmt_transmit_config_t tx_conf = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(s_tx_chan, s_tx_enc, syms,
                                 n * sizeof(rmt_symbol_word_t), &tx_conf));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_tx_chan, 2000));

    ESP_LOGI(TAG, "IR send: power=%d mode=%d fan=%d temp=%d (0x%012llX, %d syms)",
             power, mode, fan, temp, (unsigned long long)data, n);
}
