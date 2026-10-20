# ESP32 CarPlay Web Relay

[![ESP-IDF CI](https://github.com/xujingguo58/esp32-carplay-web-relay/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xujingguo58/esp32-carplay-web-relay/actions/workflows/ci.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

参考 DiPlay / xcertplay 的 CarPlay 接收流程和 WheelPlay 的浏览器显示方式，在 ESP-IDF 上构建运行于 ESP32 的无线 CarPlay 接收与网页中转工程。

> **项目状态：M2 样机代码已落地，M3–M5 仍在开发。** ESP-IDF 5.5.3 / 经典 `esp32` 已有通过构建的 8 MB Flash profile。默认软件 MFi challenge signer 与 AirPlay Pair-Setup/Pair-Verify identity provider 已接入；AirPlay Ed25519 身份首次启动时生成并写入 NVS，`/info` 声明 800×480 触控屏。主机协议测试和 provider 构建通过，但尚未在开发板、iPhone 或车机浏览器上验证热点/蓝牙共存、真实 CarPlay 会话、视频播放与 HID 控制。MFi challenge-response 不要求外置 MFi 芯片。

## 目标

- ESP32 作为无线 CarPlay 接收端，创建局域网热点。
- iPhone 经蓝牙启动连接流程，再通过 Wi-Fi 与 ESP32 建立 CarPlay 媒体会话。
- 车机连接同一热点，使用浏览器打开 ESP32 上的页面。
- 网页显示 CarPlay 画面，并把触摸事件回传给 ESP32，再转成 CarPlay 输入。
- 首版尽可能直接转发 H.264 媒体，不在 ESP32 上解码、缩放或重新编码。

CarPlay 接收和浏览器展示是两条逻辑链路：DiPlay/xcertplay 提供接收端协议实现的参考；WheelPlay 提供本地 Web 页面、媒体传输和浏览器触控的参考。DiPlay 的 `shared` 模块包含 iAP2、MFi、AirPlay 和媒体相关实现，但项目整体依赖 Android 平台能力；ESP-IDF 移植需要重写平台适配层，并评估协议代码的可移植部分。[DiPlay 工程说明](https://github.com/shihabal3amri/DiPlay/blob/main/docs/UPSTREAM-README.zh-CN.md)

## 方案摘要

- **首选验证硬件：**经典 ESP32 + PSRAM，例如 ESP32-WROVER 系列。原始 ESP32 支持 Bluetooth BR/EDR 与 2.4 GHz Wi-Fi；蓝牙和 Wi-Fi 共用 2.4 GHz 射频资源，需要验证同时工作时的稳定性。[ESP32 数据手册](https://documentation.espressif.com/esp32_datasheet_en.html)、[ESP-IDF 共存说明](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32/api-guides/coexist.html)
- **初始无线模式：**ESP32 SoftAP；iPhone 与车机接入同一局域网。首版不做 Wi-Fi Direct。
- **浏览器入口：**ESP32 HTTP 服务提供静态网页和会话信令；H.264 低延迟媒体优先走 WebRTC；触摸通过 WebSocket 或 WebRTC DataChannel 回传。
- **媒体范围：**首版选择 H.264 直通。ESP-IDF 的 `esp_peer` 提供 H.264 WebRTC 媒体及 DataChannel 能力，可作为 WheelPlay 媒体通路的 ESP32 侧基础。[ESP Peer](https://github.com/espressif/esp-webrtc-solution/tree/main/components/esp_peer)
- **首版功能范围假设：**无线连接、单台 iPhone、单个车机浏览器、画面与触摸。音频、麦克风、Siri/通话、HEVC、USB 有线连接和多浏览器控制放到后续评估。

HTTP 页面可以承载网页、状态和信令；实时视频不应默认设计成普通 HTTP 文件传输。WheelPlay 的 H.264/HEVC 直通使用 WebRTC，JPEG 是兼容模式；ESP32 首版不规划 H.264 到 JPEG 的解码/转码回退。[WheelPlay README](https://github.com/fython/wheelplay)

## 文档

- [架构设计](docs/ARCHITECTURE.md)：模块边界、连接流程、网络与媒体数据路径、资源和安全设计。
- [开发里程碑](docs/ROADMAP.md)：分阶段移植顺序、验收条件和风险关卡。
- [贡献指南](CONTRIBUTING.md)：本地构建、检查和提交要求。
- [安全策略](SECURITY.md)：漏洞报告方式与支持范围。
- [变更记录](CHANGELOG.md)：项目变更摘要。

## 工程结构

```text
main/                       启动编排与项目 Kconfig
components/net_port/        SoftAP、DHCP 接口和客户端状态
components/web_gateway/     HTTP 首页、状态 API、WebSocket 信令/触控
components/media_bridge/    esp_peer WebRTC、合成 H.264 和发送统计
components/bt_port/         BR/EDR、SDP、RFCOMM 传输适配
components/iap2_core/       有界 iAP2 link/CSM 状态机与帧处理
components/iap2_identity/   无线 IdentificationInformation 构造
components/iap2_carplay/    0x5703 热点配置与 0x4301 无线 CarPlay 启动消息构造
components/iap2_mfi_auth/   MFi challenge-response 成功门槛与断连重置
components/auth_port/       认证资源接口
components/auth_software_provider/ 默认软件 MFi signer 与实验身份文件
components/ed25519/         标准 Ed25519 软件签名/验签
components/airplay_software_provider/ 持久化 AirPlay 身份、配对回调、/info 与 mDNS 配置
components/airplay_identity_port/  可替换的 AirPlay identity provider 接口
components/airplay_pairing_store/ NVS 持久化 AirPlay controller 标识与公钥
components/airplay_screen/  AirPlay Screen DataStream AEAD/H.264 处理
components/airplay_media_bridge/  Screen DataStream 到 WebRTC 的回调适配
components/airplay_rtsp/    有界 RTSP framing、Pair-Setup/Verify 路由
components/airplay_pairing/ TLV8、Pair-Setup SRP-6a/状态机、Pair-Verify 状态机
components/airplay_control/  事件通道 ControlCipher、HID 命令与 RTSP 应答接口
components/airplay_session/  RTSP 会话路由、Pair-Verify 加密切换与 SoftAP TCP listener
components/airplay_timing/   AirPlay UDP timingPort 收发和时钟偏差观测
components/input_bridge/    触控映射、双触点 HID 报告和超时释放
components/time_port/       统一的 ESP-IDF 单调微秒/毫秒时钟接口
docs/                       架构设计与开发里程碑
tools/                      合成视频生成、协议组件主机测试
```

`managed_components/` 由 ESP-IDF Component Manager 解析并下载，依赖版本在 `dependencies.lock` 锁定。`DiPlay` 和 `WheelPlay` 的上游源码位于工程旁的 `esp32-reference/`，只作本地协议与浏览器行为参考，不会混入固件依赖。

## 构建与烧录

在已安装并加载 ESP-IDF 环境的终端中执行。目标为经典 ESP32；按实际开发板配置串口和 PSRAM。当前已知开发板为 8 MB Flash，建议使用 `sdkconfig.defaults.8mb` 与 `partitions_8mb.csv`：单应用分区为 `0x7f0000`，当前镜像约占 19%，该 profile 不保留 OTA 或文件系统分区。不要把 8 MB profile 烧录到较小容量的板卡。

```sh
idf.py set-target esp32
idf.py menuconfig
```

在 `CarPlay Relay → SoftAP` 中设置热点密码（WPA2 密码需为 8–63 个字符），并按需修改 SSID、信道和客户端数。密码保存在本地 `sdkconfig`，该文件已加入 `.gitignore`。

```sh
idf.py build
idf.py -p PORT flash monitor
```

8 MB Flash 的独立构建使用单独的 sdkconfig 和 build 目录，不会覆盖当前配置。编译和烧录都指定同一 profile：

```sh
IDF_TARGET=esp32 SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults.8mb" idf.py -B build-8mb -D SDKCONFIG="$PWD/sdkconfig.8mb" build
IDF_TARGET=esp32 SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults.8mb" idf.py -B build-8mb -D SDKCONFIG="$PWD/sdkconfig.8mb" -p PORT flash monitor
```

设备启动后，连接配置的 SoftAP 并访问 `http://192.168.4.1/`。首次会话需输入本次启动时 ESP32 串口日志输出的 8 位网页配对码；配对成功后才会创建 WebRTC 会话。状态接口 `GET /api/status` 返回 AP 地址、接入设备数、当前及历史最低内部堆/PSRAM 余量。

M2 开发固件默认嵌入合成 H.264 流；页面会明确标记它不是实际 CarPlay 画面。网页的 WebRTC 状态、浏览器帧统计、触控回显与 ESP32 丢帧/内存数据用于验证样机。默认启动会加载软件 MFi signer 和 AirPlay identity provider，并尝试启动 RTSP、Event 与 mDNS；真实 iPhone Pair-Setup、AirPlay 视频接收和 HID 回传仍待硬件验证。

协议核心的主机侧检查可在 macOS 上用 Clang 运行：

Pair-Setup/Verify、Screen DataStream 和 ControlCipher 主机脚本会构建 ESP-IDF 随附的 Mbed TLS，运行前需设置 `IDF_PATH`。

```sh
sh tools/test_iap2_core_host.sh
sh tools/test_bt_port_host.sh
clang -std=c11 -Wall -Wextra -Werror -Icomponents/iap2_identity/include components/iap2_identity/iap2_identity.c tools/test_iap2_identity.c -o /tmp/test_iap2_identity && /tmp/test_iap2_identity
tools/test_iap2_carplay_host.sh
tools/test_iap2_mfi_auth_host.sh
sh tools/test_airplay_bplist_host.sh
sh tools/test_airplay_identity_port_host.sh
sh tools/test_ed25519_host.sh
sh tools/test_airplay_info_plist_host.sh
sh tools/test_web_gateway_rate_limit_host.sh
sh tools/test_web_gateway_pairing_host.sh
sh tools/test_input_bridge_host.sh
sh tools/test_touch_hid_bridge_host.sh
clang -std=c11 -Wall -Wextra -Werror -Icomponents/airplay_screen/include components/airplay_screen/airplay_screen_codec.c tools/test_airplay_screen_codec.c -o /tmp/test_airplay_screen_codec && /tmp/test_airplay_screen_codec
clang -std=c11 -Wall -Wextra -Werror -Icomponents/airplay_control/include components/airplay_control/airplay_hid_plist.c tools/test_airplay_hid_plist.c -o /tmp/test_airplay_hid_plist && /tmp/test_airplay_hid_plist
clang -std=c11 -Wall -Wextra -Werror -Icomponents/airplay_rtsp/include components/airplay_rtsp/airplay_rtsp_codec.c tools/test_airplay_rtsp_codec.c -o /tmp/test_airplay_rtsp_codec && /tmp/test_airplay_rtsp_codec
clang -std=c11 -Wall -Wextra -Werror -Icomponents/airplay_pairing/include components/airplay_pairing/airplay_tlv8.c tools/test_airplay_tlv8.c -o /tmp/test_airplay_tlv8 && /tmp/test_airplay_tlv8
sh tools/test_airplay_srp6a_host.sh
sh tools/test_airplay_pair_setup_host.sh
sh tools/test_airplay_pair_verify_host.sh
sh tools/test_airplay_pairing_store_host.sh
sh tools/test_airplay_rtsp_pairing_host.sh
sh tools/test_airplay_screen_stream_host.sh
IDF_PATH=/path/to/esp-idf sh tools/test_airplay_media_bridge_host.sh
sh tools/test_media_timestamp_host.sh
IDF_PATH=/path/to/esp-idf tools/test_airplay_control_cipher_host.sh
tools/test_airplay_control_stream_host.sh
tools/test_airplay_bplist_host.sh
tools/test_airplay_timing_host.sh
IDF_PATH=/path/to/esp-idf tools/test_airplay_rtsp_session_host.sh
sh tools/test_airplay_event_lifecycle_host.sh
sh tools/test_media_frame_gate_host.sh
sh tools/test_time_port_host.sh
python3 tools/validate_test_h264.py
```

## 重要约束

1. DiPlay/WheelPlay 不是可直接刷入 ESP32 的固件；Android 蓝牙、Wi-Fi、音频和媒体 API 需要替换为 ESP-IDF 实现。
2. ESP32-WROVER 一类经典 ESP32 模组只有 2.4 GHz Wi-Fi。ESP32-S3 和 ESP32-C5 提供 BLE 而没有 BR/EDR，因此不能在本方案中直接替代原始 ESP32 的蓝牙链路。[ESP32-S3](https://www.espressif.com/products/socs/esp32-s3)、[ESP32-C5](https://documentation.espressif.com/esp32-c5_datasheet_en.html)
3. iAP2 MFi challenge-response 由软件 provider 完成，不需要外置 MFi 芯片。本工程在 `auth_software_provider` 中集成了 DiPlay v0.2.21 官方 APK 所带的实验性 P-256 私钥和证书；该身份不是为本项目签发的 Apple 认证身份，未来 iOS 是否接受也没有保证。密钥已编入固件镜像，任何拿到镜像的人都能提取。AirPlay 配对身份由独立的软件 provider 首次启动时生成并保存到 NVS；当前未启用 NVS/Flash 加密，实体接触设备者可能提取该身份。Pair-Setup 使用开发配对码（默认 `3939`），可通过 Kconfig 修改。身份来源与限制见 [MFi 声明](components/auth_software_provider/IDENTITY_NOTICE.md) 和 [AirPlay 身份说明](components/airplay_software_provider/IDENTITY_NOTICE.md)。
4. DiPlay 与 WheelPlay 标注 GPL-3.0。若直接移植并分发衍生固件，需要在开发前审查许可证义务及所用依赖的许可。[DiPlay](https://github.com/shihabal3amri/DiPlay)、[WheelPlay](https://github.com/fython/wheelplay)

## 当前实现边界

当前实现边界：

- **M1：**SoftAP、DHCP、HTTP 状态页和 BR/EDR/RFCOMM 代码存在；未在开发板上验证热点接入或配对。
- **M2：**`esp_peer` 单浏览器 H.264 WebRTC 样机、合成码流、WebSocket 信令和触控往返回显已实现；每次启动随机生成 8 位十六进制网页配对码，只通过 UART 输出，配对成功后才启动 WebRTC。主机脚本和 `node tools/test_web_gateway_page.mjs` 覆盖协议、触控 helper 与网页脚本。页面只在 ESP Peer offer 的所有媒体角色均为 `actpass` 时把浏览器 answer 设为 `passive`；其他 offer 保留浏览器 WebRTC 自动协商结果。当前完整固件镜像为 `0x185040` 字节，8 MB profile 的 `0x7f0000` 单应用分区余 `0x66afc0` 字节；4 MB profile 的 `0x3f0000` 分区余 `0x26afc0` 字节。2 MB 的 1500 KiB 应用分区无法容纳当前固件。链接期 DRAM 占用 74624 字节（余 49956），IRAM 占用 103371 字节（余 27701）。4/8 MB 布局没有 OTA/文件系统分区。动态媒体缓冲峰值、浏览器协商与播放仍待实机验证。
- **M3：**iAP2 link/CSM、无线身份数据、MFi auth provider、`0x5703`（含实时 SoftAP BSSID）和 `0x4301` 编码器已有代码及主机测试。iAP2 核心处理序号缺口与扩展 ACK；ASan/UBSan 覆盖缺包恢复、255→0 序号回绕、扩展 ACK 触发重传、组合控制位不进入 CSM，以及 READY 状态忽略重复 SYNC。只有纯 `0x40` ACK 帧的 payload 会作为会话数据处理。MFi 状态门槛等待对端 iAP2 link ACK 确认 `0xaa03` challenge 响应后才接受 `0xaa05`；仅入队或 RFCOMM 写入成功不推进认证状态。认证失败/链路死亡/断连会清理状态。RFCOMM 与 iAP2 worker 的有界 RX 队列溢出时会丢弃残留字节并断开会话，避免在失步流上继续处理；`test_bt_port_host.sh` 模拟 SPP 数据回调和队列溢出，但不模拟 FreeRTOS 调度或无线射频行为。`0x1d03` 若只拒绝不支持的消息列表，会按 DiPlay 兼容流程剔除被拒 ID 并重发一次身份消息；其他拒绝不重试。`airplay_pairing_store` 最多 NVS 保存 8 个 controller 标识与公钥。默认 AirPlay 软件 provider 使用 Ed25519、NVS 持久身份、Pair-Setup/Verify callbacks、`/info` 与 mDNS TXT；实际 BR/EDR、iAP2 与 iPhone 无线会话仍未上板验证。
- **M4：**RTSP/Pair-Setup/Pair-Verify、`/info`、主屏 SETUP/RECORD/TEARDOWN、Screen DataStream 解密/组帧和 WebRTC 媒体桥已有代码；默认 provider 与 mDNS 条件启动已接入。8 MB profile 已编译。mDNS 实机广播、iPhone 配对与视频、车机浏览器持续播放尚未验证，故 M4 仍待硬件验收。
- **M5：**浏览器最多跟踪两个 PointerEvent，归一化坐标、边界钳制、旋转、双指 HID、超时/断连释放与 WebSocket 限流均有代码和主机测试。默认 `/info` 声明 800×480 Touch HID，RTSP Event listener 由有效身份 provider 条件启动；单点 down/move/up 与 iPhone 端断连释放未实机验证。

Event Control 断开时，固件会把本地触点释放工作排入 HTTP server task，避免与浏览器触控同时访问输入桥；若 work queue 暂满，250 ms 看门狗会消费 pending 释放标记。由于 event socket 已关闭，此路径清理本地状态，不会假称已向 iPhone 送达 HID release。各阶段验收和外部依赖见 [开发里程碑](docs/ROADMAP.md)。本机当前没有可用 ESP32 串口；`/dev/cu.Bluetooth-Incoming-Port` 是系统蓝牙入口，不是开发板串口。工程构建成功不能替代真实 CarPlay 验证。
