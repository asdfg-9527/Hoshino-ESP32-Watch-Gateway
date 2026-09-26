# Hoshino Watch Gateway

Hoshino 是面向 Redmi Watch 6 / Xiaomi Vela QuickApp 的 ESP32 网络网关项目：手表通过 Bluetooth Classic SPP 与 ESP32 建立原有网络隧道，ESP32 再通过家庭 Wi-Fi 提供联网能力；QuickApp 通过手表侧 Interconnect 的 HTTP 请求完成状态读取、Wi-Fi 扫描和配网。

本仓库只发布源码和文档，不发布固件、RPK、密钥或其他构建产物。

## 项目来源与版本关系

本项目是在上一版公开项目 [xiazixua/esp32](https://github.com/xiazixua/esp32/) 的研究基础上继续整理和开发的 Hoshino ESP32 Watch Gateway。上一版项目公开了 `源码.zip` 与 `源码v31.zip`，其中的 README、硬件说明、PlatformIO 配置和网关思路均保留为本仓库的参考资料。

上一版仓库 README 中列出的功能模式包括：

本仓库的直接上游是 [Wanfeng-MI/Hoshino-ESP32-Watch-Gateway](https://github.com/Wanfeng-MI/Hoshino-ESP32-Watch-Gateway)，本分支基于其 `v2026.09.18-nc2` 快照，并在其基础上增加了 0.96 寸 SSD1306 状态屏、低内存 ESP-IDF 构建的蓝牙 ACL 上限修复，串口监视波特率改为 115200。

| 上游模式 | 说明 | 当前 Hoshino 源码状态 |
|---|---|---|
| 手表网关 | Bluetooth Classic SPP + Wi-Fi + NAPT，为手表提供联网桥接 | 当前主功能，保留并继续维护 |
| BLE 广播 | 上游测试/研究模式 | 不包含在当前 source-only 快照 |
| Wi-Fi 抓包 | 上游研究模式 | 不包含在当前 source-only 快照 |
| 手环上网 | 上游已标记废弃的模式 | 不包含在当前 source-only 快照 |
| 网络测速 | 上游测试模式 | 不包含在当前 source-only 快照 |
| OTA | 上游版本曾使用 OTA 分区和回滚 | 当前版本不承诺 OTA；按当前 PlatformIO env 刷写 |

上表用于保留上一版项目的说明，不代表当前仓库启用了这些历史模式。当前仓库的真实构建配置和路由以 `esp32/platformio.ini`、`esp32/src/main.cpp` 和 `quickapp/src/` 为准。

## 当前功能

- ESP32-WROOM / WROOM-32E 的 Bluetooth Classic SPP 手表连接与认证。
- 原有 SPP、RFCOMM、虚拟网卡、DHCP、DNS、NAPT、TCP 转发和 Watch 网络隧道保持不变。
- 手表侧虚拟网卡：手表通常获得 `10.1.10.2/24`，网关为 `10.1.10.1`。
- 手表 IPv4 流量经 lwIP NAPT 转发到家庭 Wi-Fi。
- 首次配置时开启 `Hoshino-Bridge` SoftAP，手机或电脑可以访问内置网页。
- QuickApp 通过 `/watch-setup/status`、`/watch-setup/scan` 和 `/watch-setup/wifi` 完成手表端配网。
- Wi-Fi 扫描最多保留 20 个唯一 SSID，同名网络保留较强 RSSI，响应包含 `rawCount` 和 `truncated`。
- Wi-Fi 密码为空或少于 8 个字符时拒绝提交；连接失败会向手表返回明确错误，不会无限重试错误密码。
- QuickApp 输入法为英文-only、rect QWERTY 版本，密码输入按当前 UI 要求显示明文。
- 可选 0.96 寸 SSD1306 状态屏：实时显示本机 IP、家庭 Wi-Fi 状态、手表隧道状态、NAPT 转发计数和空闲堆。

## 硬件要求

| 项目 | 要求 |
|---|---|
| 芯片 | ESP32 经典双核，推荐 ESP32-WROOM-32 / 32E / DevKit |
| 蓝牙 | 必须支持 Bluetooth Classic SPP；ESP32-C3 等 BLE-only 芯片不适用 |
| Flash | 当前低内存环境使用 `bare_minimum_2MB.csv`，实际板卡应满足当前分区表容量 |
| 串口 | 默认监视速率 115200 baud |
| 配网触发 | `GPIO16` 与 `GPIO17` 短接约 2 秒，或串口执行 `WATCH_SETUP` |
| 状态屏（可选） | 0.96 寸 128x64 SSD1306，I2C 接 `GPIO21`(SDA) / `GPIO22`(SCL) |

上一版公开项目推荐 uPesy ESP32 WROOM、4MB Flash，并描述了 BOOT 键、板载 LED 和可选 SSD1306 OLED；这些内容是上游硬件参考，不是当前 Hoshino 版本的必需外设。

## 状态屏（0.96 寸 SSD1306）

状态屏是**可选**功能，用于在没有手机/串口的情况下直接看板子当前状态。接线（模块必须支持 3.3V 逻辑）：

| SSD1306 模块 | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

`GPIO21` / `GPIO22` 是本固件里唯一空闲的常用引脚：`GPIO16`/`GPIO17` 被配网短接触发占用，`GPIO5`/`GPIO18`/`GPIO19`/`GPIO23` 被 SD 卡的 HSPI 占用。

屏幕每 500 ms 刷新一次，5 行内容为：

```text
Hoshino GATEWAY      # 工作模式（SETUP = 配网模式）
IP 192.168.1.123     # 本机地址；配网模式显示 SoftAP 的 192.168.4.1
WIFI MyHomeWiFi      # 家庭 Wi-Fi：SSID / 失败原因（如 wrong_password）/ 连接中
WATCH tunnel up      # 手表隧道状态（tunnel up / connected / connecting / idle）
TX1234 RX5678 118K   # NAPT 转发计数与空闲堆
```

关于 `WATCH` 行的取值：

- `tunnel up`：`gWatchNetworkReady`，虚拟网卡与 NAPT 已就绪，手表可以上网；
- `connected` / `connecting` / `idle`：桥接任务状态，隧道尚未就绪；
- `task FAIL`：桥接任务创建失败。

关于第 5 行的计数：每行最多只能画 21 个字符（6px 等宽字体 × 21 = 126px）。计数超过 10000 后会压成紧凑形式（`12k`），超过 1000 万压成 `12M`，因此该行不会因为计数变长而挤掉堆的数字；万一仍然超宽，整段堆信息会被丢掉而不会把数字画到屏幕外。

**实现约束（与项目低内存策略一致）**：全屏 framebuffer 仅 1024 字节；不开新任务、不做堆分配，所有文案都是栈上 `char[]` + `snprintf`；已配置的正常启动会把屏幕初始化**推迟到 Wi-Fi 就绪**，让蓝牙控制器和 `esp_wifi_init` 先拿到未被 `Wire` 切碎的堆（与 SD 卡惰性挂载同一思路）。配网模式没有经典蓝牙竞争内存，会立即点亮。

排障：`initDisplay()` 会扫描整条 I2C 总线并把结果打到串口。常见日志：

```text
DISPLAY_I2C_SCAN sda=21 scl=22: 0x3C     # 找到设备
DISPLAY_READY addr=0x3C refresh_ms=500
DISPLAY_ADDR_OVERRIDE configured=0x3C using=0x3D   # 模块是 0x3D，自动兜底
DISPLAY_INIT_FAILED no_i2c_device sda=21 scl=22    # 接线/供电问题，或根本没接屏幕
```

屏幕初始化失败只打串口日志，**不会**影响蓝牙桥接、NAPT 或配网。

## 目录结构

```text
.
├── esp32/                         # 当前 ESP32 网关源码与 PlatformIO 配置
│   ├── src/main.cpp               # 网关主程序（含可选 SSD1306 状态屏）
│   ├── platformio.ini             # 构建环境
│   ├── sdkconfig.wroom_lowmem_idf  # 低内存 ESP-IDF 配置
│   ├── bare_minimum_2MB.csv       # 当前低内存分区表
│   ├── lib/lwip_napt_override/    # NAPT 覆盖代码及 BSD 声明
│   ├── tools/                     # 辅助工具（如 Android 存储 mock）
│   └── test_*.py                  # 合约测试（需在 esp32/ 目录下运行）
├── quickapp/                      # Xiaomi Vela QuickApp 源码
│   ├── src/pages/wifi_setup/      # 手表配网页面
│   ├── src/common/esp32_bridge.js # HTTP/Interconnect 桥接
│   └── src/components/InputMethod/# 精简英文键盘
├── archive/                       # 上一版源码-only 快照
├── LICENSE                        # PolyForm Noncommercial 1.0.0
└── README.md
```

固件和包产物始终留在本地，不进入 Git，包括 `.bin`、`.rpk`、`.elf`、`.map`、`.pio/`、构建目录、缓存、签名材料和本地密钥。

## 当前版本编译

### ESP32

在 `esp32/` 目录执行：

```powershell
C:\Users\liuya\AppData\Local\Programs\Python\Python312\python.exe -m platformio run -e wroom_lowmem_idf
```

烧录仅在确认目标串口后执行：

```powershell
C:\Users\liuya\AppData\Local\Programs\Python\Python312\python.exe -m platformio run -e wroom_lowmem_idf -t upload
```

串口监视：

```powershell
C:\Users\liuya\AppData\Local\Programs\Python\Python312\python.exe -m platformio device monitor -b 115200
```

当前 WROOM 低内存环境固定使用：`board = esp32dev`、`framework = espidf, arduino`、`bare_minimum_2MB.csv` 和 `sdkconfig.wroom_lowmem_idf`。不要擅自切换蓝牙、lwIP/NAPT 或分区配置。

#### 环境与状态屏开关

状态屏由编译宏 `HOSHINO_ENABLE_DISPLAY` 控制（默认 `1`，可用 `-D` 覆盖）：

| 环境 | 分区表 | 状态屏 |
|---|---|---|
| `esp32dev` | `huge_app.csv`（3MB app） | **开启** |
| `esp32-wrover` | `huge_app.csv` | **开启** |
| `wroom_lowmem_idf` | `bare_minimum_2MB.csv`（1900K app） | **关闭** |

**为什么 2MB 环境默认关闭**：上一版带 OLED 的分支固件约 2.0MB，而 1900K 的 app 分区装不下它，因此低内存环境显式加了 `-DHOSHINO_ENABLE_DISPLAY=0`，让 U8g2 完全不参与链接，固件体积与该功能引入前保持一致。

4MB 板要带状态屏，用 4MB 环境编译：

```powershell
C:\Users\liuya\AppData\Local\Programs\Python\Python312\python.exe -m platformio run -e esp32dev
```

如果要继续使用 `wroom_lowmem_idf` 的 ESP-IDF 低内存配置并同时点亮屏幕，需要把该环境的 `board_build.partitions` 改成 `huge_app.csv`（仅对 4MB 板）并把末尾的 `-DHOSHINO_ENABLE_DISPLAY=0` 改成 `=1`。

刷新间隔可用 `-DHOSHINO_DISPLAY_REFRESH_MS=...` 调整（默认 500）。

### QuickApp

在 `quickapp/` 目录执行：

```powershell
npm install
npm run build
```

构建生成的 RPK 只保存在本地并被 Git 忽略。

## 首次配网

当前 Hoshino 固件的配网流程：

1. ESP32 在无完整 Watch MAC/Auth Key 配置时开启 `Hoshino-Bridge` 热点。
2. 默认 AP 密码为 `hoshino-setup`；首次保存后建议改为自己的密码。
3. 手机或电脑连接该热点，打开 `http://192.168.4.1/`。
4. 网页端可填写家庭 Wi-Fi、Watch MAC、32 位 Watch Auth Key、本地 Token 和 AP 密码。
5. 手表端 QuickApp 通过 Interconnect 访问 `http://10.1.10.1`，读取状态、扫描 Wi-Fi 并提交配置。
6. 配置保存成功后 ESP32 重启，连接家庭 Wi-Fi 并自动启动手表桥接。

上一版 `xiazixua/esp32` 文档中的 `Vela-Bridge`、`12345678` 和 `upesy_wroom_lowmem_idf` 是其历史版本参数；当前 Hoshino 默认值以本节和 `esp32/src/main.cpp` 为准。

## 正常工作流程

```text
Redmi Watch 6
    │ Bluetooth Classic SPP / RFCOMM
    ▼
ESP32 Hoshino Gateway
    ├─ 认证与 SPPv2 会话
    ├─ 虚拟网卡 10.1.10.1/24
    ├─ DHCP / DNS
    ├─ lwIP NAPT
    └─ 家庭 Wi-Fi → Internet
```

建立桥接后，手表得到 `10.1.10.2`，默认网关为 `10.1.10.1`，DNS 由 ESP32 提供。配网扫描期间不应重构或替换原有 Bluetooth Classic 隧道。

## Web API

### 手机配网 AP

```text
GET  /                         # 内置配网页面
POST /setup                    # 保存 Wi-Fi / Watch / AP 配置
GET  /setup/status             # 当前配网状态
```

### 手表侧配网隧道

QuickApp 使用的基础地址为 `http://10.1.10.1`：

```text
GET  /watch-setup/status       # ESP32 与 Wi-Fi 状态
GET  /watch-setup/scan         # 扫描附近 Wi-Fi
POST /watch-setup/wifi         # 保存并连接指定 Wi-Fi
```

### 存储诊断

```text
GET /api/v1/storage/status
GET /api/v1/storage/list
GET /api/v1/storage/file
GET /api/v1/storage/chunk
```

敏感配置接口只允许从 ESP32 配网 SoftAP 或已建立的受控手表隧道访问；仓库不包含任何真实 Auth Key、Wi-Fi 密码或 Token。

## 串口命令

| 命令 | 作用 |
|---|---|
| `WATCH_CONFIG ssid=.. wifi_pass=.. watch_mac=.. watch_auth=.. auto_connect=1` | 命令行保存配置 |
| `WATCH_AUTH <MAC> <32hex>` | 手动触发认证/桥接 |
| `WATCH_SETUP` | 写入配网标志并重启 |
| `WATCH_SDP <MAC>` | 查询 SPP 通道 |
| `ESP_DNS_SELFTEST` | ESP32 独立 DNS 连通性自测 |

## 上一版公开项目的历史说明

上一版 `xiazixua/esp32` README 还介绍了以下内容：ESP32-WROOM 4MB 硬件、OLED/LED 状态显示、BLE 广播、Wi-Fi 抓包、网络测速、OTA 分区回滚，以及通过 `esptool.py` 写入 bootloader、分区表和固件。这些内容保留在本 README 作为项目历史和引用，但当前 source-only Hoshino 版本不包含那些历史模式的完整源码，也不随仓库发布任何 `.bin` 文件。

上一版公开项目的源码包使用 PlatformIO，核心构建框架为 `framework = espidf, arduino`，依赖包括 ArduinoJson、`sh123/esp32_opus` 和 U8g2；这些是技术栈信息，不是 AGPL 或其他开源许可证名称。

## 参考项目与致谢

- [xiazixua/esp32](https://github.com/xiazixua/esp32/) — 上一版公开的 ESP32 Watch Gateway 项目。
- [NEORUAA/Vela_input_method](https://github.com/NEORUAA/Vela_input_method) — QuickApp 输入法上游；当前仅保留英文 rect QWERTY 分支，见 `quickapp/src/components/InputMethod/UPSTREAM.md`。
- AstroBox-NG — AstralSightStudios。
- AstroBox-Public — AstralSightStudios。

上一版公开项目 README 中列出的作者页面：

- Bilibili：<https://m.bilibili.com/space/2132666053>
- 酷安：<https://www.coolapk.com/u/21850863?from=qr>

Hoshino 的 ESP32 固件实现基于设备通信行为、抓包和公开协议实现的研究。私有协议抓包数据不包含在本仓库中。

## 许可证

- 当前仓库原创代码使用 **PolyForm Noncommercial License 1.0.0**，见 [LICENSE](LICENSE)。未经原作者/版权持有人书面许可，不得将本项目、修改版或基于本项目的衍生作品用于商业目的、商业产品、商业服务或商业分发。
- 原作者/版权持有人可以自行商业使用原创代码，或另行授予商业许可；第三方组件仍以其各自许可证为准。
- `esp32/lib/lwip_napt_override/` 中的 lwIP 覆盖文件保留原始 **BSD** 许可声明。
- `quickapp/src/components/InputMethod/` 保留上游 **MIT** 许可证。
- 其他第三方代码和组件的许可证以对应源码目录中的声明为准。

## 免责声明

本项目仅用于个人学习、设备互联研究和非商业用途。使用前请确认你拥有相关设备，并遵守设备制造商的服务条款与当地法律法规。作者不对任何误用、设备损坏或法律后果负责。
