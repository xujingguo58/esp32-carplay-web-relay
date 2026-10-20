# AirPlay 软件身份说明

该组件为开发测试提供 AirPlay receiver identity，不需要外置 MFi 芯片，也不把 MFi challenge 跳过或伪装成成功。iAP2 MFi challenge-response 与 AirPlay Pair-Setup/Pair-Verify 是独立流程。

## 身份与存储

- 首次启动时，组件使用 ESP-IDF 硬件随机源生成 32 字节 Ed25519 seed 和随机 pairing UUID，将它们保存到 NVS `airplay_id` namespace。后续启动读取同一份身份，以保持配对关系稳定。
- 公钥、device ID、`/info` binary plist 和 `_airplay._tcp` TXT 记录由该身份派生或生成。默认 `/info` 声明 800×480 主屏和 Touch HID。
- Pair-Setup 默认码为 `3939`，属于开发默认值，可在 `CarPlay Relay → AirPlay receiver` 的 Kconfig 中修改。
- 当前固件没有启用 NVS encryption 或 Flash encryption。拿到设备或 Flash 镜像的人可以读取 NVS 中的 seed；这是实验实现，不适合公开部署或存放敏感身份。

## 协议实现与限制

- Pair-Setup 按 SRP-6a 和 Ed25519 accessory 签名处理配对；Pair-Verify 使用已配对 controller 公钥验签，并派生会话密钥。controller 公钥记录单独存入 NVS，最多保存 8 个。
- Ed25519 使用 `orlp/ed25519` 上游实现，revision `b1f19fab4aebe607805620d25a5e42566ce46a0e`，保留 zlib 许可证与上游版权信息，见 `components/ed25519/`。
- 该代码按 DiPlay 的协议行为构建，不代表 Apple 签发或批准的 AirPlay receiver 身份，也不保证特定 iOS 版本会接受。当前完成了构建与主机测试；真实配对、mDNS、CarPlay 媒体和触控仍需要 ESP32、iPhone 与车机实测。

不要将 Pair-Setup 码、seed、私钥或设备镜像提交到公开仓库、问题报告或日志。若要部署到共享网络，应先更改默认配对码并为目标硬件评估 NVS/Flash 加密。
