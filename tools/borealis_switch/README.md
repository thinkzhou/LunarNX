# LunarNX Borealis Switch patch

The patches in this directory apply to Borealis commit
`240223f372731949e04cba943c453dc45b69faa1`.

- `lunarnx-borealis-gpu-lifecycle.patch` serializes Borealis frame submission
  and LunarNX's external video renderer on the shared Deko3D queue and image
  descriptor pool.
- `lunarnx-borealis-command-buffer.patch` increases Borealis' dynamic Deko3D
  command memory from 128 KiB to 256 KiB. This protects complex UI frames and
  Activity replacement from command-buffer exhaustion.

Run `scripts/setup_dependencies.sh` to apply both patches to `lib/borealis`,
then rebuild Borealis in the Switch Docker environment.
