#!/usr/bin/env bash

# Convenience entry point. Switch-targeted code must be built in Docker; this
# wrapper intentionally performs no host compilation.
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$project_root/scripts/docker_build_full.sh"
