# Hoshino ESP32 Watch Gateway

让一块经典双核 ESP32（含 Bluetooth Classic）长期充当 Redmi Watch 6 的「手机侧网络网关」：

- 手表通过蓝牙 SPP 连接 ESP32；
- ESP32 在内部架起一块虚拟网卡，为手表提供 DHCP（`10.1.10.2/24`）；
- 手表的 IPv4 流量经 lwIP NAPT 转发到家庭 Wi-Fi，实现真实联网；
- 语音经 Opus 解码后交给 MiMo ASR，再把识别文字回填到手表原生 XiaoAI 界面。

本仓库只包含**固件源码**。私有协议的逆向过程与抓包数据不在此公开。

---

## 功能特性

- **首次配网**：无任何配置时自动开启 `Hoshino-Bridge` 热点，手机连上后在网页里填写 Wi-Fi 与手表参数。
- **网页配置**：内置 Web 控制台（配网 + 状态 + 启动/停止 + 对话上下文管理）。
- **网络桥（NAPT）**：手表 `ch7` 原始 IPv4 → 虚拟网卡 → lwIP NAPT → 家庭 Wi-Fi → 互联网，支持 DNS/TCP/HTTP/TLS。
- **XiaoAI 语音**：`ch3` Opus 流 → 滚动 ASR → 原生 transcript 回填。
- **多轮上下文**：MiMo Chat 保留最近 3 轮 `user/assistant` 对话，空闲自动清空。
- **IO 短接重新配网**：短接两个 GPIO 约 2 秒，自动重启进入配网模式。

---

## 硬件要求

| 项 | 要求 |
|----|------|
| 芯片 | ESP32（经典双核，如 ESP32-WROOM-32 / 32E / DevKit） |
| 蓝牙 | **必须支持 Bluetooth Classic SPP**（ESP32-C3 等 BLE-only 芯片不适用） |
| Flash | ≥ 2 MB（默认使用 `bare_minimum_2MB.csv` 分区表） |
| 串口 | 波特率 2000000 |

> 建议内存：项目已针对 2 MB Flash / 无 PSRAM 的 WROOM 做了内存优化，
> 桥接稳定态空闲堆约 60~90 KB。若后续要跑更重的语音/TLS，可换 WROVER（8 MB PSRAM）。

---

## 依赖

- [PlatformIO](https://platformio.org/)（推荐 VS Code 插件）
- `framework = espidf, arduino`（ESP-IDF 4.4.x + Arduino 作为组件）
- [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) ^7
- [sh123/esp32_opus](https://github.com/sh123/esp32_opus)（Opus 解码；发布前请自行复核其许可证）

---

## 编译与烧录

```bash
# 编译（低内存 ESP-IDF 环境）
pio run -e wroom_lowmem_idf

# 烧录
pio run -e wroom_lowmem_idf -t upload

# 串口监视
pio device monitor -b 2000000
```

> 首次编译若报 `sdkconfig` 相关错误，删除已生成的 `sdkconfig.wroom_lowmem_idf` 后重新 `pio run`。

---

## 首次配网

1. 烧录后（无任何配置时），ESP32 自动开启热点 **`Hoshino-Bridge`**。
2. 默认 AP 密码：**`hoshino-setup`**（第一次保存配置时建议改成你自己的）。
3. 手机/电脑连接该热点。
4. 浏览器打开 **`http://192.168.4.1/`**（连上热点后通常会自动弹出）。
5. 填写：
   - 家庭 Wi-Fi SSID / 密码；
   - Watch MAC（形如 `AA:BB:CC:DD:EE:FF`）；
   - Watch Auth Key（32 位 hex，需自行从官方设备/抓包获取，仓库不含任何密钥）；
   - （可选）MiMo Base URL / API Key / 模型 / 本地 Token / CA 证书。
6. 保存 → ESP32 自动重启 → 连家庭 Wi-Fi 并自动连接手表。

---

## 正常工作流程

配置保存后，每次开机都会：

1. 连接家庭 Wi-Fi（STA）；
2. 自动认证并连接手表（SDP 发现 SPP 通道）；
3. 建立虚拟网卡 + NAPT；
4. 手表获得 `10.1.10.2`、网关 `10.1.10.1`、DNS `114.114.114.114`；
5. 手表的网络流量经 ESP32 转发上网。

家庭 Wi-Fi 环境下，可用浏览器访问 `http://hoshino-bridge.local/`（mDNS）或 ESP32 的 STA IP 查看状态。

---

## 重新配网（IO 短接）

需要重新进入配网模式时：

- **短接 `GPIO16` 与 `GPIO17`**（板子上丝印 `D16` / `D17`）约 **2 秒**；
- ESP32 写入标志并**重启**，重启后直接进入配网热点模式。

也可通过串口命令触发：

```
WATCH_SETUP
```

> 说明：配网模式采用「写标志 + 重启」而非运行时热切换，避免 STA→AP 切换因内存/状态问题失败。

---

## Web API

家庭 LAN 管理需带 Header `X-Hoshino-Token: <你的 token>`：

```
GET  /api/v1/status
POST /api/v1/watch/start
POST /api/v1/watch/stop
POST /api/v1/context/reset
POST /api/v1/chat
POST /api/v1/test
GET  /api/v1/models
```

配网 AP 专用：

```
POST /setup          # 保存配置（含 Wi-Fi / 手表参数）
GET  /setup/status
GET  /setup/trace
```

---

## 串口命令

| 命令 | 作用 |
|------|------|
| `WATCH_CONFIG ssid=.. wifi_pass=.. watch_mac=.. watch_auth=.. auto_connect=1` | 命令行保存配置 |
| `WATCH_AUTH <MAC> <32hex>` | 手动触发认证/桥接 |
| `WATCH_SETUP` | 进入配网模式（写标志 + 重启） |
| `WATCH_SDP <MAC>` | 查询 SPP 通道 |
| `ESP_DNS_SELFTEST` | ESP32 独立 DNS 连通性自测 |

---

## 架构概览

```
Redmi Watch 6
   │  Bluetooth Classic SPP
   ▼
ESP32
   ├─ 认证 / SPPv2 会话
   ├─ 虚拟网卡 10.1.10.1/24（为手表提供 DHCP）
   ├─ lwIP NAPT（IP 转发 + 源地址改写）
   │
   ├─ ch7 原始 IPv4 ──► NAPT ──► 家庭 Wi-Fi ──► 互联网
   └─ ch3 Opus ──► 解码 ──► MiMo ASR ──► 原生 XiaoAI 回填
```

> 注：本仓库未包含协议逆向抓包数据。若你需要在此基础上继续扩展，
> 请自行遵守目标设备/服务的相关条款与当地法律法规。

---

## 许可证

- 本项目的原创代码以 **GNU GPL v3.0** 发布，见 [LICENSE](LICENSE)。
- `lib/lwip_napt_override/` 中的 `.inc` 文件源自 ESP-IDF 的 lwIP 实现，
  保留其原始 **BSD** 许可声明；GPL 项目可以包含 BSD 组件，但该部分仍受 BSD 条款约束。

---

## 免责声明

本项目仅用于**个人学习与设备互联研究**。使用前请确认你拥有相关设备，
并遵守设备制造商的服务条款与当地法律法规。作者不对任何误用、设备损坏或法律后果负责。

---

## References & Acknowledgements

本项目的 Xiaomi MiWear / Vela 穿戴设备通信协议研究过程中，
参考并交叉验证了以下社区开源项目及公开资料：

- AstroBox-NG — AstralSightStudios
- AstroBox-Public — AstralSightStudios

Hoshino 的 ESP32 固件实现基于对设备通信行为、抓包及公开协议实现的研究。
第三方代码及组件的许可证以对应源码目录中的声明为准。
