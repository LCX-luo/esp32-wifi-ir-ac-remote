# 版本记录 (Changelog)

本项目所有重要变更均记录于此。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [v0.1.5] - 2026-08-03

### 新增
- **红外收发模块测试** `main/ir_test.c`：RMT 外设产生 38kHz 载波发射测试帧（HX-53 发射 @GPIO22），RMT RX 捕获脉冲时序（HX-M121 接收 @GPIO21），验证两块模块是否正常。
- 接线：发射 DAT→GPIO22、接收 DAT→GPIO21，模块 VIN(5V) 供电共地。

### 修复
- RMT TX 通道漏调 `rmt_enable()` 导致 `ESP_ERR_INVALID_STATE` abort，补 enable 后正常（详见 [docs/error-fixes.md](docs/error-fixes.md) 问题 9）。

### 注意
- 接收模块 DAT 输出电平可能为 5V，**不得直连 ESP32 GPIO**（最大 3.6V）；需先测量实际电平，必要时分压/开漏上拉（详见 [docs/error-fixes.md](docs/error-fixes.md) 问题 8）。

## [v0.1.4] - 2026-08-03

### 修复
- 电源指示灯恢复为 **GPIO23 受控输出**（移除调试用 GPIO23 翻转测试任务），`on`/`off` 命令驱动 D23。
- `gpio_set_level` 增加 `ESP_ERROR_CHECK` 返回值检查，GPIO 异常可显式暴露。

### 说明
- 硬件调试确认 D23 即 GPIO23、引脚与驱动 API 均正常；此前「命令到达但灯不亮」实为板载固件为 v0.1.0（无 GPIO 控制代码）所致，详见 [docs/error-fixes.md](docs/error-fixes.md) 问题 7。

## [v0.1.3] - 2026-08-03

### 修复
- **mqtt.js 本地化**：改用仓库内 `web/vendor/mqtt.min.js` 同源加载，规避浏览器"跟踪防护"对第三方 CDN（jsdelivr）的屏蔽（详见 [docs/error-fixes.md](docs/error-fixes.md) 问题 6）。
- 修复 HTML `<label>` 与表单控件未关联的无障碍警告。

### 变更
- 全链路增加 **console 调试输出**：库加载检查、连接各事件、publish 的主题与负载、按钮点击、全局错误捕获，便于定位问题。

## [v0.1.2] - 2026-08-03

### 修复
- **网页遥控器设备 ID base64 编码错误**：`atob('ZThhOGU4')` 解码为 `e8a8e8`（应为 `e9a8e8`），导致命令发布到错误主题、ESP32 收不到（详见 [docs/error-fixes.md](docs/error-fixes.md) 问题 5）。已移除 base64 混淆层，设备 ID 直接为可配置字符串。

### 变更
- **取消访问密码限制**（开发阶段）：网页打开即自动连接默认设备 `e9a8e8`，无需输入密码。设备 ID / Broker / 前缀在"高级设置"中可改。
- 简化发送逻辑：移除 DOM 正则提取设备 ID 的脆弱写法，改用全局变量 `s_devId`。

## [v0.1.1] - 2026-08-03

### 新增
- **电源指示灯控制**：`on`/`off` 命令绑定 GPIO（默认 D23，`AC_LED_GPIO` 可配），红外模块未到前可通过 LED 直观看到远程控制效果。
- **网页版遥控器重构**：白色 + 淡蓝色系、简洁移动端布局；默认隐藏 Broker/主题前缀等高级设置（可折叠展开）。
- **访问密码登录**：输入密码 `060718` 自动连接设备 `e9a8e8`，无需手动输入设备 ID；设备 ID 以 base64 混淆存储于前端（演示级保护，见 README 安全说明）。
- **GitHub Pages 托管**：网页遥控器通过 `gh-pages` 分支部署，手机/电脑浏览器打开网址即可用。

### 修复
- `main.c` 使用 `driver/gpio.h` 但未声明 `driver` 组件依赖，导致编译失败（详见 [docs/error-fixes.md](docs/error-fixes.md)）。

### 变更
- `on`/`off` 命令现在同时控制电源指示灯（原为仅日志）；`mode`/`temp`/`fan` 仍仅解析记录，待红外模块接入。

## [v0.1.0] - 2026-08-03

### 新增
- **WiFi STA 连接**：连路由器，断线自动重连（`wifi_event_handler`）。
- **MQTT 客户端**：连接公共免费 Broker（默认 `broker.emqx.io`），订阅命令主题、发布状态主题。
- **设备唯一标识**：以 MAC 地址后 3 字节生成 `device_id`，主题为 `{prefix}/{id}/cmd` 与 `{prefix}/{id}/status`，避免公共 Broker 主题冲突。
- **JSON 命令解析**：cJSON 解析收到的指令，`app_command_execute()` 命令分发（当前仅串口打印，预留红外接口）。
- **网页版遥控器** `web/remote_control.html`：手机/电脑浏览器通过 WebSocket (WSS) 连接 Broker，按钮发命令、实时收状态，无需安装 App。
- **menuconfig 参数化** `main/Kconfig.projbuild`：WiFi / MQTT / 主题前缀均可图形化配置，改配置不重新编译代码。
- **文档体系**：README（架构 + 协议选型对比）、architecture、error-fixes、CHANGELOG。

### 修复
- VSCode IntelliSense 报 `freertos/FreeRTOS.h` 无法打开（详见 [docs/error-fixes.md](docs/error-fixes.md)）。

### 说明
- 本版本为「远程控制链路」验证版：收到命令仅打印到串口日志，不驱动任何硬件。
- 公共 Broker 明文传输，仅供开发演示，生产需 TLS + 认证（见 README「安全说明」）。
