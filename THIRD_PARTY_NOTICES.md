# Third-party notices

This project references and adapts protocol and browser behavior from the following GPL-3.0
projects. Their original notices remain in their respective upstream repositories; the project
license text is included as [`COPYING`](COPYING).

- **DiPlay / xcertplay** — GPL-3.0, upstream revision `6d3b8a8`; reference for iAP2 link/session
  state and MFi challenge-response flow, AirPlay RTSP and screen framing, TLV8 pairing messages, SRP-6a Pair-Setup equations
  and message flow, Pair-Verify message flow and key labels, ControlCipher framing, binary plist HID commands,
  bounded binary plist parsing/setup replies, HID reports, timing-port packet exchange, and iAP2
  `0x5703`/`0x4301` message field mappings.
  <https://github.com/shihabal3amri/DiPlay>
- **WheelPlay** — GPL-3.0, upstream revision `4ed3536`; reference for browser WebRTC negotiation,
  H.264 passthrough, and normalized touch input. <https://github.com/fython/wheelplay>
- **orlp/ed25519** — zlib license, revision `b1f19fab4aebe607805620d25a5e42566ce46a0e`; portable
  Ed25519 key derivation, signing, and verification for the AirPlay Pair-Setup/Pair-Verify provider.
  The upstream license and source attribution are retained in `components/ed25519/`.
- **ESP-IDF** — Apache-2.0, version 5.5.3; component license texts are in the ESP-IDF checkout.
  <https://github.com/espressif/esp-idf>
- **ESP Peer** — Espressif Modified MIT, version 1.5.7; the fetched component's `LICENSE` file is
  retained under `managed_components/espressif__esp_peer/LICENSE` after dependency resolution.
  <https://github.com/espressif/esp-webrtc-solution/tree/main/components/esp_peer>
- **ESP libSRTP** — see the fetched dependency's `LICENSE` file under
  `managed_components/espressif__esp_libsrtp/LICENSE`.
- **ESP mDNS** — Apache-2.0, version 1.4.3; the fetched component's `LICENSE` file is retained
  under `managed_components/espressif__mdns/LICENSE`.

Dependency versions are pinned in [`dependencies.lock`](dependencies.lock). Do not remove upstream
copyright headers or license files when moving source between components.
