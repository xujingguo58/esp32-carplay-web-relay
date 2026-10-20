# Security policy

## Supported versions

This project has no stable release yet. Security fixes are made against the latest code on the main branch.

## Reporting a vulnerability

Please do not publish exploitable vulnerabilities in a public issue. If the repository's GitHub Security tab offers **Report a vulnerability**, use it to send a private advisory. If private reporting is unavailable, contact the maintainer through the GitHub profile contact method. If that is unavailable too, open an issue asking for a private contact path without including technical details.

Include the affected revision, a concise impact description, and steps to reproduce. Redact personal data, Wi-Fi passwords, pairing codes, and unrelated device identifiers from logs. Do not attach private keys or credentials.

The project contains experimental identity material whose provenance and limitations are documented in components/auth_software_provider/IDENTITY_NOTICE.md. A report about that material should reference the file and affected behavior; do not send copies of private credentials.

## Security-sensitive limitations

- The current firmware is experimental and has not completed hardware validation.
- The development web pairing code is printed to UART. The web service is intended for a WPA2-protected development SoftAP.
- The experimental iAP2 identity is embedded in firmware, and the generated AirPlay identity is stored in NVS. This project does not currently claim production-grade credential protection.
