# LunarNX Borealis Switch patch

`lunarnx-borealis-gpu-lifecycle.patch` applies to Borealis commit
`240223f372731949e04cba943c453dc45b69faa1`.

It serializes Borealis frame submission and LunarNX's external video renderer
on the shared Deko3D queue and image descriptor pool. Run
`scripts/setup_dependencies.sh` to apply it to `lib/borealis`, then rebuild
Borealis in the Switch Docker environment.
