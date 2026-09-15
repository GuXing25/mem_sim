#!/usr/bin/env bash
set -euo pipefail

sweep_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
sweep_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
sweep_tmp=$(mktemp -d)
trap 'rm -rf -- "$sweep_tmp"' EXIT

# --bank-requests 取小值保持 smoke 快；bank 组因此会走“不可分辨”分支，
# 真正的三点比较由 LPDDR6 覆盖（lane 少，64 请求即可分辨）。
python3 "$sweep_source/experiments/architecture_sweep/run.py" \
  --binary "$sweep_bin" --standards hbm4,lpddr6 --requests 64 \
  --bank-requests 64 --inject-interval 2 --out "$sweep_tmp" >/dev/null

test -s "$sweep_tmp/results.csv"
test -s "$sweep_tmp/checks.csv"
test -s "$sweep_tmp/summary.md"
test -s "$sweep_tmp/trends.html"
test "$(wc -l < "$sweep_tmp/results.csv")" -eq 17
grep -q 'bank,bpg_half' "$sweep_tmp/results.csv"
grep -q 'bank,bpg_standard' "$sweep_tmp/results.csv"
grep -q 'geometry,cols_high' "$sweep_tmp/results.csv"
grep -q 'refresh,all_bank' "$sweep_tmp/results.csv"
grep -q 'PASS,bank scaling: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,geometry trend: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,bank scaling: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,geometry trend: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: LPDDR6' "$sweep_tmp/checks.csv"
# 容量核算是验收标准，必须进回归保护：Python 按容量公式算出的组织容量
# 要等于引擎报告的 aggregate_capacity_bytes。
grep -q 'PASS,capacity: HBM4 ' "$sweep_tmp/checks.csv"
grep -q 'PASS,capacity: LPDDR6 ' "$sweep_tmp/checks.csv"
grep -q 'PASS,organization drift: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,organization drift: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,baseline stability: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,baseline stability: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,address encoding: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,address encoding: LPDDR6' "$sweep_tmp/checks.csv"
if grep -q '^FAIL,' "$sweep_tmp/checks.csv"; then
  echo "architecture sweep contains a failed automated check" >&2
  exit 1
fi
test -s "$sweep_tmp/hbm4/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/hbm4/geometry_cols_low/workload.trace"
test -s "$sweep_tmp/hbm4/baseline.cfg"
test -s "$sweep_tmp/lpddr6/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/lpddr6/geometry_cols_low/workload.trace"
test -s "$sweep_tmp/lpddr6/baseline.cfg"

# bank 组现在保留标准 channels/pseudo_channels，因此第二个请求落点是
# columns * 32 B：HBM3/HBM4 的 columns=32 -> 0x400，LPDDR5/LPDDR6 的
# columns=64 -> 0x800。这个按标准区分的结果本身就是“bank 组尊重标准列宽”
# 的断言。fixture 必须收敛 lane，否则请求 #1 会落到另一条 lane 而不是
# 另一个 bank。
python3 - "$sweep_source/experiments/architecture_sweep/run.py" "$sweep_tmp" <<'PY'
import pathlib
import runpy
import sys

module = runpy.run_path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
expected = {"hbm3": "0 R 0x400", "hbm4": "0 R 0x400",
            "lpddr5": "0 R 0x800", "lpddr6": "0 R 0x800"}
for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
    assert module["transaction_bytes"](standard) == 32
    # 与组织无关的事务粒度断言：column 前进一格就是一条 32 B 事务。
    assert module["default_address"](
        row=0, column=1, bank=0, bank_group=0,
        org=module["STANDARD_ORGS"][standard],
        tx_bytes=module["transaction_bytes"](standard)) == 32
    case = module["Case"]("bank", "unit", {
        "channels": 1, "pseudo_channels": 1, "sids": 1, "ranks": 1,
        "bank_groups": 2, "banks_per_group": 4})
    trace = root / f"{standard}_address_unit.trace"
    module["write_trace"](trace, standard, case, 2, 2)
    assert trace.read_text().splitlines()[1] == expected[standard], standard
PY

echo "architecture sweep smoke passed"
