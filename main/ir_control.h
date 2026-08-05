#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 美的(Midea)空调红外控制模块
 * 通过 RMT 外设产生 38kHz 载波发射 Midea 协议帧。
 *
 * 协议（经 IRremoteESP8266 逆向 + 实测确认）：
 *   引导 4480/4480us；数据位 mark 560us + space(1=1680us / 0=560us)；
 *   48bit = [Sum, SensorTemp, OffTimer, Temp+useF, Mode/Fan/Power, Type+Header]；
 *   发送 data 帧后再发 ~data 帧（双重）。
 */

/* 模式（Midea） */
#define MIDEA_MODE_COOL  0
#define MIDEA_MODE_DRY   1
#define MIDEA_MODE_AUTO  2
#define MIDEA_MODE_HEAT  3
#define MIDEA_MODE_FAN   4

/* 风速（Midea） */
#define MIDEA_FAN_AUTO   0
#define MIDEA_FAN_LOW    1
#define MIDEA_FAN_MED    2
#define MIDEA_FAN_HIGH   3

/* 温度范围 */
#define MIDEA_TEMP_MIN   17
#define MIDEA_TEMP_MAX   30

/** 初始化红外发射通道（GPIO22, 38kHz 载波） */
void ir_control_init(void);

/**
 * 发送一条 Midea 命令。
 * @param power 电源开关
 * @param mode  MIDEA_MODE_*
 * @param fan   MIDEA_FAN_*
 * @param temp  温度 17~30°C
 */
void ir_control_send(bool power, uint8_t mode, uint8_t fan, uint8_t temp);
