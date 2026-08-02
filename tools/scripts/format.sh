#!/usr/bin/env bash
set -euo pipefail

if ! command -v clang-format >/dev/null 2>&1; then
  if [[ -x /opt/homebrew/bin/clang-format ]]; then
    clang_format=/opt/homebrew/bin/clang-format
  elif [[ -x /opt/homebrew/opt/clang-format/bin/clang-format ]]; then
    clang_format=/opt/homebrew/opt/clang-format/bin/clang-format
  else
    echo "clang-format not found. Install with: brew install clang-format" >&2
    exit 1
  fi
else
  clang_format=clang-format
fi

"${clang_format}" -i \
  apps/PhotonStackCLI/main.cpp \
  engine/include/photonstack/*.hpp \
  engine/src/*.cpp \
  tests/engine/*.cpp
