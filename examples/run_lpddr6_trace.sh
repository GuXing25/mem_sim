#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
# LPDDR6 trace 示例；README 中的 `--standard lpddr` 也会映射到该 preset。
./build/hbm_sim --standard lpddr6 --trace examples/sample.trace --requests 0
