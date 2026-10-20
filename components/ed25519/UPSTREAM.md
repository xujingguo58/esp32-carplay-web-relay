# Ed25519 implementation

This component vendors the portable Ed25519 implementation from
[`orlp/ed25519`](https://github.com/orlp/ed25519), revision
`b1f19fab4aebe607805620d25a5e42566ce46a0e`.

The upstream C sources are retained unmodified. They are licensed under the
zlib license; see [LICENSE.txt](LICENSE.txt). This component uses the
key-generation, signing, and verification source files only. Seed generation
uses the ESP-IDF hardware random source through the platform adapter.
