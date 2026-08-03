# 版本记录 (Changelog)

本项目所有重要变更均记录于此。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

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
