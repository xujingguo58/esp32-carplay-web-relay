# 开发里程碑

## 当前状态

最近一次烧录到经典 ESP32 Rev 3.0 开发板的固件（2026-10-22）镜像为 `0x1b4f10` 字节，`0x7f0000` 单应用分区剩余 `0x63b0f0` 字节；当时 `idf.py size` 显示 DRAM 使用 89360 字节（余 35220）、IRAM 使用 124763 字节（余 6309），启动日志检测到 4096 KB PSRAM。2026-10-23 的源码优化尚未烧录；最新构建数据见下方。4/8 MB profile 均没有 OTA/文件系统分区。用户确认 M2 合成 H.264 浏览器视频在实机播放正常；M3–M5 仍需 iPhone/真实 CarPlay 环境验证，不把合成视频当成真实 CarPlay 成功。

## 里程碑一览

| 阶段 | 内容 | 验收条件 | 状态 |
|---|---|---|---|
| M0 | 架构与计划审阅 | README、架构和里程碑形成首版实现范围 | 已完成 |
| M1 | ESP-IDF 平台骨架 | SoftAP 可同时接入 iPhone/车机；HTTP 页面可打开；经典蓝牙服务可扫描/配对；关键资源余量可观测 | SoftAP、HTTP 和 PSRAM 已在板上运行；蓝牙可见性、配对及并发接入待测 |
| M2 | 浏览器媒体样机 | 使用合成 H.264 测试流，通过 ESP32 WebRTC 发到车机浏览器；触控消息可往返；记录帧率、丢包、延迟和内存峰值 | 已完成：用户确认实机视频播放正常；量化资源与延迟数据未单独记录 |
| M3 | DiPlay 协议核心移植 | 完成蓝牙/iAP2 启动阶段；在目标测试环境中通过认证和无线会话启动；连接状态可诊断 | 本机 Car Audio CoD/EIR 与网页就绪后可发现已实现并有主机测试；MFi challenge-response 状态门槛、软件 signer、`0x5703`/`0x4301` 编码和 `0x1d03` 兼容重试已有代码；一次旧固件会话到达 Pair-Setup/Verify 与 MFi auth-setup；蓝牙设置可见性和最新固件启动过程仍待复测 |
| M4 | CarPlay 视频接收 | AirPlay 会话建立，H.264 可解密/组帧并在车机浏览器连续显示；断连可清理并重新连接 | RTSP/配对/Screen DataStream/WebRTC 桥和默认 AirPlay provider 已实现并可构建；旧固件日志在 RTSP SETUP 阶段因 Event/Timing listener 资源失败而关闭控制连接；当前代码已合并 listener 轮询并优化内存，真实 Screen H.264 和恢复待新固件实测 |
| M5 | 触控回传 | 单点触摸 down/move/up 映射准确；断线强制释放；双点输入已有软件支持 | 输入映射、Event key 派生、双向 ControlCipher、HID 命令与 Event listener 已接线；默认 `/info` 声明 800×480 DiPlay Touch HID；网页最多跟踪两个 PointerEvent，主机覆盖配对、动态几何、触点槽位和超时释放；iPhone 端到端触控与释放行为待实测 |
| M6 | 音频和语音 | CarPlay 音频可播放；若目标范围要求，完成麦克风上行、Siri/通话路由和权限处理 | 未开始 |
| M7 | 稳定性与兼容 | 长时间运行、弱信号、重复连接、浏览器刷新和车机重启均符合项目定义的验收指标 | 未开始 |
| M8 | 发布准备 | 许可证/第三方声明、认证资源管理、密钥保护、配置说明和已知限制齐全 | 未开始 |

## 详细阶段

### M0：方案与项目边界

当前按以下工作假设推进，具体板卡和功能范围可按实机结果调整：

- 使用经典 ESP32 + PSRAM 开发板作为首个目标。
- 首版使用 2.4 GHz SoftAP，不移植 Wi-Fi Direct。
- 首版车机以浏览器为显示端，优先 WebRTC H.264 直通。
- 首版至少包含画面和触控；音频是否纳入 MVP 由项目确认。
- 明确目标 iPhone/iOS 与车机/浏览器型号，并确认可用的认证资源路径。

**通过条件：**README、架构和里程碑内容齐备，形成首轮开发假设。已完成并进入 M1。

### M1：ESP-IDF 平台骨架

- 创建 ESP-IDF 工程结构、组件边界和统一日志/错误码。已完成 `main`、`net_port` 和 `web_gateway` 的初始划分。
- 实现 SoftAP、DHCP、状态页和配置项；目前已实现 SoftAP、DHCP、客户端数状态、首页和 `/api/status`，可查看当前及历史最低内部堆与 PSRAM 余量，并通过 ESP-IDF 5.5.3 `esp32` 编译；尚未实机验证并发接入。
- 已加入经典蓝牙 BR/EDR 客户端、iPhone inquiry、远端 iAP2 SDP 查询和 RFCOMM 连接。当前补上 Audio/Video Car Audio CoD、本机 EIR 名称及浏览器就绪门控的 connectable/general-discoverable 模式；`tools/test_bt_port_host.sh` 验证门控、CoD/EIR 参数与 RX 溢出清理。iPhone 可见性、配对与 RFCOMM 行为仍待新固件实测。
- 当前状态 API 提供当前/历史最低堆与 PSRAM 余量、接入客户端数、WebRTC 丢帧和触点计数。当前接入板启动日志确认检测到 4096 KB PSRAM；8 MB 调试 profile 面向经典 ESP32 Rev 3.0 或更新版本。

**通过条件：**连续重启后热点和页面可恢复；手机和车机保持在同一子网；蓝牙连接可观测且不会使热点服务失效。SoftAP、HTTP 页面和 PSRAM 已在当前板卡启动；BR/EDR 与 iPhone RFCOMM 行为仍待实测。

### M2：WheelPlay 风格浏览器媒体样机

- 静态网页已编入固件，使用 `esp_peer` 单浏览器发送合成 H.264 测试流（320×240@10fps）。
- WebSocket 交换 SDP 和 ICE，网页接收触摸事件后回显；触控 HID sink 仍显示未连接。
- 状态页采集浏览器帧/丢包/RTT、ESP32 发送帧/丢帧、当前内部堆/PSRAM 余量和两类内存的启动以来最低余量；具体测试流为 Annex-B，40 个 access unit、4 个 IDR、最大 access unit 15381 字节。
- 页面明确告知测试图像不是 CarPlay 实际接收结果；当前调试构建将网页配对码固定为 `11111111`，未配对时不启动 WebRTC。该固定码仅供本地调试。

**最新构建记录：**ESP-IDF 5.5.3 classic `esp32`。2026-10-23 commit `31908bb` 的 8 MB 清洁 CI 构建关闭未使用的 BLE/GATT host，保留 BR/EDR/SPP；镜像为 `0x13b6b0` 字节，`0x7f0000` app 分区余 `0x6b4950` 字节（84%）。`idf.py size` 显示 DRAM 使用 71360 字节、剩余 53220；IRAM 使用 96023 字节、剩余 35049。相较此前同 CI 默认配置、BLE host 启用的清洁构建 `0x1696e0`，镜像减少 188464 字节（12.7%）；该静态体积变化不等于运行时 internal heap 的实测增量。为实际 8 MB 板卡配置的 `sdkconfig.8mb` 此前也通过构建：镜像 `0x18ad70`，分区剩余 `0x665290`（81%）；DRAM 使用 86832 字节、剩余 37748，IRAM 使用 124971 字节、剩余 6101。4 MB 无 PSRAM、Classic-only profile 当前构建通过，镜像 `0x157da0`，`0x3f0000` app 分区余 `0x298260`（66%），验证了没有 PSRAM 时的 internal 回退编译路径。Screen H.264 原位解密通过 600 KiB 主机用例；SDP answer 队列内存优化也通过完整 CI。源码尚未烧录。8 MB profile 的上一次实机镜像记录在本文档开头；2 MB 默认 app 分区不再适用。4/8 MB profile 无 OTA/文件系统分区。静态占用不含运行时任务栈、WebRTC/媒体缓冲和峰值。

**实机验证记录（2026-10-22）：**用户确认合成 H.264 视频在浏览器播放正常；启动日志确认 4 MB PSRAM。`esp_peer_open` 与 ICE candidate 配对成功。8 KB 的 `webrtc_peer` 任务栈曾在 ICE Binding 期间溢出，已增至 16 KB；后续一次握手日志的栈余量为 4680 字节且未再崩溃。触控往返及帧率、丢帧、RTT、播放期间最低堆的量化记录未单独保存。

`node tools/test_web_gateway_page.mjs` 从 `web_gateway.c` 实际嵌入 HTML 中提取 JavaScript，先解析整页脚本语法，再检查 SDP helper 只在所有 offer media `setup` 属性均为 `actpass` 时把浏览器 answer 设为 `passive`；active/passive/mixed/no-setup 输入均保留浏览器生成的 answer。它还检查实际触控 helper 对上下/左右黑边的按下拒绝、首帧前尺寸回退及 move/up 越界夹取。该测试覆盖网页脚本 helper，不替代 ESP Peer 与目标浏览器的 DTLS 实际协商或触控实测。

### M3：协议核心移植

- 已将 BR/EDR/RFCOMM 放入 `bt_port`，iAP2 link/CSM framing、同步、确认、重传、限长队列放入 `iap2_core`。
- `bt_port` 与 app 层 iAP2 worker 使用有界 RX 队列；任一级 RX 队列满会清空待处理字节并主动断开 RFCOMM，状态事件优先排入队列，避免丢字节后沿用损坏的 iAP2 状态。`tools/test_bt_port_host.sh` 用 ESP-IDF API 替身模拟 SPP 回调，验证 RX 队列溢出触发 disconnect、清除旧数据并保留断连状态事件；仍未模拟真实 FreeRTOS 调度或射频行为。
- `iap2_core` 按 DiPlay 链路规则处理有序序号、累计 ACK 和扩展 ACK；收到序号缺口时发送受窗口限制的 missing-sequence 列表，并对对端请求的未确认包立即重传。该核心不缓存乱序 CSM，所以会把缺口到当前包都列为待重传，避免静默丢弃后续包。只有纯 ACK `0x40` 的 payload 才分发到 CSM；组合 ACK/扩展 ACK 的 payload 保持为链路控制数据。已建立链路收到重复 SYNC 不会重置接收序号。`tools/test_iap2_core_host.sh` 使用 ASan/UBSan 检查序号缺口/恢复、序号 255→0 回绕、扩展 ACK 重传、组合控制位隔离和重复 SYNC；尚未用真实 iPhone RFCOMM trace 验证协商兼容性。
- `iap2_identity` 构建无线 IdentificationInformation；`auth_port` 提供证书读取和 challenge 签名接口；`auth_software_provider` 已内嵌 DiPlay v0.2.21 官方 APK 中的 P-256 PKCS#8 私钥/PKCS#7 证书，启动时检查证书与私钥匹配并注册软件 signer，无需外置 MFi 芯片。身份来源及固件内密钥可提取的限制见 `components/auth_software_provider/IDENTITY_NOTICE.md`。
- 主流程已按 iAP2 消息分发身份、认证、Wi-Fi 配置请求和 `0x4300` CarPlay availability；断连或认证失败时清除当前会话状态。
- 新增 `iap2_carplay`：按 DiPlay schema 编码 `0x5703`（可选 SoftAP BSSID、SSID、passphrase、安全类型和信道）及 `0x4301`（wireless SSID/passphrase/channel/IP/security、AirPlay 端口、device identifier、公钥和 sourceVersion）。`0x5703` 已接入认证后的 `0x5702` 处理；`0x4301` 条件发送路径要求 iAP2 MFi 认证成功、授权 AirPlay identity provider 可用且 RTSP listener 正在运行。
- 新增 `airplay_identity_port` 与可选 `app_register_deployment_providers()` 钩子，并接入默认 `airplay_software_provider`。默认 provider 首次启动生成并保存 Ed25519 seed/UUID，使用可配置 Pair-Setup 码（默认开发码 `3939`），接入 Pair-Setup/Verify callbacks、controller NVS store、动态 `/info` plist 和 mDNS TXT。部署 hook 可以覆盖默认实现；RTSP/mDNS 启动失败时仍不会发送 `0x4301`。身份来源与存储限制见 provider 的 `IDENTITY_NOTICE.md`。
- 新增 `airplay_pairing_store`：`nvs_flash_init()` 后初始化，在 NVS 持久保存最多 8 个 AirPlay controller 标识和公钥；其 save/lookup 函数签名可直接作为 Pair-Setup 与 Pair-Verify provider callback。记录采用版本化、限长的固定格式并校验损坏/重复项；只保存 controller 公钥等公开配对元数据，不保存附件或 MFi 私钥。ASan/UBSan 主机测试覆盖 upsert、满容量、移除、清空、损坏数据和提交失败。
- 新增 `time_port`，统一提供 ESP-IDF 单调微秒与毫秒时钟；`app_main`、WebSocket 触点/限流、AirPlay Screen 与 UDP timing、WebRTC 测试流调度不再直接调用平台计时函数。主机测试覆盖零点、毫秒截断和较大运行时间换算。
- 新增 `iap2_mfi_auth` 状态门槛：只有对端在 iAP2 link 层确认了由认证 provider 签出的 `0xaa02` challenge 对应 `0xaa03` 响应，才接受 `0xaa05`；入队、RFCOMM 写入或未经 ACK 的超时重发都不能推进认证状态。失败、iAP2 link DEAD、RFCOMM 断连/重连都会重置状态。主机 ASan/UBSan 测试覆盖越级 success 拒绝、等待 ACK、重复 challenge 和失败重置。
- `0x1d03 IdentificationRejected` 仅含参数 6/7 且列出格式合法的非认证消息 ID 时，`iap2_identity` 会从 sent/received 列表中过滤这些 ID 并在当前会话重发一次；空列表、畸形内容、其他被拒身份字段或认证消息 ID 不触发重试。此路径参考 DiPlay 对旧 iOS 拒绝未知 `0x4300/0x4301` 的处理。
- `tools/test_iap2_carplay_host.sh` 通过 ASan/UBSan 验证嵌套字段、Wi-Fi 配置、端口字节序、字符串结尾、缓冲不足和配置边界；ESP-IDF 构建验证了 provider/启动条件代码。软件 MFi provider 已接入并在启动时执行身份 key-pair 检查。此前 iPhone 会话日志显示 iAP2 CSM 有交互，并推进到 AirPlay Pair-Setup/Verify 与 MFi auth-setup；但未保存完整 RFCOMM trace，当前固件的 BR/EDR 可见性、challenge-response、热点加入与无线会话仍需复测。DiPlay 实验身份不代表 Apple 认证，且内嵌在固件中的私钥可被提取。
- `tools/test_iap2_mfi_auth_host.sh` 通过 ASan/UBSan 验证未经 challenge 签名响应及 iAP2 link ACK 不得进入 `authenticated`，并覆盖失败、重复 challenge 与 reset；`test_iap2_core.c` 还检查 ACK callback 先于同一 link frame 内 piggyback 的 `0xaa05` 消息处理，以及扩展 ACK 和缺口重传。
- `tools/test_iap2_core_host.sh` 通过 ASan/UBSan 覆盖累计 ACK callback 顺序、入站序号缺口的扩展 ACK 列表、对端扩展 ACK 触发即时重传、组合控制位 payload 不分发为 CSM，以及 READY 状态重复 SYNC 不改写当前接收序号。真实 RFCOMM trace 尚未采集。
- iAP2 identity ASan/UBSan 主机测试覆盖 `0x1d03` 不支持消息列表解析、重复 ID 去重、认证 ID/其他字段/畸形内容拒绝，以及重建后的通告列表。
- `airplay_pairing_store` ASan/UBSan 主机测试覆盖空库、读写回环、同标识更新、8 条容量上限、并发保存、删除、清空、无效长度、损坏记录与 NVS commit 失败。

**通过条件：**日志能显示完整的阶段状态和失败原因；断连后可干净复位状态机；challenge-response 必须由软件 signer 执行并经过 link ACK 门槛，不得用伪造成功状态替代。身份来源和固件内可提取性记录在随附声明中。

### M4：AirPlay H.264 媒体链路

- 已按 DiPlay Screen DataStream 实现 128 字节头校验、ChaCha20-Poly1305 解密、带 `avcC` 标记或裸 AVCDecoderConfigurationRecord 的 SPS/PPS 转 Annex-B、长度前缀 NAL 转 Annex-B 和发送时间解析；解析器验证 SPS/PPS NAL 类型与 AVCDecoderConfigurationRecord 保留位，显式 `hvcC` 配置会按当前 H.264-only 范围拒绝。新增主机用例验证裸记录兼容和 H.265 拒绝。
- 加入 mbedTLS ChaCha20/Poly1305 与 HKDF-SHA512 构建配置及启动自检；增加以共享密钥和 stream connection ID 派生 `DataStream-Output-Encryption-Key` 的函数，并在启动自检中校验固定 HKDF 向量。主机侧 codec 测试覆盖头长、时间戳、NAL 转换和 SPS/PPS 提取。
- 已加入单 peer TCP listener，按帧限长接收 Screen DataStream，使用 PSRAM（若可用）或受限内部堆分配缓冲。协议 body 硬上限对齐 DiPlay 为 8 MiB（包括 AEAD tag）；RX 缓冲按每次最多 1 KiB 的 TCP 收包量几何扩容，不因头部声明提前分配整帧，接收时直接写入该缓冲；ChaCha20-Poly1305 在同一 RX 缓冲原位解密，随后将 NAL 长度覆盖为 Annex-B start code，去掉了第二份 access-unit 大小的 plaintext 缓冲。WebRTC staging buffer 限制为 8 MiB + SPS/PPS 空间，异步发送期间仍可能同时持有 RX 和 staging 两份完整帧数据，最大帧受 PSRAM 可用量与碎片影响。默认和 4 MB profile 未启用 SPIRAM；8 MB Flash 调试 profile 已启用 40 MHz PSRAM 自动检测，并将 mbedTLS 工作分配放入 PSRAM，允许无 PSRAM 时继续启动。Flash 容量不代表板卡带 PSRAM；当前 8 MB profile 要求经典 ESP32 Rev 3.0 或更新版本。RX 分配失败会关闭该 Screen 会话，避免 nonce 帧计数失步；媒体 staging 分配失败会丢帧并触发 IDR 恢复。缓冲高水位日志输出 body/RX 容量、RX 分配域、free/largest internal block、PSRAM 与 Screen task stack high-water。`test_airplay_screen_stream_host.sh` 使用 IDF 5.5.3 同版本 Mbed TLS 验证 600 KiB access unit 原位解密、Annex-B 转换、avcC/SPS/PPS、发送端/到达时间戳、坏 tag 拒绝，以及认证失败不推进帧 nonce；该用例覆盖软件集成到 WebRTC 发送 API 边界，不代表真实 `esp_peer` 发送或目标板 PSRAM 碎片行为。每个输出 key 只接受一个 TCP peer；AEAD tag 错误或 peer/listener 非预期退出会结束所属 RTSP 会话，不重置计数器后复用同一 key。活动 Screen 的重复 SETUP 复用原 listener；TEARDOWN 后拒绝重复的 `streamConnectionID`，每个 Pair-Verify 会话最多记录 8 个 Screen key ID。新增 Screen lifecycle 和 RTSP session 主机测试验证连接退出与 key ID 复用规则。
- 本轮 RAM/栈优化将 server `/info` plist 从 3.5 KB inline 字段拆为 PSRAM 优先缓冲，并让 server 创建的 RTSP session 显式借用同一份只读数据，避免每个 RTSP session 再复制一份；原公开 session create API 仍保留深拷贝语义，无 PSRAM 时回退 internal。RTSP 收包 scratch 从 10 KB RTSP task 的局部栈移动到 PSRAM 优先分配的 connection 对象，并记录实际分配位置；任务仍分配原有 10 KB internal stack，需实机重新记录高水位后再评估缩栈。`RECORD` 仅在主屏 SETUP 成功后接受。主机 RTSP session ASan/UBSan 用例验证深拷贝/借用生命周期和 RECORD 前置条件。
- WebRTC answer SDP 队列改为只存指针和长度，SDP 本体 PSRAM 优先分配、在送入 `esp_peer` 后擦除释放；移除了原队列项和 worker 局部变量中的 3 KiB 定长 SDP 数组，降低内部堆占用与栈峰值。`webrtc_peer` 仍用 16 KiB 内部栈，以保留此前 PeerBinding 栈溢出修复的余量；缩栈必须等本次固件在设备上经历 ICE/DTLS 和实际视频收发并记录新的最小剩余栈后再决定。该优化已通过编译，设备侧内存增量尚未测量。
- 为降低 SETUP 后 Event 和 Timing listener 对 internal DRAM 的叠加峰值，Event TCP 与 Timing UDP 均由 RTSP task 的 `select()` 循环复用；Event 不再有固定 4 KB 的独立 task stack、TCB 和 semaphore。Event state 与 1200 字节 RX 缓冲优先使用 PSRAM，缺少 PSRAM 时回退 internal。RTSP server、session、Pair-Setup、Pair-Verify、SRP 与 pairing wrapper 状态也改为 PSRAM 优先。旧分析将 server/session 各估为 3.6–3.7 KB；当前 8 MB ELF 的 DWARF 实测类型大小为 128/208 字节（server/session），连同 372/460/132/8 字节的 pairing 状态合计 1308 字节，约 1.3 KB，而非 7.3 KB。`stopped_semaphore` 和 Timing `task_create_failed` 日志属于旧 listener-task 实现；当前源码没有这些分配点。旧日志只证明当时 internal heap 余量很低、连续块很小，不能单独证明当前失败或把所有堆下降归因于 WebRTC。
- 默认、4 MB 和 8 MB profiles 现都关闭未使用的 BLE/GATT host，同时保留 Classic BR/EDR、Classic SMP 与 SPP。8 MB CI 镜像相较此前同配置、BLE host 启用的版本从 `0x1696e0` 降至当前 `0x13b6b0`（减少 188464 字节，12.7%）；实际板卡 `sdkconfig.8mb` 历史 profile 为 `0x18ad70`，当前 4 MB 无 PSRAM profile 为 `0x157da0`。完整 `tools/ci.sh` 的固件构建、host checks、网页逻辑和 H.264 测试流均通过。Event 解密使用 RTSP task 的 10 KB internal stack；板上堆余量、任务高水位、蓝牙发现、真实 AirPlay SETUP 和视频仍待实测。本机目前只有 Bluetooth-Incoming-Port，没有 ESP32 USB 串口，因此未烧录和监控设备。
- 已加入 `airplay_media_bridge` 回调适配器：配置帧更新 WebRTC H.264 参数集，解密 access unit 进入 `media_bridge` 的有界单帧队列；WebRTC 发送在 `esp_peer` 所属任务执行，AirPlay sender 时间优先转换为会话相对 90 kHz PTS。sender 时间缺失或回退时，mapper 切到单调到达时间并锚定在上一 PTS；RTP 32 位回绕保留。主机 ASan/UBSan 用例覆盖 sender/arrival 时钟、时钟回退及回绕。SETUP adapter 从 Pair-Verify session 派生 output key、绑定 SoftAP IP 启动 Screen listener，并将消息交给媒体桥；新 RTSP session router 已调用该 adapter。WebRTC 停止时关闭新的帧提交入口并等待活动提交结束，再删除有界队列和 PSRAM/内部帧缓冲，避免 Screen 接收任务与浏览器断连清理并发访问已释放资源；`tools/test_media_frame_gate_host.sh` 用 pthread 测试活动生产者与停止/排空交错，验证已进入的提交退出前不会认为资源可销毁。该测试模拟互斥串行化策略，不代替 FreeRTOS 实机压力测试。
- `tools/test_airplay_media_bridge_host.sh` 用 ASan/UBSan 独立检查适配器的消息类型、时间戳、非法参数和不支持消息；配置和视频载荷的端到端软件路径由 `test_airplay_screen_stream_host.sh` 覆盖。两者均使用替身捕获接口调用，不代替 `esp_peer` 实机发送验证。
- Pair-Setup M1/M3/M5 与 Pair-Verify M1/M3 状态机已接入 RTSP 路由；默认软件 provider 提供配对码、Ed25519 accessory 签名/controller 验签、controller NVS 存储、有效 `/info` plist 和 `_airplay._tcp` TXT。RTSP/Screen/Event/timing 端口与会话路由已有主机测试及 ESP-IDF 构建验证。默认 Pair-Setup 码是开发配置（`3939`），应在测试网络上更改。mDNS 实机广播、iPhone Pair-Setup/Verify、Screen 实收和 H.264 播放仍未验证。
- 已新增有界 RTSP request parser：限制头 2 KB、正文 32 KB、header 数和字段长度；处理部分到达、binary body、重复/非法 Content-Length 和 CSeq 回显。另有 binary plist subset reader/writer，校验 trailer、object offset/reference 和 payload 限长，可解析初始 SETUP 的 `timingPort`、主屏 SETUP 的 `streams/type/streamConnectionID`，并构造初始 `timingPort`/`eventPort` 及 Screen `dataPort` 响应；host ASan/UBSan 测试通过。
- 新增 `airplay_timing` UDP endpoint：校验 32 字节 PT_REQUEST/PT_RESPONSE，按 DiPlay 格式回送请求时间戳并每秒发送 timing request；主机 ASan/UBSan 测试覆盖包结构、回送字段、延迟/偏移计算和畸形/陈旧响应。时钟偏差暂只记录，不应用于媒体时间轴。
- 已移植限长 TLV8 编解码组件，处理 255 字节分片、连续片段重组、重复字段分隔和畸形输入；主机 ASan/UBSan 检查通过。它只提供配对消息 framing，不负责 RTSP 路由或完整 Pair-Setup 状态编排。
- 已实现 Pair-Setup SRP-6a 服务端计算与 TLV8 状态编排；主机互测覆盖 SRP proof、HKDF/ChaCha20-Poly1305、错误 proof、controller 签名拒绝和存储失败。默认软件 provider 通过 vendored Ed25519 实现 accessory 签名和 controller 验签；RFC 8032 向量、签名/验签与篡改拒绝通过主机测试。
- 已实现 Pair-Verify M1/M2/M3/M4 状态机、X25519、Control/Event 密钥派生与 Screen key 派生。主机测试覆盖 RFC 7748 向量、成功往返、密钥方向、篡改签名和随机源失败。完整软件组件已接入默认启动路径；身份接受情况、mDNS 广播、真实 iPhone 会话、Screen H.264 以及浏览器连续播放/重连、资源峰值和设备侧清理仍待实测。
- `media_keyframe_gate` 丢帧恢复、Event `forceKeyFrame` 控制与其 RTSP/CSeq/加密路径已有主机检查；它们不能替代设备端 iPhone 响应、IDR 恢复和浏览器解码验证。

**通过条件：**实际 iPhone 会话的 H.264 画面在目标车机浏览器连续显示；分辨率/帧率变更和断线恢复行为可重复。

### M5：触控与控制

- Event Control 断开时，RTSP server 会把触控释放工作排入 HTTPD work queue，由 HTTP server task 串行调用 `input_bridge_release_all()`。若队列暂满，pending 标记由下一次 250 ms 看门狗工作消费。连接已断时 HID sink 不可达，因此该回调清理本地状态；浏览器 WebSocket 断连和触点超时仍分别执行原有释放路径。

- 已实现归一化坐标、0/90/180/270 度旋转、两触点状态、12 字节双指 HID 报告和无效事件拒绝。`tools/test_input_bridge_host.sh` 断言同一点经四种旋转映射后的 HID 像素坐标。HTTPD work queue 上的 250 ms 看门狗独立于浏览器请求检查 1 秒触点超时；WebSocket 断开也会立即清理触点。
- 网页最多同时跟踪两个 PointerEvent，每个触点独立做 move 限频；事件先发送到 ESP32 并回显。按下只接受显示画面内坐标，拖动/抬起超出画面时钳制到边缘，指针捕获丢失或页面失焦时发送 cancel。上游 DiPlay 的实际触控路径使用 Touch HID UID `0x2a2a2a2a` 经 AirPlay 加密事件连接发送 `hidSendReport`。
- `airplay_bplist_get_main_display_size()` 读取 provider `/info` 中主屏几何。默认 AirPlay provider 声明 800×480，并由 `airplay_identity_port` 检查主屏与唯一 Touch HID（UID `2a2a2a2a`、display UUID、vendor/product ID、109 字节双指报告描述符）匹配；其他 provider 也必须通过相同检查。坐标量化采用 `normalized × extent`，旋转限于 0/90/180/270 度；几何改变前先释放旧触点。主机测试覆盖描述符拒绝和尺寸映射；iPhone 对描述符的实际识别仍待实机确认。
- 已将 `input_bridge` 的 report callback 接入 `airplay_control`；实现有限字段的 `bplist00` `hidSendReport` 编码、`POST /command` RTSP 消息和双向 ControlCipher AEAD 帧。新 cipher 使用 2 字节长度字段作为 AAD、4 字节零前缀加 8 字节小端计数器 nonce，支持 TCP 分片和连续帧、最大 4096 字节明文，并在 tag/长度错误时将会话置为失败。Pair-Verify 已派生并暴露 Event read/write keys；入站 event RTSP 请求可按完整消息回加密 200，应答和 HID 命令共用发送计数器。主机 ASan/UBSan 测试覆盖分片/连续帧、方向、容量、空帧、超长与篡改 tag；RTSP event stream 主机测试覆盖消息分片/合并和请求/响应分发。
- 已新增 `airplay_event_port`：绑定 SoftAP IPv4 的单连接 TCP listener，接受后 attach event ControlCipher、收发密文并在断连时 detach/通知上层。初始 SETUP 可创建 listener 并返回 `eventPort`，HID `POST /command` 使用 event 方向密钥加密。默认 AirPlay provider 已接入启动；网页 `hid=not_connected` 表示当前没有活动 iPhone Event 通道。单点触摸、断连释放和重连需在 iPhone/车机上实测。
- Event listener 的非预期退出（包括连接前的 listener/attach 失败）会将 Event 通道标为失败、禁用触控并保留 RTSP 会话；已连接的 Event 密钥在同一 Pair-Verify 会话中只允许使用一次。TEARDOWN 后若客户端仍在当前 RTSP 会话重发 SETUP，服务返回不可用并要求客户端重建会话，避免加密计数器归零后复用 AEAD nonce。`tools/test_airplay_event_lifecycle_host.sh` 覆盖活动 listener 可复用、已用密钥禁止重开、非预期退出与主动关闭判定。Event 断开仍会先通知本地输入桥释放触点；实际 iPhone 释放与重连尚未上板验证。
- `/ws` 加入 120 消息/秒、120 条突发额度的令牌桶；超限释放当前触点并关闭 WebSocket。`tools/test_web_gateway_rate_limit_host.sh` 通过 ASan/UBSan 检查令牌补充、突发限制和重置。网页最多同时跟踪两个触点，`tools/test_input_bridge_host.sh` 检查释放第一个槽位后第二个触点不跳槽，并检查超时释放。
- WebSocket 必须先通过网页配对码，未配对时不启动 WebRTC 或接收触控；当前调试构建固定使用 `11111111`，并通过 UART 日志输出。连续五次错误后锁定 30 秒。`tools/test_web_gateway_pairing_host.sh` 通过 ASan/UBSan 检查正确/错误码、大小写、输入长度、失败锁定和恢复。
- **待验收：**实机确认单点 down/move/up 坐标正确，并验证刷新、断连和超时释放行为。浏览器配对码目前是开发阶段的 UART 一次启动码，TLS 和适合产品交付的 provisioning 仍未实现；Event listener 的会话授权也需要真实 AirPlay 配对 provider 后实测。

- `tools/test_touch_hid_bridge_host.sh` 在一个调用链中验证归一化 down/move/up 坐标、12 字节触点报告、Touch UID，以及 HID binary plist 的 `uuid`/`hidReport` 与触点报告内容一致；该软件测试不代替 iPhone 端到端验证。

**通过条件：**目标画面坐标与车机触摸区域对应；浏览器刷新或断网不会留下卡住的触点。

### M6：音频和麦克风

- 确认 CarPlay 下行音频格式与浏览器/WebRTC 可接受格式。
- 评估 ESP32 的解码/编码占用，决定直接 PCM、硬件 codec 或更强处理器路径。
- 设计浏览器音频播放、回声/缓冲策略及麦克风上行权限；测试 Siri 与通话切换。

**通过条件：**媒体、导航提示、Siri 和电话音频路由符合 MVP 定义；若 ESP32 无法满足，提交外部音频/协处理器方案供确认。

### M7：稳定性和兼容性

- 长时间运行、重复配对、蓝牙断开、热点切换、弱信号、车机浏览器刷新。
- 记录 heap/PSRAM 最低值、任务栈余量、RTP/WebRTC 统计及重连原因。
- 建立至少一台目标 iPhone 和一台目标车机的回归矩阵。
- 根据指标评估分辨率、帧率和 AP 信道配置；必要时转为 ESP32 + 外部 AP/网关架构。

**通过条件：**各项性能阈值由项目先行定义，所有关键故障都有可读诊断，不发生不可恢复的资源泄漏或会话残留。

### M8：发布准备

- 检查 GPL-3.0 和依赖许可证，整理版权、许可证、源代码提供方式。
- 将认证资源与公开源码/固件产物分离；审查 secure boot、flash encryption、调试口和生产密钥流程。
- 发布安装、配网、已知限制、支持芯片和浏览器的文档。

**通过条件：**许可证、认证资源管理和发布产物经项目负责人审阅。

## 风险关卡

1. **M1 → M2：**若经典蓝牙和热点同时工作造成不可接受的断连或吞吐，暂停协议移植，重新选择网络拓扑/硬件。
2. **M2 → M3：**若浏览器端无法在目标车机解 H.264 WebRTC，先决定支持的浏览器或外部媒体网关方案。
3. **M3 → M4：**若认证身份来源或有效性未确认，不继续投入完整无线协议集成。
4. **M4 → M5：**若 H.264 解密和转发无法在目标分辨率持续运行，先降低范围或转向更强主机。
5. **M5 → M6：**音频编解码若超出 ESP32 资源预算，单独评估外部 codec/主机协处理，不拖累视频链路验收。

## 计划依据

- [DiPlay / xcertplay](https://github.com/shihabal3amri/DiPlay)
- [WheelPlay](https://github.com/fython/wheelplay)
- [Espressif ESP-WebRTC Solution](https://docs.espressif.com/projects/esp-adf/en/latest/solution-center/esp-webrtc-solution.html)
- [ESP-IDF HTTP Server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
