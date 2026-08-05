# 错误修复报告

> 记录开发过程中遇到的所有问题、根因与解决方案，作为持续改进与简历素材。
> 每条记录格式：现象 → 根因 → 解决 → 经验。

---

## 2026-08-03

### 问题 1：VSCode IntelliSense 报 `freertos/FreeRTOS.h` 无法打开（error 1696）

- **现象**：`main.c` 第 2 行 `#include "freertos/FreeRTOS.h"` 报 `1696 无法打开 源 文件`，但 `idf.py` 编译完全正常。
- **根因**：编译成功是因为 ESP-IDF 构建系统（CMake）自己知道所有 include 路径；而 VSCode C/C++ 插件的 IntelliSense 引擎并不读取构建系统，它需要单独的配置（`c_cpp_properties.json`）才知道去哪里找头文件。项目里没有这个文件，所以它按默认路径找不到 `components/freertos/FreeRTOS-Kernel/include` 下的 `freertos/` 目录。
- **解决**：新建 `.vscode/c_cpp_properties.json`，让 IntelliSense 直接读取构建产物 `build/compile_commands.json`（其中包含编译每个文件时的全部 `-I` include 路径），无需手工维护路径列表：

  ```json
  {
      "configurations": [
          {
              "name": "ESP-IDF",
              "compileCommands": "${workspaceFolder}/build/compile_commands.json",
              "compilerPath": "C:/Espressif/tools/xtensa-esp32-elf/esp-12.2.0_20230208/xtensa-esp32-elf/bin/xtensa-esp32-elf-gcc.exe",
              "intelliSenseMode": "linux-gcc-x64",
              "cStandard": "c17",
              "cppStandard": "c++17"
          }
      ],
      "version": 4
  }
  ```

  构建后 `compile_commands.json` 会随编译自动更新，新增组件（如 esp_wifi、mqtt）无需再手动配置。
- **经验**：ESP-IDF 的「编译能过」和「IDE 智能提示能过」是两套独立系统。接入 IDE 时优先让 IntelliSense 复用编译数据库（`compileCommands`），而非手写 include 路径。

### 问题 2：Git Bash 中无法运行 `idf.py build`（MSYS 环境不支持）

- **现象**：在 Git Bash 里 `source export.sh` 报 `ERROR: MSys/Mingw is not supported`，即使 `unset MSYSTEM` 也无法绕过（Git Bash 每次启动都会重新注入 `MSYSTEM=MINGW64` 环境变量，`idf_tools.py` 检测到即拒绝）。
- **根因**：ESP-IDF 的官方支持环境是 Windows 原生（PowerShell / CMD）或 WSL / Linux；MSYS / MinGW 因路径转换和工具链兼容性问题不被官方支持。VSCode 的 ESP-IDF 扩展实际是在 Windows 原生环境（CMD）中调用 `idf.py` 的，所以扩展内编译正常。
- **解决**：**统一使用 VSCode ESP-IDF 扩展编译烧录**（`Ctrl+E, B` 编译 / `Ctrl+E, D` 烧录），不依赖命令行。命令行用户在 PowerShell 中通过 `%USERPROFILE%\esp-idf-v5.1.2\export.ps1` 初始化环境。
- **经验**：Windows 上开发 ESP-IDF，IDE 扩展（原生环境）比 Git Bash 命令行更可靠；不要试图在 MSYS 里跑官方工具链。

### 问题 3：`esp_mqtt_client_config_t` 结构在新版中嵌套变化

- **现象**：早期写法 `.broker.address.uri = ...` 之后的字段（用户名/密码）参考旧版代码写成 `.username = ...`，无法编译。
- **根因**：ESP-IDF 5.x 将 MQTT 配置重构为嵌套结构：地址在 `broker.address`，认证在 `credentials.username` / `credentials.authentication.password`，会话参数在 `session.keepalive`。
- **解决**：对照 `components/mqtt/esp-mqtt/include/mqtt_client.h` 中的结构体定义确认字段层级后修正。
- **经验**：ESP-IDF 大版本升级常有 API 结构变化，写配置代码前先核对目标版本头文件，而不是照抄旧示例。

### 问题 4：使用 GPIO API 报 "driver component is not in the requirements list"

- **现象**：`main.c` 新增 `#include "driver/gpio.h"`（`gpio_config` / `gpio_set_level`）后编译失败，提示 `driver component(s) is not in the requirements list of "main"`。
- **根因**：ESP-IDF 的组件依赖是**显式声明**的——`main` 组件的 `idf_component_register()` 里没有把 `driver` 加入 `REQUIRES`，构建系统拒绝让 `main.c` 包含 `driver` 组件的头文件。
- **解决**：在 `main/CMakeLists.txt` 的 `REQUIRES` 中追加 `driver`：

  ```cmake
  idf_component_register(SRCS "main.c"
                      INCLUDE_DIRS "."
                      REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver)
  ```
- **经验**：每引入一个新组件（头文件在 `components/xxx/include` 下），都要记得把 `xxx` 加进 `REQUIRES`。可以用编译错误的提示反查缺哪个组件。

### 问题 5：网页遥控器发命令无效果，ESP32 收不到（设备 ID base64 编码错误）

- **现象**：网页能正常连接 Broker（登录"有行为"），但点电源/温度等按钮 ESP32 串口无任何输出（按键"无行为"）。
- **根因**：前端设备注册表 `atob('ZThhOGU4')` 是手算的 base64，**编码错误**——解码结果是 `e8a8e8`，而真实设备 ID 是 `e9a8e8`。命令被发布到错误主题 `/ac-remote/e8a8e8/cmd`，ESP32 订阅的是 `/ac-remote/e9a8e8/cmd`，主题不匹配，永远收不到。

  ```python
  base64.b64decode('ZThhOGU4')   # → b'e8a8e8'（错）
  base64.b64encode(b'e9a8e8')    # → b'ZTlhOGU4'（正确的 base64 应为 ZTlhOGU4）
  ```

- **解决**：取消 base64 混淆层，设备 ID 直接作为字符串常量 `DEFAULT_DEVICE = 'e9a8e8'` 写在网页前端，并在"高级设置"中可修改。
- **经验**：
  1. 手动计算 base64 / 哈希极易出错，务必用工具核对（如 `python -c "import base64; ..."`）；
  2. MQTT 排错第一步：**核对发布与订阅的主题是否精确一致**；浏览器调试时查看 WebSocket 帧里实际发布的 topic；
  3. 通过「DOM 文本 + 正则」反向提取连接参数（如 device_id）的写法脆弱，应直接保存到全局变量。

### 问题 6：浏览器"跟踪防护"屏蔽 mqtt.js CDN，导致库加载异常、命令发不出

- **现象**：浏览器控制台反复出现 `Tracking Prevention blocked access to storage for https://cdn.jsdelivr.net/npm/mqtt@5/dist/mqtt.min.js`，网页按键无任何网络事件。
- **根因**：页面通过第三方 CDN（jsdelivr）加载 mqtt.js，浏览器（Edge/Chrome）的"跟踪防护"会阻断这类跨站脚本对存储的访问/执行，导致 `mqtt` 对象不可用，MQTT 连接无法建立，所有命令被"未连接"拦下。
- **解决**：把 `mqtt.min.js` 下载到仓库 `web/vendor/`，页面改用**同源相对路径** `<script src="vendor/mqtt.min.js">`，由 GitHub Pages 从本站域名提供，不再经过第三方 CDN。
- **经验**：IoT Web 控制端应**本地化全部 JS 依赖**，避免第三方 CDN 被浏览器安全策略/网络环境阻断；关键链路（库加载 → 连接 → publish）加 console 日志便于快速定位。

### 问题 7：远程命令到达但 GPIO23 不输出（硬件调试记录）

- **现象**：网页发 `on`/`off` 命令，串口打印命令信息，但 GPIO23 保持低电平、LED 不亮。
- **排查**：临时改代码为「GPIO22 受控 + GPIO23 以 1 秒周期翻转」调试版后，**两个引脚输出均正常**——证明 D23 丝印即 GPIO23、引脚本身、`gpio_config` / `gpio_set_level` API 全部正确，**问题不在硬件**。
- **根因**：`git show` 版本追溯确认 **v0.1.0 的 main.c 完全没有 GPIO 控制代码**（收到命令仅打印日志），GPIO 控制是 v0.1.1 才加入。此前一次测试板载固件为 v0.1.0，命令到达只打印 `[EXEC]`，`gpio_set_level` 从未执行，引脚自然保持低电平。
- **解决**：恢复正式版（GPIO23 受控输出），并对 `gpio_set_level` 增加 `ESP_ERROR_CHECK` 返回值检查，使 GPIO 错误显式可见。
- **经验**：
  1. 「命令打印了但外设没动」首先要确认**固件是否真的包含该外设逻辑**（核对 git 版本 / 编译时间），而不是先怀疑硬件；
  2. GPIO 排障用**固定周期翻转脚本**可快速区分「引脚映射问题」与「代码未执行」；
  3. `gpio_set_level` 等驱动 API 的返回值应检查（`ESP_ERROR_CHECK`），避免静默失败难以定位。

### 问题 8：红外接收模块 DAT 输出 5V 直连 ESP32 GPIO 有损坏风险

- **风险**：HX-M121 接收模块标称 5V 器件，DAT 输出电平可能为 5V；**标准 ESP32 GPIO 绝对最大输入电压 3.6V**，"ESP32 可以容忍 5V 输入"的说法是**错误**的，直连可能损坏芯片。
- **解决（按优先级）**：
  1. 先万用表实测 DAT 高电平——很多"5V 供电"模块的 DAT 逻辑电平其实已是 3.3V，≤3.3V 可直连；
  2. 若 DAT 为**开漏输出**（常见），将 ESP32 GPIO 配为输入 + 内部上拉到 3.3V，无需外部元件；
  3. 仅当 DAT 为推挽 5V 输出时才需**电阻分压**（DAT→2.2kΩ→GPIO，GPIO→3.3kΩ→GND，约 3V）。
- **不可靠做法**：串联普通二极管降压——GPIO 高阻输入下二极管几乎无电流、无压降，且单管 0.7V 降压不足，无法保护。
