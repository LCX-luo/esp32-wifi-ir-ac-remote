# 架构设计

## 1. 系统总览

本项目是一个典型的 **物联网三端架构**：控制端（网页）、云端（MQTT Broker）、设备端（ESP32）。

```
┌─────────────┐     publish      ┌──────────────┐     subscribe     ┌─────────────┐
│  控制端      │ ──cmd 主题──────▶│  云端 Broker  │ ───转发命令──────▶│  设备端      │
│ 浏览器网页   │                 │ broker.emqx  │                   │  ESP32       │
│  mqtt.js    │ ◀──status 主题───│     .io       │ ◀───发布状态─────│  WiFi+MQTT   │
└─────────────┘     subscribe    └──────────────┘     publish       └─────────────┘
```

**解耦的价值**：三个部分之间只约定「主题名 + 消息格式」，互不知道对方网络地址。任何一端离线都不影响其他端，Broker 负责中转、持久化（保留消息）、断线通知（遗嘱消息）。这就是「全网远程控制」不需要内网穿透的根本原因。

## 2. 设备端代码结构（main/main.c）

单文件、按职责分节，保证入门可读性；后续红外模块接入时按需拆分。

```
app_main()
├── nvs_flash_init()                  # NVS 持久化（WiFi 必需）
├── esp_netif_init()                  # 网络接口层
├── esp_event_loop_create_default()   # 事件循环（WiFi/IP/MQTT 事件）
├── app_build_identity()              # 生成 device_id + 主题
└── wifi_init_sta()
    ├── esp_netif_create_default_wifi_sta()
    ├── esp_wifi_init / set_mode / set_config / start
    └── 事件回调 wifi_event_handler()
        ├── WIFI_EVENT_STA_START     → esp_wifi_connect()
        ├── WIFI_EVENT_STA_DISCONNECTED → esp_wifi_connect()  断线重连
        └── IP_EVENT_STA_GOT_IP      → mqtt_app_start()       拿 IP 后启动 MQTT
            ├── esp_mqtt_client_init / register_event / start
            └── 事件回调 mqtt_event_handler()
                ├── MQTT_EVENT_CONNECTED → 订阅 cmd + 发布 online(保留)
                ├── MQTT_EVENT_DATA     → cJSON 解析 → app_command_execute()
                └── MQTT_EVENT_DISCONNECTED → 客户端自动重连
```

### 关键设计决策

| 决策 | 理由 |
|---|---|
| **参数走 menuconfig**（`Kconfig.projbuild`） | 改 WiFi/MQTT 配置不需要改代码重编译，也可通过 `sdkconfig.defaults` 固化默认值，符合工程规范 |
| **device_id 取 MAC 后 3 字节** | 每台设备天然唯一，公共 Broker 上主题不冲突；客户端 ID 复用同一值，避免与同名客户端踢线 |
| **status 用保留消息 (retain)** | 控制端随时上线都能立刻收到设备最近一次状态，无需设备在线时才可查询 |
| **命令分发独立函数** `app_command_execute()` | 把「通信」和「执行」解耦：红外模块到位后只需替换函数内部实现，通信层不动 |
| **MQTT 在 GOT_IP 后才启动** | 保证有网络时再连接 Broker，避免无谓重试 |

## 3. 通信协议设计

### 主题命名

```
{prefix}/{device_id}/cmd        # 下行：控制端 → ESP32（QoS 1）
{prefix}/{device_id}/status     # 上行：ESP32 → 控制端（QoS 1 + retain）
```

- `prefix` 默认 `/ac-remote`，可多设备/多用户共存。
- `device_id` 为 MAC 后 3 字节 hex（如 `3c71bf`）。

### 消息格式（JSON）

| 方向 | 示例 | 说明 |
|---|---|---|
| 命令 | `{"cmd":"on"}` | 电源开 |
| 命令 | `{"cmd":"mode","mode":"cool"}` | 模式 auto/cool/heat/dry/fan |
| 命令 | `{"cmd":"temp","temp":25}` | 目标温度 16~30 |
| 命令 | `{"cmd":"fan","fan":"high"}` | 风速 auto/low/mid/high |
| 状态 | `{"state":"online","device_id":"3c71bf"}` | 上线通知（保留） |

选 JSON 而非自定义二进制：人类可读、调试方便、生态成熟（cJSON 解析，网页端 `JSON.stringify` 直出），对空调控制这种低频小消息完全够用。

## 4. 控制端（web/remote_control.html）

- 单文件 HTML，通过 CDN 引入 `mqtt.js`，以 **WebSocket (WSS)** 连接 Broker（`wss://broker.emqx.io:8084/mqtt`）。
- 浏览器做 MQTT 客户端：订阅 status 主题收状态，向 cmd 主题发命令。
- 零部署：手机/电脑浏览器打开即用；未来可复用同一协议开发原生 App。

## 5. 安全与可靠性

- 当前为**公共 Broker + 明文 MQTT**，属于开发演示级别；生产需升级为 TLS + 认证 + 私密 Broker（README「安全说明」）。
- 可靠性：WiFi 断线自动重连、MQTT 客户端内置自动重连、命令 QoS 1（至少一次投递）。

### 安全演进规划（红外模块 v0.2.0 之后）

**核心约束**：安全的本质是「控制端持有设备可验证的、不公开的秘密」。公开 GitHub Pages 托管的前端代码对所有人透明，其中的密码/设备 ID/broker 凭据均可被查看——**前端密码在公开网页上无法提供真实防护**；且公共 broker 允许任何人连接并订阅任意主题，ESP32 无法区分命令来源。

推荐演进（三步一起做，缺一不可）：
1. **私有 Broker + 账号认证**：自建（Mosquitto/EMQX）或云托管（EMQX Serverless 免费额度），开启 username/password + TLS（`mqtts://`），无凭据者无法接入。
2. **设备端命令签名**：ESP32 内置 HMAC 密钥，命令负载带时间戳 + 签名，设备验签通过才执行（防伪造/防重放）；密钥仅存在于设备与私有控制端，不进入任何公开页面。
3. **控制端私密化**：控制端迁出公开托管——手机 App（密钥在 App 内）或私有带登录的 Web 服务。公开网页控制端与「可控性安全」不可兼得。

当前演示环境（公共 broker + 无鉴权 + 公开网页）仅适合开发/受信网络，不能作为远程生产方案。

## 6. 红外协议获取策略（v0.2.0 设计决策）

空调红外**底层调制是标准的（38kHz 载波）**，但**数据帧是厂商私有协议**（格式/长度/校验各品牌不同），且多数国内品牌无公开完整资料。因此码的获取策略：

1. **优先搜索开源码库**：Arduino IRremote、ESPHome climate_ir、GitHub 空调码库。能找到品牌/型号的码表则零成本复用。
2. **兜底方案——红外接收头学习**（推荐主线，通用可靠）：
   - 接收：红外接收头（VS1838B 等）+ ESP32 **RMT RX**，抓取手机红外 App / 原装遥控器每个按钮的 **raw 脉冲时序**。
   - 存储：raw 序列存入 NVS，绑定到命令（on/off/mode/temp/fan）。
   - 发送：ESP32 **RMT TX** 原样重放 raw（含载波调制），不依赖对厂商协议的 bit 级解码，对任意品牌通用。
   - 驱动：发射端用 940nm 红外 LED + 三极管放大（GPIO 直驱电流不足）。
3. **命令流集成**：`app_command_execute()` 升级为「查命令→码映射表 → RMT 发送」，MQTT 可增加 `learn` 命令触发学习流程。

## 7. 演进方向

- **v0.2.0**：红外模块 → RMT 学习/回放，`app_command_execute()` 升级为查码表发送（见上节）。
- **v1.0.0**：App 控制端、TLS、OTA。红外码库可通过主题下发（如 `{"cmd":"raw","data":"0x..."}`）。
