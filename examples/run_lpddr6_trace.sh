#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
# LPDDR6 trace 示例；显式选择权威 master 的 LPDDR6 baseline。
./build/hbm_sim --config configs/lpddr.cfg --standard lpddr6 \
  --trace examples/sample.trace --requests 0
