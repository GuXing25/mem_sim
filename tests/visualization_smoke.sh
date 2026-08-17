#!/usr/bin/env bash
set -euo pipefail

vis_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
vis_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
vis_tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$vis_tmp_dir"' EXIT

"$vis_bin" \
  --config "$vis_source/configs/run/hbm4.cfg" \
  --requests 16 \
  --read-ratio 50 \
  --cmd-trace "$vis_tmp_dir/commands.csv" \
  --dfi-trace "$vis_tmp_dir/dfi.csv" \
  --dump-thermal-map "$vis_tmp_dir/thermal.txt" \
  > "$vis_tmp_dir/stats.txt"

python3 "$vis_source/tools/visualize.py" \
  --command-trace "$vis_tmp_dir/commands.csv" \
  --dfi-trace "$vis_tmp_dir/dfi.csv" \
  --stats "$vis_tmp_dir/stats.txt" \
  --thermal-map "$vis_tmp_dir/thermal.txt" \
  --max-events 5 \
  --out "$vis_tmp_dir/dashboard.html"

test -s "$vis_tmp_dir/dashboard.html"
grep -q "Offline validation dashboard" "$vis_tmp_dir/dashboard.html"
grep -q "Trace explorer" "$vis_tmp_dir/dashboard.html"
grep -q "Request swimlanes" "$vis_tmp_dir/dashboard.html"
grep -q '"commands"' "$vis_tmp_dir/dashboard.html"
grep -q '"sampled":true' "$vis_tmp_dir/dashboard.html"
grep -q 'f(r)' "$vis_tmp_dir/dashboard.html"

echo "visualization smoke passed"
