# Hoshino Watch Gateway

This repository contains the source for the Hoshino ESP32 gateway and its Xiaomi Vela QuickApp.

## Layout

- `esp32/` — current ESP32 gateway source, PlatformIO/ESP-IDF configuration, lwIP override, tests, and development notes.
- `quickapp/` — current QuickApp source, manifest/package metadata, input method, and contract tests.
- `archive/archive-Hoshino-ESP32-v2026.08.19-source-only.zip` — source-only snapshot of the previous remote version.

Firmware and package outputs are intentionally excluded from Git. This includes `.bin`, `.rpk`, `.elf`, `.map`, build directories, caches, signing material, and local secrets.

## Build locally

ESP32 (from `esp32/`):

```powershell
C:\Users\liuya\AppData\Local\Programs\Python\Python312\python.exe -m platformio run -e wroom_lowmem_idf
```

QuickApp (from `quickapp/`):

```powershell
npm install
npm run build
```

Build outputs stay local and are ignored by the repository.

配网 AP 专用：

```
POST /setup          # 保存配置（含 Wi-Fi / 手表参数）
GET  /setup/status
GET  /setup/trace
GET  /setup/wifi/scan?start=1
GET  /setup/bluetooth/scan?start=1
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
   └─ 配网页面 ──► Wi-Fi / Bluetooth Classic 扫描 ──► AuthKey 配置
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
