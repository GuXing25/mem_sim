#!/usr/bin/env bash
set -euo pipefail

sweep_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
sweep_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
sweep_tmp=$(mktemp -d)
trap 'rm -rf -- "$sweep_tmp"' EXIT

python3 "$sweep_source/experiments/architecture_sweep/run.py" \
  --binary "$sweep_bin" --standards hbm4,lpddr6 --requests 64 --inject-interval 2 \
  --out "$sweep_tmp" >/dev/null

test -s "$sweep_tmp/results.csv"
test -s "$sweep_tmp/checks.csv"
test -s "$sweep_tmp/summary.md"
test -s "$sweep_tmp/trends.html"
test "$(wc -l < "$sweep_tmp/results.csv")" -eq 17
grep -q 'bank,bg2_bank4' "$sweep_tmp/results.csv"
grep -q 'geometry,row131072_col2048' "$sweep_tmp/results.csv"
grep -q 'refresh,all_bank' "$sweep_tmp/results.csv"
grep -q 'PASS,bank scaling: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,geometry trend: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,bank scaling: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,geometry trend: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: LPDDR6' "$sweep_tmp/checks.csv"
if grep -q '^FAIL,' "$sweep_tmp/checks.csv"; then
  echo "architecture sweep contains a failed automated check" >&2
  exit 1
fi
test -s "$sweep_tmp/hbm4/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/hbm4/geometry_row32768_col512/workload.trace"
test -s "$sweep_tmp/lpddr6/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/lpddr6/geometry_row32768_col512/workload.trace"

echo "architecture sweep smoke passed"
