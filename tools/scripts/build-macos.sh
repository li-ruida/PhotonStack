#!/usr/bin/env bash
set -euo pipefail

swift build \
  --scratch-path build/swift \
  --product PhotonStackMac
