# Security policy

## Reporting a vulnerability

Report privately through GitHub: **Security** tab > **Report a vulnerability** ([form](https://github.com/lythka/hao-runtime/security/advisories/new)).
Do not open a public issue for a vulnerability. We acknowledge a report within 7 days and tell you what we will do.

## Supported versions

Only the latest release. hao is a developer preview (0.y.z); fixes ship in the next release.

## Scope

In scope: hao-runtime: translation of ARM code, its caches, its kernel extension and the tools that install it, where a program or user gains more than macOS with the protections in docs/security.md turned off would allow.

Out of scope:
- The protections hao turns off on purpose; [docs/security.md](https://github.com/lythka/hao-patcher/blob/main/docs/security.md) lists them.
- macOS itself: report to [Apple](https://security.apple.com).
- OpenCore, OpenCore Legacy Patcher and the kexts they ship: report to their projects.
