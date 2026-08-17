# Pinned Chiaki protobuf outputs

These files are generated from `lib/protobuf/takion.proto` in the pinned
`xlanor/chiaki-ng` revision documented by `scripts/setup_chiaki_dependencies.sh`.
They are tracked because the devkitPro build image does not include the host
`protoc` and Python protobuf tools needed by nanopb's generator.

The source schema SHA-256 is:

```text
987deff9069dfd805b83ffc0959642c2c15c7ab057f7ec9990c13e8ef03f6d78
```

Do not refresh these outputs without also updating the pinned Chiaki revision
and verifying that the schema hash matches.
