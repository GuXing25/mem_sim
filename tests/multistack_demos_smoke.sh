#!/usr/bin/env bash
set -euo pipefail

demo_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
demo_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
demo_tmp=$(mktemp -d)
trap 'rm -rf -- "$demo_tmp"' EXIT

for demo_standard in hbm3 hbm4 lpddr5 lpddr6; do
  HBM_SIM_BIN="$demo_bin" OUTPUT_ROOT="$demo_tmp" \
    bash "$demo_source/examples/multistack_demos/${demo_standard}_nstack.sh" \
    >"$demo_tmp/${demo_standard}.log"
  grep -Eq '^stack_count[[:space:]]*: 2$' "$demo_tmp/${demo_standard}.log"
  grep -Eq '^active_stacks[[:space:]]*: 2$' "$demo_tmp/${demo_standard}.log"
  grep -Eq '^data_mismatches[[:space:]]*: 0$' "$demo_tmp/${demo_standard}.log"
  test -s "$demo_tmp/${demo_standard}_2stack/resolved.cfg"
  test -s "$demo_tmp/${demo_standard}_2stack/dashboard.html"
  grep -F '"stack":1' "$demo_tmp/${demo_standard}_2stack/dashboard.html" >/dev/null
  grep -F 'id="thermalStack"' "$demo_tmp/${demo_standard}_2stack/dashboard.html" >/dev/null
done

echo "four multistack demos passed"
