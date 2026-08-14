# LunarNX development bridge

The bridge publishes versioned Nintendo Switch NRO builds and receives bounded
development logs through a Cloudflare Worker backed by Workers KV.

## Build and publish with one version

Use the unified entry point for release-like development builds. It chooses a
short version such as `0.1.0-d260814.1` before compilation, writes that exact
version into the NRO, publishes it under the same Cloudflare version, and uses
it in the optional Feishu notification:

```sh
tools/dev_bridge/build_and_publish.sh \
  --drop-diag 1 \
  --notes "Investigate long-running PlayStation packet loss"
```

Pass `--version 0.1.0-dev.4` to choose the version explicitly. Versions are
limited to 15 ASCII characters so they fit the Switch NACP display-version
field. The generated `build/switch/build-manifest.json` records the version,
Git revision, diagnostic switches, curl provider, size, SHA-256, and publish
time for the current local artifact. It is build output and is intentionally
not committed. Cloudflare's `/dev/versions.json` remains the durable release
history, avoiding a second manually maintained history file.

Each release stores both the original `.nro` and a gzip transport copy. Existing
clients and Feishu links keep using the original artifact. New clients prefer
`compressed_download_url`, let libcurl decode gzip while streaming, and verify
the decompressed NRO against the original size and SHA-256 before replacement.

A successful Cloudflare publish sends the Feishu notification by default. Pass
`--no-feishu` only when a publish intentionally should not notify the configured
release chat.

## Publish a version

```sh
LUNARNX_DEV_BRIDGE_URL=https://lunarnx.tooyang.qzz.io \
  tools/dev_bridge/publish_build.sh \
  build/switch/LunarNX.nro \
  0.1.0-dev.20260814.2 \
  "Describe the changes in this build"
```

The version must contain only letters, digits, `.`, `_`, or `-`. Publishing a
new version prepends it to the index without removing previous versions.
Publishing the same version again replaces that version's metadata entry.
Binary objects are addressed by SHA-256, so identical builds share one KV value.

Workers KV limits each value to 25 MiB. The publish script rejects larger NROs
before uploading them.

After all Cloudflare KV writes succeed, the publish command sends a Feishu
notification by default:

```sh
LUNARNX_DEV_BRIDGE_URL=https://lunarnx.tooyang.qzz.io \
  tools/dev_bridge/publish_build.sh \
  build/switch/LunarNX.nro \
  0.1.0-dev.20260814.2 \
  "Describe the changes in this build"
```

Pass `--no-feishu`, or set `LUNARNX_FEISHU_NOTIFY=0`, to suppress the
notification explicitly. The notification is sent
as the configured bot to the chat stored in the macOS Keychain item
`feishu-chat-id`, using the authenticated `lark-cli` profile. It includes the
version, notes, Git revision, size, and immutable NRO download link; no Feishu
credentials or chat IDs are stored in the repository. A missing configuration
or send failure is reported as a warning after the Cloudflare publish succeeds,
so retrying the command cannot accidentally be mistaken for a required deploy
retry. Re-publishing an identical NRO within Feishu's idempotency window does
not create a duplicate notification.

## Prune old versions

Cleanup is a dry run by default:

```sh
tools/dev_bridge/prune_builds.sh --keep 10
```

Review the listed versions and orphaned NRO count, then apply explicitly:

```sh
tools/dev_bridge/prune_builds.sh --keep 10 --apply
```

The script keeps the newest `N` entries, updates `latest.json` and the public
index, removes manifests for older versions, and deletes only NRO objects that
are no longer referenced by a retained version. It never runs automatically.

## Public build API

- `GET /dev/latest.json` returns the newest version manifest.
- `GET /dev/versions.json` returns all published version manifests, newest first.
- `GET /dev/versions/<version>` returns one version manifest.
- `GET /dev/builds/<sha256>.nro` downloads an immutable build.

Each manifest contains `version`, `notes`, `build_id`, `git_commit`,
`published_at`, `size`, `sha256`, and `download_url`.

## Logs

- `POST /dev/logs` accepts a log with `Authorization: Bearer <device token>`.
- `GET /admin/logs/latest` downloads the newest log with the admin token.

Log uploads include the exact LunarNX version, Git commit, and a random local
device ID. The device ID distinguishes consoles without exposing a hardware
serial number. A successful upload returns the stored `log_id` to LunarNX.

On the development Mac, both tokens are stored in Keychain under
`lunarnx-dev-bridge-device-token` and `lunarnx-dev-bridge-admin-token`.
The unified build command reads the device token automatically and fails before
building if it is missing, so a published development NRO always has working log
upload credentials.
