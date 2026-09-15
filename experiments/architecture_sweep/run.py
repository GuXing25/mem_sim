#!/usr/bin/env python3
"""Run auditable, workload-directed HBM/LPDDR architecture sweeps."""

from __future__ import annotations

import argparse
import csv
import html
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from result_io import count, number, read_result


ORG_KEYS = (
    "channels", "pseudo_channels", "sids", "ranks", "bank_groups",
    "banks_per_group", "rows", "columns", "line_size", "dram_transaction_bytes",
)

# 只作为 smoke 的 fixture：smoke 的 heredoc 没有二进制可跑基线预扫。
# 正式扫描永远使用从 resolved config 解析出的基线，并用
# "builtin org fixture" 检查强制本表与解析结果逐字段一致。
STANDARD_ORGS = {
    "hbm3": {"channels": 16, "pseudo_channels": 2, "sids": 2, "ranks": 1,
             "bank_groups": 4, "banks_per_group": 4, "rows": 16384, "columns": 32,
             "line_size": 64, "dram_transaction_bytes": 32},
    "hbm4": {"channels": 32, "pseudo_channels": 2, "sids": 2, "ranks": 1,
             "bank_groups": 2, "banks_per_group": 8, "rows": 16384, "columns": 32,
             "line_size": 64, "dram_transaction_bytes": 32},
    "lpddr5": {"channels": 1, "pseudo_channels": 1, "sids": 1, "ranks": 1,
               "bank_groups": 4, "banks_per_group": 4, "rows": 65536, "columns": 64,
               "line_size": 64, "dram_transaction_bytes": 32},
    "lpddr6": {"channels": 1, "pseudo_channels": 2, "sids": 1, "ranks": 1,
               "bank_groups": 4, "banks_per_group": 4, "rows": 65536, "columns": 64,
               "line_size": 64, "dram_transaction_bytes": 32},
}

# geometry 组只改 columns。columns 是"每行能连续访问多少次"这个量本身，
# 因而是唯一能驱动行命中/冲突的几何维度；rows 只决定何时回绕，在顺序 trace
# 下不改变局部性（实测三个 rows 取值的行为指标逐位相同）。
# 三个取值由目标容量反推，属于实验设计而非派生量：
# HBM3 16/24/32 GiB、HBM4 24/32/64 GiB、LPDDR5 1/2/4 GiB、LPDDR6 2/4/8 GiB。
# HBM3 与 HBM4 的基线在三点中的位置不同（1.0/1.5/2.0 与 0.75/1.0/2.0），
# 没有统一的缩放规则，文档必须写明。HBM 侧 columns 偏离 CA[4:0] 对应的 32，
# 属于 research 型偏离，不是器件值。
GEOMETRY_COLUMNS = {
    "hbm3": {"cols_low": 32, "cols_mid": 48, "cols_high": 64},
    "hbm4": {"cols_low": 24, "cols_mid": 32, "cols_high": 64},
    "lpddr5": {"cols_low": 32, "cols_mid": 64, "cols_high": 128},
    "lpddr6": {"cols_low": 32, "cols_mid": 64, "cols_high": 128},
}

# 刷新组的研究型压力间隔。取值要能在较短窗口内触发维护，不是 JEDEC 物理间隔；
# nREFI/nREFIpb 还要乘 tick_multiplier 才是实际周期。
REFRESH_TIMINGS = {
    "nRFC": 16, "nRFCpb": 16,
    "nREFI": 32, "nREFIpb": 32,
    "nREFDB2ACT": 16, "nREFDB2REFDBS": 16, "nREFDB2REFDBL": 16,
}

# bank scaling 门禁允许的输出抖动；只做端点比较。
BANK_SCALING_TOLERANCE = 0.99

# probe 覆盖多少个 lane 来校验地址编码。
PROBE_LANES = 8

CSV_FIELDS = (
    "standard", "group", "case", "workload", "perturbation", "overrides", "requests",
    "channels", "pseudo_channels", "sids", "ranks", "bank_groups", "banks_per_group",
    "rows", "columns", "lanes", "banks_per_lane", "total_banks", "engaged_banks",
    "coverage", "aggregate_capacity_GiB", "capacity_formula_GiB",
    "capacity_ratio_vs_baseline", "channel_mapper", "avg_read_latency_ticks",
    "achieved_bw_GBps", "bw_per_engaged_bank_GBps", "row_hits", "row_misses",
    "row_conflicts", "row_hit_rate_pct", "row_conflict_rate_pct",
    "refresh_pb_batches", "refresh_ab_batches", "data_mismatches",
    "hit_cycle_limit", "cycles", "wall_seconds",
)


@dataclass(frozen=True)
class Case:
    group: str
    name: str
    overrides: dict[str, str | int]


@dataclass(frozen=True)
class Check:
    status: str
    name: str
    detail: str


@dataclass(frozen=True)
class Observation:
    standard: str
    group: str
    case: str
    row: dict[str, object]
    org: dict[str, int]
    baseline: dict[str, int]
    planned_capacity: int
    reported_capacity: int
    density_gb: float
    capacity_per_instance: int
    stack_height: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-clang-debug/hbm_sim")
    parser.add_argument("--standards", default="hbm4,lpddr6",
                        help="comma-separated subset of hbm3,hbm4,lpddr5,lpddr6")
    parser.add_argument("--requests", type=int, default=512,
                        help="requests generated for the geometry and refresh workloads")
    parser.add_argument("--bank-requests", type=int, default=0,
                        help="requests for the bank workload; 0 derives the standard bank count")
    parser.add_argument("--inject-interval", type=int, default=2,
                        help="arrival spacing used by the refresh workload")
    parser.add_argument("--case-timeout", type=float, default=120.0,
                        help="wall-clock timeout in seconds for one simulator case")
    parser.add_argument("--out", type=Path, default=ROOT / "outputs/experiments/architecture_sweep")
    return parser.parse_args()


def family(standard: str) -> str:
    if standard in {"hbm3", "hbm4"}:
        return "hbm"
    if standard in {"lpddr5", "lpddr6"}:
        return "lpddr"
    raise SystemExit(f"unsupported standard: {standard}")


def transaction_bytes(standard: str) -> int:
    # 两个权威 master 当前对四个标准都使用 32 B DRAM transaction；
    # frontend 的 64 B host line 会再拆成两个 transaction。这里必须使用
    # transaction 粒度编码 column/bank/row，不能误用 host line_size。
    values = {"hbm3": 32, "hbm4": 32, "lpddr5": 32, "lpddr6": 32}
    try:
        return values[standard]
    except KeyError as error:
        raise SystemExit(f"unsupported standard: {standard}") from error


def total_banks(org: dict[str, int]) -> int:
    return (org["channels"] * org["pseudo_channels"] * org["sids"] * org["ranks"] *
            org["bank_groups"] * org["banks_per_group"])


def lanes_of(org: dict[str, int]) -> int:
    return org["channels"] * org["pseudo_channels"] * org["sids"] * org["ranks"]


def capacity_of(org: dict[str, int]) -> int:
    # 容量 = Channel×PC×SID×Rank×BG×Bank×Row×事务Column×事务Byte；不含 stack_height。
    return total_banks(org) * org["rows"] * org["columns"] * org["dram_transaction_bytes"]


def parse_org(text: str) -> dict[str, int]:
    section = text.split("[architecture]", 1)[1].split("\n[", 1)[0]
    values = dict(re.findall(r"^(\w+)\s*=\s*(\S+)", section, re.M))
    org: dict[str, int] = {}
    for key in ORG_KEYS:
        if key not in values:
            raise SystemExit(f"resolved config is missing architecture.{key}")
        org[key] = int(values[key])
    return org


def baseline_org(args: argparse.Namespace, standard: str) -> dict[str, int]:
    # --check-config 在提前返回前写出 resolved config，实测约 7 ms，不跑仿真。
    path = args.out / standard / "baseline.cfg"
    path.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(args.binary), "--config", str(ROOT / f"configs/{family(standard)}.cfg"),
        "--standard", standard, "--check-config",
        "--dump-resolved-config", str(path),
    ]
    try:
        subprocess.run(command, cwd=ROOT, check=True, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        raise SystemExit(f"baseline pre-pass failed for {standard}: {output}") from error
    return parse_org(path.read_text(encoding="utf-8"))


def cases(standard: str, baseline: dict[str, int]) -> list[Case]:
    # 每个 case 只扰动一个维度：bank 组改 banks_per_group，geometry 组改 columns，
    # refresh 组改 refresh_policy。channels/pseudo_channels/sids/ranks 保持标准，
    # 因此 LPDDR6 的 bank_groups 始终是偶数，REFdb 能力约束一直成立。
    bpg = baseline["banks_per_group"]
    cols = GEOMETRY_COLUMNS[standard]
    return [
        Case("bank", "bpg_half", {"banks_per_group": max(1, bpg // 2)}),
        Case("bank", "bpg_standard", {}),
        Case("bank", "bpg_double", {"banks_per_group": bpg * 2}),
        Case("geometry", "cols_low", {"columns": cols["cols_low"]}),
        Case("geometry", "cols_mid", {"columns": cols["cols_mid"]}),
        Case("geometry", "cols_high", {"columns": cols["cols_high"]}),
        Case("refresh", "per_bank", {"refresh_policy": "per_bank"}),
        Case("refresh", "all_bank", {"refresh_policy": "all_bank"}),
    ]


def declared_org_fields(case: Case) -> set[str]:
    return {key for key in case.overrides if key in ORG_KEYS}


def effective_org(standard: str, case: Case,
                  baseline: dict[str, int] | None = None) -> dict[str, int]:
    org = dict(STANDARD_ORGS[standard] if baseline is None else baseline)
    for key in declared_org_fields(case):
        org[key] = int(case.overrides[key])
    return org


def common_overrides(standard: str, case: Case) -> dict[str, str | int]:
    # 只保留单个 stack。不再强制 single_controller/channels/pc/sids：
    # 标准组织本身就是基线；且 single_controller 与 channels>1 组合会绕过
    # MemorySystem::make_channel_spec 的 channel 本地化，在同一份报告容量下
    # 仿真的是另一台机器（引擎只给警告，不拦截）。
    values: dict[str, str | int] = {
        "model_name": f"architecture_sweep_{standard}_{case.name}",
        "stack_count": 1,
        "mem_phy_mode": "direct",
        "address_mapping": "default",
        # configs/hbm.cfg 默认 xor，会按 (line ^ line>>6 ^ line>>12) % channels
        # 覆盖解码出的 channel 字段。该哈希多对一、需在 row 上搜索才能反解，
        # 而 row 正是 geometry 组要测的变量，故显式改回 decoded。
        "channel_mapper": "decoded",
        "supports_refresh": "true" if case.group == "refresh" else "false",
        "max_cycles": 10_000_000,
    }
    values.update(case.overrides)
    if case.group == "refresh":
        values.update(REFRESH_TIMINGS)
        values["timing_override_source"] = "research"
    return values


def overlay_text(standard: str, case: Case) -> str:
    lines = ["[override]"]
    lines.extend(f"{key} = {value}" for key, value in common_overrides(standard, case).items())
    return "\n".join(lines) + "\n"


def default_address(*, row: int, column: int, bank: int, bank_group: int,
                    org: dict[str, int], tx_bytes: int,
                    pseudo_channel: int = 0, sid: int = 0, rank: int = 0,
                    channel: int = 0) -> int:
    # AddressMapper::Default 的低位顺序是 column, bank, BG, PC, SID, rank,
    # channel, row。byte address 先除以 dram_transaction_bytes 得到 line。
    line = column
    stride = org["columns"]
    line += stride * bank
    stride *= org["banks_per_group"]
    line += stride * bank_group
    stride *= org["bank_groups"]
    line += stride * pseudo_channel
    stride *= org["pseudo_channels"]
    line += stride * sid
    stride *= org["sids"]
    line += stride * rank
    stride *= org["ranks"]
    line += stride * channel
    stride *= org["channels"]
    return (line + stride * row) * tx_bytes


def decode_line(line: int, org: dict[str, int]) -> dict[str, int]:
    # default_address 的逆运算，供 probe 校验引擎解码坐标。
    decoded: dict[str, int] = {}
    for key in ("columns", "banks_per_group", "bank_groups", "pseudo_channels",
                "sids", "ranks", "channels"):
        decoded[key] = line % org[key]
        line //= org[key]
    # 键名与 ORG_KEYS 保持一致，probe 才能按同一张坐标表比对。
    decoded["rows"] = line % org["rows"]
    return decoded


def split_lane(lane: int, org: dict[str, int]) -> tuple[int, int, int, int]:
    # 与地址映射同序：低位是 pseudo_channel，再 SID、rank，最高是 channel。
    pseudo_channel = lane % org["pseudo_channels"]
    lane //= org["pseudo_channels"]
    sid = lane % org["sids"]
    lane //= org["sids"]
    rank = lane % org["ranks"]
    lane //= org["ranks"]
    channel = lane % org["channels"]
    return channel, rank, sid, pseudo_channel


def split_flat_bank(flat: int, org: dict[str, int]) -> tuple[int, int]:
    bank = flat % org["banks_per_group"]
    bank_group = (flat // org["banks_per_group"]) % org["bank_groups"]
    return bank, bank_group


def walk(index: int, org: dict[str, int]) -> dict[str, int]:
    # lane = index % lanes 与被扫的 banks_per_group 无关，保证三个 bank case
    # 呈现给机器的 lane 数和每 lane 请求数相同，唯一差别是 bank 复用。
    lane_count = lanes_of(org)
    lane = index % lane_count
    bank_slot = index // lane_count
    per_lane = org["bank_groups"] * org["banks_per_group"]
    bank, bank_group = split_flat_bank(bank_slot % per_lane, org)
    channel, rank, sid, pseudo_channel = split_lane(lane, org)
    return {"row": bank_slot // per_lane, "column": 0, "bank": bank,
            "bank_group": bank_group, "channel": channel, "rank": rank,
            "sid": sid, "pseudo_channel": pseudo_channel}


def requests_for(args: argparse.Namespace, case: Case, baseline: dict[str, int]) -> int:
    if case.group != "bank":
        return args.requests
    if args.bank_requests > 0:
        return args.bank_requests
    # 固定 512 请求时，HBM4 的 128 条 lane 每 lane 只有 4 个请求，bank 并行度
    # 超过 4 就完全不可分辨。bank 组按标准 bank 总数取请求数，使每 lane 的请求数
    # 等于标准 banks_per_lane，三点才有分辨力。
    return total_banks(baseline)


def write_trace(path: Path, standard: str, case: Case, requests: int,
                inject_interval: int, *, org: dict[str, int] | None = None) -> None:
    resolved = effective_org(standard, case, org)
    tx_bytes = resolved["dram_transaction_bytes"]
    columns = resolved["columns"]
    rows = resolved["rows"]
    lane_count = lanes_of(resolved)
    lines: list[str] = []
    for index in range(requests):
        if case.group == "bank":
            # 同时到达并轮转全部 (lane, bank)；同一 bank 每轮访问一个从未使用过的
            # row，避免 row wrap 产生的 FR-FCFS 命中把"可并行 bank 数"与
            # "调度器重排行命中"混在一起。
            fields = walk(index, resolved)
            arrival = 0
        elif case.group == "geometry":
            # 单 lane 顺序扫描，footprint 恰为 requests 条事务。每行能连续访问
            # columns 次才换行，因此行命中率随 columns 变化；旧的 max(4096, requests)
            # footprint 下界会让三点在小请求数下全部退化，已移除。
            fields = {"row": (index // columns) % rows, "column": index % columns,
                      "bank": 0, "bank_group": 0, "channel": 0, "rank": 0,
                      "sid": 0, "pseudo_channel": 0}
            arrival = 0
        else:
            # 持续负载让自动 REFpb/REFdb 与 REFab 都能介入调度。
            fields = walk(index, resolved)
            fields["row"] = (index // lane_count) % 4
            fields["column"] = 0
            arrival = index * inject_interval
        address = default_address(org=resolved, tx_bytes=tx_bytes, **fields)
        lines.append(f"{arrival} R 0x{address:x}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_simulator(args: argparse.Namespace, standard: str, case: Case,
                  command: list[str], case_dir: Path) -> None:
    try:
        completed = subprocess.run(
            command, cwd=ROOT, check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=args.case_timeout,
        )
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        (case_dir / "stats.txt").write_text(output, encoding="utf-8")
        raise SystemExit(
            f"case timed out after {args.case_timeout:g}s: {standard} {case.group}/{case.name}; "
            f"partial output: {case_dir / 'stats.txt'}"
        ) from error
    except subprocess.CalledProcessError as error:
        (case_dir / "stats.txt").write_text(error.stdout or "", encoding="utf-8")
        raise SystemExit(
            f"case failed: {standard} {case.group}/{case.name}; "
            f"output: {case_dir / 'stats.txt'}"
        ) from error
    (case_dir / "stats.txt").write_text(completed.stdout, encoding="utf-8")


def simulator_command(args: argparse.Namespace, standard: str, overlay: str,
                      trace_path: Path, case_dir: Path, extra: list[str]) -> list[str]:
    return [
        str(args.binary), "--config", str(ROOT / f"configs/{family(standard)}.cfg"),
        "--standard", standard, "--config", overlay,
        "--trace", str(trace_path), "--requests", "0",
        "--inject-interval", str(args.inject_interval),
        "--dump-resolved-config", str(case_dir / "resolved.cfg"),
        "--stats-view", "diagnostic", "--stats-json", str(case_dir / "result.json"),
    ] + extra


def run_case(args: argparse.Namespace, standard: str, case: Case,
             baseline: dict[str, int], ordinal: int, total: int) -> Observation:
    case_dir = args.out / standard / f"{case.group}_{case.name}"
    case_dir.mkdir(parents=True, exist_ok=True)
    requests = requests_for(args, case, baseline)
    trace_path = case_dir / "workload.trace"
    write_trace(trace_path, standard, case, requests, args.inject_interval, org=baseline)
    print(f"[{ordinal}/{total}] {standard.upper()} {case.group}/{case.name} "
          f"({requests} requests)", flush=True)
    started = time.monotonic()
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", encoding="utf-8") as overlay:
        overlay.write(overlay_text(standard, case))
        overlay.flush()
        run_simulator(args, standard, case,
                      simulator_command(args, standard, overlay.name, trace_path,
                                        case_dir, []), case_dir)
    elapsed = time.monotonic() - started

    stats = read_result(case_dir / "result.json", require_completed=True)
    observed_org = parse_org((case_dir / "resolved.cfg").read_text(encoding="utf-8"))
    resolved = effective_org(standard, case, baseline)

    hits = number(stats, "row_hits")
    misses = number(stats, "row_misses")
    conflicts = number(stats, "row_conflicts")
    decisions = hits + misses + conflicts
    reported_capacity = count(stats, "aggregate_capacity_bytes")
    planned_capacity = capacity_of(resolved)
    baseline_capacity = capacity_of(baseline)
    lane_count = lanes_of(observed_org)
    per_lane = observed_org["bank_groups"] * observed_org["banks_per_group"]
    banks = lane_count * per_lane
    engaged = min(requests, banks)
    bandwidth = number(stats, "achieved_bw_GBps")
    row = {
        "standard": standard.upper(), "group": case.group, "case": case.name,
        "workload": "bank_parallel" if case.group == "bank" else
                    "single_lane_footprint" if case.group == "geometry" else "refresh_pressure",
        "perturbation": ";".join(f"{k}={v}" for k, v in case.overrides.items()) or "none",
        "overrides": ";".join(f"{k}={v}" for k, v in common_overrides(standard, case).items()),
        "requests": requests,
        "channels": observed_org["channels"],
        "pseudo_channels": observed_org["pseudo_channels"],
        "sids": observed_org["sids"], "ranks": observed_org["ranks"],
        "bank_groups": observed_org["bank_groups"],
        "banks_per_group": observed_org["banks_per_group"],
        "rows": observed_org["rows"], "columns": observed_org["columns"],
        "lanes": lane_count, "banks_per_lane": per_lane, "total_banks": banks,
        "engaged_banks": engaged, "coverage": engaged / banks if banks else 0.0,
        "aggregate_capacity_GiB": reported_capacity / 2**30,
        "capacity_formula_GiB": planned_capacity / 2**30,
        "capacity_ratio_vs_baseline": planned_capacity / baseline_capacity,
        "channel_mapper": stats.get("channel_mapper", ""),
        "avg_read_latency_ticks": number(stats, "avg_read_latency"),
        "achieved_bw_GBps": bandwidth,
        "bw_per_engaged_bank_GBps": bandwidth / engaged if engaged else 0.0,
        "row_hits": int(hits), "row_misses": int(misses), "row_conflicts": int(conflicts),
        "row_hit_rate_pct": 100.0 * hits / decisions if decisions else 0.0,
        "row_conflict_rate_pct": 100.0 * conflicts / decisions if decisions else 0.0,
        "refresh_pb_batches": int(number(stats, "refresh_pb_batches")),
        "refresh_ab_batches": int(number(stats, "refresh_ab_batches")),
        "data_mismatches": int(number(stats, "data_mismatches")),
        "hit_cycle_limit": stats["hit_cycle_limit"].lower(),
        "cycles": int(number(stats, "cycles")),
        "wall_seconds": round(elapsed, 3),
    }
    return Observation(
        standard=standard.upper(), group=case.group, case=case.name, row=row,
        org=observed_org, baseline=baseline, planned_capacity=planned_capacity,
        reported_capacity=reported_capacity, density_gb=number(stats, "density_gb"),
        capacity_per_instance=count(stats, "capacity_per_instance_bytes"),
        stack_height=count(stats, "stack_height"))


def probe_address_encoding(args: argparse.Namespace, standard: str,
                           baseline: dict[str, int]) -> Check:
    # 用 --cmd-trace 导出的显式 DRAM 坐标校验 Python 侧地址编码与引擎解码一致。
    # 注意不能用 storage_*_touched：那些计数在 ch=1 与 ch=32 下实测都是 0，
    # 拿它做断言是空检查。
    label = f"address encoding: {standard.upper()}"
    case_dir = args.out / standard / "probe"
    case_dir.mkdir(parents=True, exist_ok=True)
    resolved = dict(baseline)
    lane_count = lanes_of(resolved)
    probe_count = min(lane_count, PROBE_LANES)
    step = max(1, lane_count // probe_count)
    tx_bytes = resolved["dram_transaction_bytes"]
    lines: list[str] = []
    for index in range(probe_count):
        lane = (index * step) % lane_count
        channel, rank, sid, pseudo_channel = split_lane(lane, resolved)
        # 每条约请求用不同 row，使 row 字段也进入校验。
        address = default_address(row=index + 1, column=0, bank=0, bank_group=0,
                                  pseudo_channel=pseudo_channel, sid=sid, rank=rank,
                                  channel=channel, org=resolved, tx_bytes=tx_bytes)
        lines.append(f"0 R 0x{address:x}")
    trace_path = case_dir / "workload.trace"
    trace_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    csv_path = case_dir / "cmd_trace.csv"
    case = Case("bank", "probe", {})
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", encoding="utf-8") as overlay:
        overlay.write(overlay_text(standard, case))
        overlay.flush()
        run_simulator(args, standard, case,
                      simulator_command(args, standard, overlay.name, trace_path,
                                        case_dir, ["--cmd-trace", str(csv_path)]),
                      case_dir)
    if not csv_path.is_file():
        return Check("FAIL", label, "engine did not write a command trace")
    mismatches: list[str] = []
    seen = 0
    coordinate_keys = (("column", "columns"), ("bank", "banks_per_group"),
                       ("bank_group", "bank_groups"), ("pseudo_channel", "pseudo_channels"),
                       ("sid", "sids"), ("rank", "ranks"),
                       ("channel", "channels"), ("row", "rows"))
    with csv_path.open(newline="", encoding="utf-8") as stream:
        for entry in csv.DictReader(stream):
            address = int(entry["address"], 16)
            expected = decode_line(address // tx_bytes, resolved)
            seen += 1
            for csv_key, org_key in coordinate_keys:
                if int(entry[csv_key]) != expected[org_key]:
                    mismatches.append(f"addr=0x{address:x} {csv_key}={entry[csv_key]}"
                                      f" expected {expected[org_key]}")
            if len(mismatches) >= 3:
                break
    if not seen:
        return Check("FAIL", label, "command trace is empty")
    if mismatches:
        return Check("FAIL", label, "; ".join(mismatches[:3]))
    return Check("PASS", label,
                 f"{seen} issued commands decode consistently with default_address(); "
                 f"covered {probe_count} of {lane_count} lanes")


def engagement_of(observation: Observation) -> int:
    # 每 lane 实际可用的 bank 并行度：banks_per_lane 超过每 lane 请求数时
    # 多出来的 bank 在该请求数下根本到不了，不能支撑 scaling 结论。
    requests_per_lane = int(observation.row["requests"]) // max(1, int(observation.row["lanes"]))
    return min(int(observation.row["banks_per_lane"]), requests_per_lane)


def evaluate(observations: list[Observation]) -> list[Check]:
    checks: list[Check] = []
    for observation in observations:
        row = observation.row
        label = f"{observation.standard} {observation.group}/{observation.case}"
        valid = (int(row["cycles"]) > 0 and row["hit_cycle_limit"] == "false" and
                 int(row["data_mismatches"]) == 0)
        checks.append(Check("PASS" if valid else "FAIL", f"completion: {label}",
                            f"cycles={row['cycles']}, cycle_limit={row['hit_cycle_limit']}, "
                            f"mismatches={row['data_mismatches']}"))
        # 验收标准：Python 按容量公式算出的组织容量必须等于引擎报告的容量。
        # 两者都是整数，不需要浮点容差。
        planned = observation.planned_capacity
        reported = observation.reported_capacity
        checks.append(Check("PASS" if planned == reported else "FAIL",
                            f"capacity: {label}",
                            f"formula={planned} ({planned / 2**30:.3f} GiB) "
                            f"reported={reported} ({reported / 2**30:.3f} GiB) "
                            f"ratio_vs_baseline="
                            f"{planned / capacity_of(observation.baseline):.3f}"))

    for standard in sorted({o.standard for o in observations}):
        subset = [o for o in observations if o.standard == standard]
        baseline = subset[0].baseline

        fixture = STANDARD_ORGS[standard.lower()]
        differences = [f"{k}: fixture={fixture[k]} parsed={baseline[k]}"
                       for k in ORG_KEYS if fixture[k] != baseline[k]]
        checks.append(Check("PASS" if not differences else "FAIL",
                            f"builtin org fixture: {standard}",
                            "; ".join(differences) or
                            "smoke fixture matches the parsed standard organization"))

        # “单维度扰动”的可执行断言：每个 case 的生效组织必须只在其声明的字段上
        # 偏离基线。直接比对 resolved.cfg 的解析结果，不依赖 result.json 的
        # changes 口径（后者是相对内置 profile 的 diff）。
        drift: list[str] = []
        for observation in subset:
            declared = {item.split("=")[0] for item in
                        observation.row["perturbation"].split(";") if "=" in item}
            for key in ORG_KEYS:
                if key in declared:
                    continue
                if observation.org[key] != baseline[key]:
                    drift.append(f"{observation.group}/{observation.case}: "
                                 f"{key} {baseline[key]}->{observation.org[key]}")
        checks.append(Check("PASS" if not drift else "FAIL",
                            f"organization drift: {standard}",
                            "; ".join(drift[:4]) or
                            "every case differs from the baseline only in its "
                            "declared dimension"))

        control = next(o for o in subset if o.case == "bpg_standard")
        off_baseline = [f"{k}: control={control.org[k]} baseline={baseline[k]}"
                        for k in ORG_KEYS if control.org[k] != baseline[k]]
        checks.append(Check("PASS" if not off_baseline else "FAIL",
                            f"baseline stability: {standard}",
                            "; ".join(off_baseline) or
                            "bpg_standard reproduces the parsed baseline field-for-field"))

        # density_gb 是 Gibit 口径（HBM 按每 die、LPDDR 按每通道子通道），
        # 不是容量；这里把它与容量口径的关系固定下来，防止被当容量读。
        # 用基线对照 case 报告，避免把某个扰动用例的密度当成标准密度。
        sample = next(o for o in subset if o.case == "bpg_standard")
        is_lpddr = standard.lower().startswith("lpddr")
        divisor = (sample.org["channels"] * sample.org["pseudo_channels"] * sample.org["ranks"]
                   if is_lpddr else sample.stack_height)
        basis = ("channels x subchannels x ranks" if is_lpddr else "stack_height")
        expected_density = (sample.capacity_per_instance / 134217728.0 / divisor
                            if divisor else 0.0)
        density_ok = divisor > 0 and abs(expected_density - sample.density_gb) <= 1e-9
        checks.append(Check("PASS" if density_ok else "FAIL",
                            f"density basis: {standard}",
                            f"density_gb={sample.density_gb:g} = "
                            f"capacity_per_instance_bytes x 8 / 2^30 / ({basis}={divisor}); "
                            f"Gibit per {'channel/subchannel' if is_lpddr else 'die'}, "
                            f"not a capacity - capacity is aggregate_capacity_bytes"))

        bank_rows = sorted((o for o in subset if o.group == "bank"),
                           key=lambda o: (engagement_of(o), int(o.row["total_banks"])))
        points = "; ".join(
            f"{o.row['total_banks']} banks, cov={float(o.row['coverage']):.2f}, "
            f"eng={engagement_of(o)}, bw={float(o.row['achieved_bw_GBps']):.3f}"
            for o in bank_rows)
        # 请求数固定时，bank 数超过请求数的配置有一部分 bank 根本到不了，
        # 其带宽不代表该组织规模，不能当作该规模的代表。优先只用覆盖到全部
        # bank 的配置；它们若凑不出两个 engagement 档，才退回全部用例。
        covered = [o for o in bank_rows if float(o.row["coverage"]) >= 1.0 - 1e-9]
        if len({engagement_of(o) for o in covered}) >= 2:
            scope, scope_label = covered, "coverage=1.00"
        else:
            scope, scope_label = bank_rows, "all cases"
        levels: dict[int, Observation] = {}
        for observation in scope:
            level = engagement_of(observation)
            best = levels.get(level)
            if best is None or (float(observation.row["achieved_bw_GBps"]) >
                                float(best.row["achieved_bw_GBps"])):
                levels[level] = observation
        if len(levels) < 2:
            only = next(iter(levels), None)
            checks.append(Check("PASS", f"bank scaling: {standard}",
                                f"not resolvable at {bank_rows[0].row['requests']} requests: "
                                f"all cases engage "
                                f"{engagement_of(only) if only else 0} banks per lane; {points}"))
        else:
            low_engagement, high_engagement = min(levels), max(levels)
            low, high = levels[low_engagement], levels[high_engagement]
            scaled = (float(high.row["achieved_bw_GBps"]) + 1e-9 >=
                      BANK_SCALING_TOLERANCE * float(low.row["achieved_bw_GBps"]))
            checks.append(Check("PASS" if scaled else "FAIL", f"bank scaling: {standard}",
                                f"{scope_label}: engagement {low_engagement} "
                                f"({low.row['total_banks']} banks, "
                                f"{float(low.row['achieved_bw_GBps']):.3f} GB/s) -> "
                                f"{high_engagement} ({high.row['total_banks']} banks, "
                                f"{float(high.row['achieved_bw_GBps']):.3f} GB/s); {points}"))

        geometry = sorted((o for o in subset if o.group == "geometry"),
                          key=lambda o: int(o.row["columns"]))
        geometry_ok = (float(geometry[-1].row["row_hit_rate_pct"]) + 1e-9 >=
                       float(geometry[0].row["row_hit_rate_pct"]) and
                       float(geometry[-1].row["row_conflict_rate_pct"]) <=
                       float(geometry[0].row["row_conflict_rate_pct"]) + 1e-9)
        checks.append(Check("PASS" if geometry_ok else "FAIL",
                            f"geometry trend: {standard}",
                            f"columns {geometry[0].row['columns']}->{geometry[-1].row['columns']}: "
                            f"hit {float(geometry[0].row['row_hit_rate_pct']):.2f}% -> "
                            f"{float(geometry[-1].row['row_hit_rate_pct']):.2f}%; conflict "
                            f"{float(geometry[0].row['row_conflict_rate_pct']):.2f}% -> "
                            f"{float(geometry[-1].row['row_conflict_rate_pct']):.2f}%"))

        by_case = {o.case: o.row for o in subset if o.group == "refresh"}
        per_bank, all_bank = by_case["per_bank"], by_case["all_bank"]
        refresh_ok = (int(per_bank["refresh_pb_batches"]) > 0 and
                      int(per_bank["refresh_ab_batches"]) == 0 and
                      int(all_bank["refresh_ab_batches"]) > 0 and
                      int(all_bank["refresh_pb_batches"]) == 0)
        checks.append(Check("PASS" if refresh_ok else "FAIL", f"refresh scope: {standard}",
                            f"per-bank(pb={per_bank['refresh_pb_batches']},"
                            f"ab={per_bank['refresh_ab_batches']}); "
                            f"all-bank(pb={all_bank['refresh_pb_batches']},"
                            f"ab={all_bank['refresh_ab_batches']})"))
    return checks


def write_csv(path: Path, observations: list[Observation]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(CSV_FIELDS))
        writer.writeheader()
        writer.writerows(observation.row for observation in observations)


def write_checks(path: Path, checks: list[Check]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["status", "check", "detail"])
        writer.writerows((check.status, check.name, check.detail) for check in checks)


def write_summary(path: Path, observations: list[Observation], checks: list[Check],
                  args: argparse.Namespace) -> None:
    lines = [
        "# Architecture sweep result", "",
        "每个 case 以该标准**自己的完整标准组织**为基线，只在一个维度上扰动：",
        "bank 组改 `banks_per_group`，geometry 组改 `columns`，refresh 组改 `refresh_policy`；",
        "`channels/pseudo_channels/sids/ranks` 始终保持标准值。",
        f"geometry/refresh 生成 {args.requests} 个请求（refresh 的 inject_interval="
        f"{args.inject_interval}）；bank 组按标准 bank 总数取请求数，使每 lane 有足够请求"
        "暴露 bank 并行度。",
        "`channel_mapper` 显式设为 `decoded`：`configs/hbm.cfg` 默认的 `xor` 会按地址哈希覆盖",
        "解码出的 channel，而该哈希多对一、需在 row 上搜索才能反解，会污染 geometry 变量。",
        "workload.trace 与 resolved.cfg 是审计依据。", "",
        "## Automated checks", "", "| status | check | detail |", "|---|---|---|",
    ]
    lines.extend(f"| {c.status} | {c.name} | {c.detail} |" for c in checks)
    lines.extend([
        "", "## Trend interpretation", "",
        "以下解释只比较同一标准、同一组内的定向 workload。bank 与 geometry 具有由 trace "
        "构造保证的预期方向；refresh 只对 REFpb/REFdb/REFab scope 做硬性门禁，"
        "不预设 per-bank 必然快于 all-bank。", "",
    ])
    for standard in sorted({o.standard for o in observations}):
        subset = [o for o in observations if o.standard == standard]
        base = subset[0].baseline
        banks = sorted((o for o in subset if o.group == "bank"),
                       key=lambda o: (engagement_of(o), int(o.row["total_banks"])))
        geometry = sorted((o for o in subset if o.group == "geometry"),
                          key=lambda o: int(o.row["columns"]))
        refresh = {o.case: o.row for o in subset if o.group == "refresh"}
        per_bank, all_bank = refresh["per_bank"], refresh["all_bank"]
        lines.extend([
            f"### {standard}", "",
            f"- 标准基线：{base['channels']}ch × {base['pseudo_channels']}pc × "
            f"{base['sids']}sid × {base['ranks']}rank × {base['bank_groups']}bg × "
            f"{base['banks_per_group']}bpg × {base['rows']}rows × {base['columns']}cols × "
            f"{base['dram_transaction_bytes']}B = {capacity_of(base) / 2**30:.3f} GiB。",
            f"- Bank：{banks[0].row['total_banks']}→{banks[-1].row['total_banks']} banks，"
            f"容量 {float(banks[0].row['capacity_ratio_vs_baseline']):.3f}→"
            f"{float(banks[-1].row['capacity_ratio_vs_baseline']):.3f}× 基线，带宽 "
            f"{float(banks[0].row['achieved_bw_GBps']):.3f}→"
            f"{float(banks[-1].row['achieved_bw_GBps']):.3f} GB/s。门禁按每 lane 实际可用的"
            " bank 并行度（engagement）归一；三点 engagement 相同时判定为在该请求数下"
            "不可分辨，而不是给出趋势。",
            f"- Geometry：columns {geometry[0].row['columns']}→{geometry[-1].row['columns']}，"
            f"容量 {float(geometry[0].row['capacity_ratio_vs_baseline']):.3f}→"
            f"{float(geometry[-1].row['capacity_ratio_vs_baseline']):.3f}× 基线，行命中率 "
            f"{float(geometry[0].row['row_hit_rate_pct']):.2f}%→"
            f"{float(geometry[-1].row['row_hit_rate_pct']):.2f}%，冲突率 "
            f"{float(geometry[0].row['row_conflict_rate_pct']):.2f}%→"
            f"{float(geometry[-1].row['row_conflict_rate_pct']):.2f}%。该组固定在单 lane 上"
            "顺序扫描，其带宽只是单 lane 值，不可跨标准或对峰值比较。",
            f"- Refresh：per-bank 触发 {per_bank['refresh_pb_batches']} 个 PB batch，"
            f"延迟/带宽 {float(per_bank['avg_read_latency_ticks']):.2f} tick / "
            f"{float(per_bank['achieved_bw_GBps']):.3f} GB/s；all-bank 触发 "
            f"{all_bank['refresh_ab_batches']} 个 AB batch，延迟/带宽 "
            f"{float(all_bank['avg_read_latency_ticks']):.2f} tick / "
            f"{float(all_bank['achieved_bw_GBps']):.3f} GB/s。间隔是 research 型压力值，"
            "取值以保证在短窗口内可观测，不是物理间隔。", "",
        ])
    lines.extend([
        "", "## Measurements", "",
        "| standard | group | case | perturbation | requests | rows | bpg | banks | "
        "engaged | cov | cap/GiB | ratio | BW/GBps | hit/% | conflict/% | REFpb | REFab | wall/s |",
        "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for observation in observations:
        row = observation.row
        lines.append(
            f"| {row['standard']} | {row['group']} | {row['case']} | `{row['perturbation']}` | "
            f"{row['requests']} | {row['rows']} | {row['banks_per_group']} | "
            f"{row['total_banks']} | {row['engaged_banks']} | {float(row['coverage']):.2f} | "
            f"{float(row['aggregate_capacity_GiB']):.3f} | "
            f"{float(row['capacity_ratio_vs_baseline']):.3f} | "
            f"{float(row['achieved_bw_GBps']):.3f} | {float(row['row_hit_rate_pct']):.2f} | "
            f"{float(row['row_conflict_rate_pct']):.2f} | {row['refresh_pb_batches']} | "
            f"{row['refresh_ab_batches']} | {float(row['wall_seconds']):.3f} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_html(path: Path, observations: list[Observation], checks: list[Check]) -> None:
    serialized_checks = "\n".join(
        f"<tr><td class='{c.status.lower()}'>{c.status}</td><td>{html.escape(c.name)}</td>"
        f"<td>{html.escape(c.detail)}</td></tr>" for c in checks
    )
    serialized_rows = "\n".join(
        f"<tr><td>{html.escape(str(o.row['standard']))}</td>"
        f"<td>{html.escape(str(o.row['group']))}</td>"
        f"<td>{html.escape(str(o.row['case']))}</td>"
        f"<td>{html.escape(str(o.row['perturbation']))}</td>"
        f"<td>{float(o.row['aggregate_capacity_GiB']):.3f}</td>"
        f"<td>{float(o.row['capacity_ratio_vs_baseline']):.3f}</td>"
        f"<td>{float(o.row['avg_read_latency_ticks']):.2f}</td>"
        f"<td>{float(o.row['achieved_bw_GBps']):.3f}</td>"
        f"<td>{float(o.row['row_hit_rate_pct']):.2f}</td>"
        f"<td>{float(o.row['row_conflict_rate_pct']):.2f}</td></tr>" for o in observations
    )
    document = f"""<!doctype html><meta charset=\"utf-8\"><title>Architecture sweep</title>
<style>body{{font:14px system-ui;background:#0b1020;color:#e6edf7;padding:28px}}table{{border-collapse:collapse;width:100%;margin-bottom:28px}}th,td{{border:1px solid #334155;padding:8px;text-align:right}}th:first-child,td:first-child,th:nth-child(2),td:nth-child(2),th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){{text-align:left}}tr:hover{{background:#17213b}}.pass{{color:#4ade80}}.fail{{color:#f87171}}</style>
<h1>HBM/LPDDR architecture sweep</h1><p>每个 case 是该标准完整组织的单维度扰动；容量由容量公式自动核算，不是器件标定值。</p>
<p>bank scaling 门禁按每 lane 可用 bank 并行度（engagement）归一，engagement 三点相同的标准会显式标注“不可分辨”。geometry 组固定在单 lane，其带宽不可跨标准比较。详细逐标准解释见同目录 summary.md。</p>
<h2>Automated checks</h2><table><thead><tr><th>status</th><th>check</th><th>detail</th></tr></thead><tbody>{serialized_checks}</tbody></table>
<h2>Measurements</h2><table><thead><tr><th>standard</th><th>group</th><th>case</th><th>perturbation</th><th>cap/GiB</th><th>ratio</th><th>latency/tick</th><th>BW/GBps</th><th>row hit/%</th><th>conflict/%</th></tr></thead><tbody>{serialized_rows}</tbody></table>"""
    path.write_text(document, encoding="utf-8")


def main() -> int:
    args = parse_args()
    if not args.binary.is_file():
        raise SystemExit(f"binary not found: {args.binary}")
    if (args.requests < 1 or args.inject_interval < 1 or args.case_timeout <= 0 or
            args.bank_requests < 0):
        raise SystemExit("--requests, --inject-interval and --case-timeout must be positive; "
                         "--bank-requests must be non-negative")
    standards = [item.strip().lower() for item in args.standards.split(",") if item.strip()]
    if not standards or len(standards) != len(set(standards)):
        raise SystemExit("--standards must contain a non-empty, unique list")
    for standard in standards:
        family(standard)
    args.out.mkdir(parents=True, exist_ok=True)

    baselines = {standard: baseline_org(args, standard) for standard in standards}
    all_cases = {standard: cases(standard, baselines[standard]) for standard in standards}
    total = sum(len(all_cases[standard]) for standard in standards)
    observations: list[Observation] = []
    probes: list[Check] = []
    ordinal = 0
    for standard in standards:
        for case in all_cases[standard]:
            ordinal += 1
            observations.append(run_case(args, standard, case, baselines[standard],
                                         ordinal, total))
        probes.append(probe_address_encoding(args, standard, baselines[standard]))
    checks = evaluate(observations) + probes
    write_csv(args.out / "results.csv", observations)
    write_checks(args.out / "checks.csv", checks)
    write_summary(args.out / "summary.md", observations, checks, args)
    write_html(args.out / "trends.html", observations, checks)
    failures = [check for check in checks if check.status == "FAIL"]
    if failures:
        for check in failures:
            print(f"FAIL: {check.name}: {check.detail}", file=sys.stderr)
        print(f"architecture sweep failed {len(failures)} check(s): {args.out / 'checks.csv'}")
        return 1
    print(f"architecture sweep complete: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
