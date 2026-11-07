#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
real_user_dir="$(cd "$HOME" && pwd -P)"
test_root="$(mktemp -d /private/tmp/PhotonStackSwiftTests.XXXXXXXX)"
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/user/Desktop" "$test_root/tmp" "$test_root/modules"

# Workspace tests exercise the default Foundation Application Support location.
# Protect the real home even if a child process resolves a different location.
export CFFIXED_USER_HOME="$test_root/user"
export TMPDIR="$test_root/tmp"
export CLANG_MODULE_CACHE_PATH="$test_root/modules"
scratch="${PHOTONSTACK_TEST_SCRATCH_PATH:-$test_root/build}"
/usr/bin/sandbox-exec -D "REAL_USER_DIR=$real_user_dir" \
  -p '(version 1) (allow default) (deny file-write* (subpath (param "REAL_USER_DIR")))' \
  /usr/bin/swift test --package-path "$repo_root" --scratch-path "$scratch" \
    --disable-sandbox --no-parallel "$@"
