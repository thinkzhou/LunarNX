# LunarNX development bridge

The bridge publishes versioned Nintendo Switch NRO builds and receives bounded
development logs through a Cloudflare Worker backed by Workers KV.

## Publish a version

```sh
LUNARNX_DEV_BRIDGE_URL=https://lunarnx-dev-bridge.zy741870701.workers.dev \
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
