# ESP32 CarPlay Web Relay

[![ESP-IDF CI](https://github.com/xujingguo58/esp32-carplay-web-relay/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xujingguo58/esp32-carplay-web-relay/actions/workflows/ci.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

基于 ESP-IDF 的实验性无线 CarPlay 接收与网页中转项目。协议实现参考 [DiPlay / xcertplay](https://github.com/shihabal3amri/DiPlay)，浏览器媒体和触控路径参考 [WheelPlay](https://github.com/fython/wheelplay)。

> **项目状态：** M2 合成视频已实测；2026-10-23 用户确认真实 CarPlay 画面和单点触控可用。目前帧率偏低、画面卡顿，持续运行、断连恢复与完整 M3–M5 验收仍未完成。

## 工作流程

1. ESP32 开启 SoftAP 和 HTTP 服务，iPhone 与车机浏览器接入同一局域网。
2. iPhone 经 Bluetooth BR/EDR 和 iAP2 启动连接，再与 ESP32 建立 AirPlay 媒体会话。
3. ESP32 将 H.264 通过 WebRTC 转发到浏览器；浏览器触控经 WebSocket 回传，再由 ESP32 转为 AirPlay HID 输入。

设计目标是直接转发 H.264，避免在 ESP32 上解码或转码。

## 实现进度

| 阶段 | 已有代码 | 尚待验证 |
| --- | --- | --- |
| M1：平台骨架 | SoftAP、HTTP 状态页、BR/EDR/RFCOMM 适配 | 开发板热点、蓝牙及共存行为 |
| M2：浏览器样机 | 合成 H.264、WebRTC、WebSocket 信令、触控回显；用户确认实机视频播放正常；answer SDP 使用 PSRAM 优先的临时缓冲，避免大块常驻在内部队列 | 量化资源峰值和延迟数据尚未单独记录 |
| M3：协议核心 | iAP2 状态机、MFi signer 接口、无线启动消息；网页媒体就绪后广播 Car Audio BR/EDR 设备 | iPhone 蓝牙设置中仍不可见；一次会话日志已到 AirPlay Pair-Setup/Verify 和 MFi auth-setup，仍需新固件复测完整 iAP2/无线启动 |
| M4：视频接收 | RTSP 配对、Screen DataStream 解密/组帧、WebRTC 媒体桥；H.264 在接收缓冲区原位解密并通过单帧借用队列发送，省去应用层第二份整帧缓冲和逐帧复制 | 修复 RECORD 顺序和非视频 opcode 误断连后，用户确认真实 CarPlay 画面与触控可用；帧率偏低，长期播放和重连尚待验证 |
| M5：触控回传 | 触点映射、HID/Event Control 软件路径、断连清理 | 用户确认基础触控可用；超时释放、多点及重连后的触控仍需专项验收 |

运行中的 M2 固件发送的是**合成测试图像**，并会在网页标明它不是 CarPlay 画面。当前调试构建将网页配对码固定为 `11111111`，仅经 UART 输出；未完成配对时不会启动 WebRTC。各阶段实现和验收记录见 [开发路线图](docs/ROADMAP.md)。

## 快速开始

### 硬件与软件

- 目标芯片为支持 Bluetooth BR/EDR 的经典 ESP32；ESP32-S3、ESP32-C5 等仅支持 BLE 的芯片不能替代该蓝牙链路。
- 项目使用 Classic BR/EDR 的 iAP2/SPP，不使用 BLE/GATT；默认、4 MB 和 8 MB 构建配置都关闭未用 BLE host，保留 Classic SMP 配对。
- 推荐使用带 PSRAM 的模组（例如 ESP32-WROVER）。8 MB Flash profile 会以 40 MHz 自动检测并启用 PSRAM，并将 mbedTLS/DTLS 工作分配放入 PSRAM；没有检测到 PSRAM 时仍会启动，但 WebRTC 可能无法建立。当前 8 MB 调试 profile 要求经典 ESP32 Rev 3.0 或更新版本。4 MB profile 也可构建，2 MB 分区不足以容纳当前固件。Flash 容量不等于 PSRAM 容量。
- 需要 ESP-IDF 5.5.3，并在已加载 ESP-IDF 环境的终端中运行命令。

### 配置和构建 8 MB profile

```sh
export IDF_TARGET=esp32
export SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults.8mb"

idf.py -B build-8mb -D SDKCONFIG="$PWD/sdkconfig.8mb" menuconfig
idf.py -B build-8mb -D SDKCONFIG="$PWD/sdkconfig.8mb" build
```

在 `CarPlay Relay → SoftAP` 中设置 SSID 和 WPA2 密码（8–63 个字符）。配置保存在本地 `sdkconfig.8mb`，该文件不会提交到仓库。

硬件烧录只使用上面的 `build-8mb` profile。`tools/ci.sh` 的检查镜像放在独立的 `build-ci/` 目录；CI 配置不包含 SoftAP 密码，不能用于实机烧录。

烧录时使用同一个 profile，并将串口路径替换为开发板对应端口：

```sh
idf.py -B build-8mb -D SDKCONFIG="$PWD/sdkconfig.8mb" -p /dev/cu.usbserial-XXXX flash monitor
```

设备启动后，连接 ESP32 SoftAP，浏览器访问 <http://192.168.4.1/>，输入调试网页配对码 `11111111`。`GET /api/status` 可查看 AP、客户端、内部堆/PSRAM 余量和媒体丢帧统计。

8 MB profile 使用单个应用分区，不包含 OTA 或文件系统分区。4 MB 与 8 MB 分区布局和构建选项见 [架构设计](docs/ARCHITECTURE.md)。

## 检查

在加载 ESP-IDF 5.5.3，并安装 Clang 和 Node.js 后，运行与 GitHub Actions 相同的构建和主机检查：

```sh
sh tools/ci.sh
```

部分协议检查会构建 ESP-IDF 随附的 Mbed TLS，并依赖 `IDF_PATH`。自动化检查不能替代蓝牙、热点、iPhone、真实视频或触控的硬件验收。详见 [贡献指南](CONTRIBUTING.md)。

## 项目结构

- `components/net_port/`、`bt_port/`、`iap2_*`：SoftAP、Bluetooth BR/EDR 和 iAP2 启动链路。
- `components/airplay_*`：AirPlay 配对、RTSP、媒体接收、事件通道和 HID 控制。
- `components/web_gateway/`、`media_bridge/`、`input_bridge/`：网页信令、WebRTC 媒体及触控映射。
- `docs/`：架构、开发路线图和验收记录；`tools/`：合成媒体及主机检查。

DiPlay、WheelPlay 等上游参考源码位于工程旁的 `esp32-reference/`，不会作为固件依赖；组件和来源说明见 [第三方声明](THIRD_PARTY_NOTICES.md)。

## 身份材料与安全说明

- iAP2 MFi challenge-response 由软件 signer 完成，不需要外置 MFi 芯片。仓库中的 P-256 私钥和证书来自 DiPlay v0.2.21 官方 APK，是实验身份材料，不是为本项目签发的 Apple 认证身份，也不保证未来 iOS 会接受。私钥编入固件后可从镜像提取，来源和限制见 [MFi 身份声明](components/auth_software_provider/IDENTITY_NOTICE.md)。
- AirPlay Ed25519 身份首次启动时生成并保存到 NVS；当前未启用 Flash/NVS 加密，接触设备的人可能提取身份。Pair-Setup 默认使用开发配对码 `3939`，可通过 Kconfig 修改。详见 [AirPlay 身份声明](components/airplay_software_provider/IDENTITY_NOTICE.md)。
- 当前调试构建使用固定网页配对码 `11111111`，仅通过 UART 输出；请勿将此设置用于不受信任的网络。开发服务不提供 TLS，也不适合作为产品级 provisioning 方案。

使用和分发本项目时，请遵守 GPL-3.0 及依赖许可要求。参见 [许可证](LICENSE)、[安全策略](SECURITY.md) 和 [变更记录](CHANGELOG.md)。
