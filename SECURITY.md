# Security Policy

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting feature for this
repository. Do not include access tokens, refresh tokens, Xbox identifiers,
email addresses, public IP addresses, or complete diagnostic logs in a public
issue.

If private vulnerability reporting is not available, open a public issue that
contains only a short, redacted description and ask the maintainers for a
private contact channel.

## Sensitive local files

LunarNX stores Microsoft/Xbox authentication material on the SD card under
`sdmc:/switch/LunarNX/`. Treat that directory as sensitive. Do not publish it,
include it in bug reports, or commit it to this repository.

Diagnostic logging and raw Xbox response tracing are disabled in release
builds. Builds made with `APP_DIAG=1`, `NETWORK_DIAG=1`, or
`XBOX_RESPONSE_TRACE=1` can contain account, console, network, and session
metadata. Redact logs before sharing them and delete them after debugging.

## Project status

LunarNX is an unofficial, early-stage homebrew client. It is not affiliated
with Microsoft, Xbox, or Nintendo, and it should not be treated as a security
boundary for a Microsoft account or home network.
