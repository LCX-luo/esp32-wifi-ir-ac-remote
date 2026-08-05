#pragma once

/**
 * 红外收发模块测试（临时）：验证 HX-M121(接收) + HX-53(发射) 是否正常。
 * 通过 RMT 外设产生 38kHz 载波发射测试帧，并用 RMT RX 捕获接收到的时序。
 */
void ir_test_start(void);
