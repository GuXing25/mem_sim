#!/usr/bin/env bash
set -euo pipefail

phy_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
phy_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
phy_tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$phy_tmp_dir"' EXIT

value_of() {
  local key=$1
  awk -F: -v wanted="$key" '$1 ~ "^" wanted "[ ]*$" {gsub(/[[:space:]]/, "", $2); print $2; exit}'
}

for standard in hbm3 hbm4 lpddr5 lpddr6; do
  output=$(
    "$phy_bin" \
      --config "$phy_source/configs/run/${standard}.cfg" \
      --requests 24 \
      --read-ratio 50 \
      --max-cycles 500000 \
      --dfi-trace "$phy_tmp_dir/${standard}.csv" \
      --validate-dfi-trace
  )
  [[ $(value_of mem_phy_mode <<<"$output") == behavioral ]]
  [[ $(value_of dfi_validation <<<"$output") == pass ]]
  [[ $(value_of data_mismatches <<<"$output") == 0 ]]
  [[ $(value_of remaining_requests <<<"$output") == 0 ]]
  [[ $(value_of remaining_pending <<<"$output") == 0 ]]
  [[ $(value_of hit_cycle_limit <<<"$output") == false ]]
  [[ $(value_of phy_read_requests <<<"$output") == $(value_of phy_read_completions <<<"$output") ]]
  [[ $(value_of phy_write_requests <<<"$output") == $(value_of phy_write_completions <<<"$output") ]]
done

multi_output=$(
  "$phy_bin" \
    --config "$phy_source/configs/run/hbm4_6stack.cfg" \
    --requests 24 \
    --read-ratio 50 \
    --max-cycles 500000
)
[[ $(value_of mem_phy_mode <<<"$multi_output") == behavioral ]]
[[ $(value_of controller_count <<<"$multi_output") == 192 ]]
[[ $(value_of active_stacks <<<"$multi_output") == 6 ]]
[[ $(value_of remaining_requests <<<"$multi_output") == 0 ]]
[[ $(value_of remaining_pending <<<"$multi_output") == 0 ]]
[[ $(value_of data_mismatches <<<"$multi_output") == 0 ]]

echo "phy smoke passed"
