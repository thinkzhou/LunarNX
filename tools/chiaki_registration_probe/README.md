# Chiaki LAN registration probe

`run_macos.sh` compiles the pinned Chiaki registration, crypto, HTTP, socket,
stop-pipe, and thread sources and runs them against a
loopback fake PlayStation endpoint. The fake endpoint exercises the real UDP
search and TCP registration request sockets for PS4 and PS5, then deliberately
returns a registration rejection. This verifies transport selection and request
routing without needing a console or exposing a real Account ID or PIN.

It does not emulate Sony's encrypted success response and cannot prove libnx
socket-service capacity or real-console compatibility. Those boundaries still
require a Switch hardware test.
