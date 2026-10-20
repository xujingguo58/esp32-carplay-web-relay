# Contributing

Thanks for helping improve ESP32 CarPlay Web Relay. The project is experimental: software builds and host-side checks do not establish compatibility with an iPhone, an ESP32 board, or a car-head-unit browser. Please describe exactly what you tested.

## Before opening a change

- Search existing issues and pull requests for related work.
- For a large protocol or architecture change, open an issue first so the scope can be discussed.
- Keep each pull request focused and include the reason for the change.
- Preserve upstream copyright headers, license files, and the attribution in THIRD_PARTY_NOTICES.md.
- Do not add credentials, personal certificates, device logs containing private data, or new identity material. The existing experimental identity assets and their provenance are documented in components/auth_software_provider/IDENTITY_NOTICE.md.
- Update README.md, docs/ARCHITECTURE.md, or docs/ROADMAP.md when behavior or project status changes.

## Build

Use ESP-IDF 5.5.3 and the classic ESP32 target. The project documents the 8 MB profile in README.md. From an ESP-IDF terminal, run:

~~~~sh
IDF_TARGET=esp32 SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults.8mb" idf.py -B build-8mb -D SDKCONFIG="$PWD/sdkconfig.8mb" build
~~~~

Do not commit generated build directories or local sdkconfig files. Keep sdkconfig.defaults and the checked-in profile defaults under version control.

## Checks

The GitHub Actions workflow builds the 8 MB profile, runs the host-side C checks in tools, validates the embedded test H.264 stream, and checks the browser page script. To run the same checks locally, load ESP-IDF 5.5.3 and install Clang and Node.js, then run:

~~~~sh
sh tools/ci.sh
~~~~

Several host checks compile ESP-IDF Mbed TLS sources and require IDF_PATH to point to the ESP-IDF checkout. A successful host check is not a substitute for hardware testing.

## Pull requests

Include:

- A concise description of the change and any protocol or memory impact.
- The exact build and checks run, including their results.
- Hardware details and observed results when hardware was available; otherwise say that hardware testing was not performed.
- Any known limitations, compatibility assumptions, or remaining work.

Do not describe a synthetic H.264 stream or a successful firmware build as a successful CarPlay session.
