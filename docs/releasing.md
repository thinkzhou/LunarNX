# Releasing LunarNX

LunarNX uses Release Please and Conventional Commits to prepare releases from
`main`. The tracked `version.txt` file is the single source of truth for the
current stable version.

## Normal release flow

1. Merge normally named commits or pull requests into `main`.
2. Release Please creates or updates one release pull request containing the
   next version and generated `CHANGELOG.md` entries.
3. Review and merge that release pull request.
4. The same workflow creates the `vMAJOR.MINOR.PATCH` GitHub Release, runs the
   host regressions and full Switch build, and attaches `LunarNX.nro`,
   `LunarNX.zip`, and `SHA256SUMS`. The release tag, not the asset filename,
   identifies the version.

The ZIP is the preferred distribution because it contains a stable top-level
`LunarNX/` directory with `LunarNX/LunarNX.nro`, LunarNX's license, the
third-party notices, and the AGPL-3.0 license required by the combined
Xbox/PlayStation binary.

The About page uses two coordinated version mechanisms. The version shown
beside the project identity is compiled from `version.txt` by `Makefile.switch`
as `LUNARNX_VERSION`. During every Switch build,
`scripts/generate_changelog_resource.py` converts the release headings and
summaries in `CHANGELOG.md` into `romfs:/changelog.json`. The About page reads
that resource and therefore lists future releases automatically. Translated
cards for 0.3.0, 0.2.0, and 0.1.0 remain as localized overrides; a future
release only needs its normal Release Please entry in `CHANGELOG.md` and does
not require a C++ edit. Add translations later when they are available.

For the Sphaira/ForTheUsers package, keep the installed binary path stable at
`/switch/LunarNX/LunarNX.nro`. Use the release ZIP as the single package asset
and map `/LunarNX/**/*` into `/switch/LunarNX`; including both the standalone
NRO and ZIP assets packages duplicate payloads. Changes to a pending store
submission should be made on that submission's PR before it is merged.

Release Please chooses the version from Conventional Commit prefixes:

- `fix:` creates a patch release.
- `feat:` creates a minor release.
- `BREAKING CHANGE:` or a `!` after the commit type creates a major release.
- Documentation and refactoring commits can appear in the changelog but do not
  force a version bump by themselves; maintenance-only commits are omitted.

Pull request and branch builds use `<stable>-g<commit>`, for example
`0.2.0-gabcdef0`, so test packages remain distinguishable from official
releases.

## Manual recovery

Pushing a strict `vMAJOR.MINOR.PATCH` tag runs the same tested Switch build and
creates or updates the corresponding GitHub Release, provided the tag matches
`version.txt`. To rebuild an existing release, manually run **Build Nintendo
Switch application** with that tag selected as the workflow ref. Normal
releases should use the release pull request so that `version.txt` and
`CHANGELOG.md` stay synchronized.

The repository must allow GitHub Actions to create pull requests. If that
setting is disabled, enable **Settings > Actions > General > Allow GitHub
Actions to create and approve pull requests** before the first automated
release pull request is prepared.
