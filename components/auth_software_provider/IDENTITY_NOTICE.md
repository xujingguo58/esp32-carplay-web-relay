# Bundled experimental iAP2 MFi identity

The two files in `assets/` were extracted from the official DiPlay v0.2.21 release APK:

- `identity.pk8`: unencrypted PKCS#8 P-256 private key.
- `certificate.p7b`: DER PKCS#7 certificate container returned by the iAP2 authentication flow.
- `certificate.der`: X.509 DER certificate extracted from that container for local public/private key matching. The original PKCS#7 bytes remain the certificate payload returned by the provider.

Release APK: <https://github.com/shihabal3amri/DiPlay/releases/download/v0.2.21/DiPlay-0.2.21.apk>

The APK's published SHA-256 is `916c0708beb1022d4ea9678ff6a46dfb6d234f707837a2cbd80d615e8e2bb2e3`. DiPlay's `docs/THIRD_PARTY_NOTICES.md` says the experimental accessory identity was recovered from public Carlinkit C2Air Allwinner V821 firmware; it is not a newly issued identity for DiPlay and is not relicensed as DiPlay source code. This project preserves that provenance.

The identity is experimental and does not establish Apple certification or guarantee that any iPhone/iOS version will accept it. The private key is embedded in the ESP32 firmware image and can be extracted by anyone who obtains that image. This provider performs the normal signed challenge-response; it does not skip the protocol exchange.
