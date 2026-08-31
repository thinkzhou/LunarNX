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
   host regressions and full Switch build, and attaches these assets:
   `LunarNX-MAJOR.MINOR.PATCH.nro`, `LunarNX-MAJOR.MINOR.PATCH.zip`, and
   `SHA256SUMS`.

The ZIP is the preferred distribution because it includes LunarNX's license,
the third-party notices, and the AGPL-3.0 license required by the combined
Xbox/PlayStation binary.

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
