#!/usr/bin/env python3
"""Compare only the shared HBM4 command surface with Ramulator2.1 as a reference."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, asdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

SCENARIOS = {
    "closed_read": [("Read", 0, 0)],
    "row_hit": [("Read", 0, 0), ("Read", 0, 1)],
    "row_conflict": [("Read", 0, 0), ("Read", 1, 0)],
    "write_forwarded_read": [("Write", 0, 0), ("Read", 0, 0)],
}

RAMULATOR_HELPER = r"""
import json
import sys

import ramulator
import tests.controller_scheduling.harness as cs

scenarios = json.load(sys.stdin)
result = {"scenarios": {}}
timings = None
levels = None
for name, requests in scenarios.items():
    dram = ramulator.dram.HBM4(
        org_preset="HBM4_32Gb_8Hi",
        timing_preset="HBM4_8000Mbps",
    )
    dut = cs.ControllerUnderTest.make_hbm34(dram)
    levels = dut.level_names
    timings = dut.timings
    for request_type, row, column in requests:
        address = dut.addr_vec(
            Channel=0,
            PseudoChannel=0,
            Sid=0,
            BankGroup=0,
            Bank=0,
            Row=row,
            Column=column,
        )
        dut.send_request(request_type, address)
    history = dut.run_until_idle(max_ticks=2048)
    result["scenarios"][name] = [
        {
            "cycle": item.clk,
            "command": item.command,
            "decoded": dict(zip(levels, item.addr_vec)),
        }
        for item in history
    ]
result["timings"] = timings
result["level_names"] = levels
json.dump(result, sys.stdout)
"""


@dataclass
class Check:
    name: str
    passed: bool
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/hbm_sim")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs/validation/hbm4_reference_1ch.cfg",
    )
    parser.add_argument(
        "--ramulator-root",
        type=Path,
        default=Path("/path/to/ramulator2"),
    )
    parser.add_argument(
        "--identity-manifest",
        type=Path,
        default=ROOT / "configs/validation/project_identity.csv",
    )
    parser.add_argument("--cycle-tolerance", type=int, default=2)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def run_ramulator(root: Path) -> dict:
    env = os.environ.copy()
    python_paths = [str(root / "python"), str(root / ".python-deps")]
    if env.get("PYTHONPATH"):
        python_paths.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(python_paths)
    library_paths = [str(root)]
    if env.get("LD_LIBRARY_PATH"):
        library_paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = os.pathsep.join(library_paths)
    completed = subprocess.run(
        [sys.executable, "-c", RAMULATOR_HELPER],
        cwd=root,
        env=env,
        input=json.dumps(SCENARIOS),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"Ramulator2.1 harness failed:\n{completed.stderr}")
    return json.loads(completed.stdout)


def address_for(row: int, column: int) -> int:
    line_size = 32
    columns = 32
    banks = 8
    bank_groups = 2
    pseudo_channels = 2
    sids = 2
    row_stride_lines = columns * banks * bank_groups * pseudo_channels * sids
    return (row * row_stride_lines + column) * line_size


def run_hbm_scenario(
    binary: Path,
    config: Path,
    name: str,
    requests: list[tuple[str, int, int]],
    temp: Path,
) -> list[dict]:
    trace = temp / f"{name}.trace"
    command_csv = temp / f"{name}.commands.csv"
    lines = []
    for request_type, row, column in requests:
        token = "R" if request_type == "Read" else "W"
        lines.append(f"0 {token} 0x{address_for(row, column):x}")
    trace.write_text("\n".join(lines) + "\n", encoding="ascii")
    command = [
        str(binary),
        "--config",
        str(config),
        "--trace",
        str(trace),
        "--cmd-trace",
        str(command_csv),
        "--validate-cmd-trace",
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"hbm_sim scenario {name} failed:\n{completed.stdout}{completed.stderr}"
        )
    with command_csv.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    return [
        {
            "cycle": int(row["cycle"]),
            "command": row["command"],
            "decoded": {
                "Channel": int(row["channel"]),
                "PseudoChannel": int(row["pseudo_channel"]),
                "Sid": int(row["sid"]),
                "Rank": int(row["rank"]),
                "BankGroup": int(row["bank_group"]),
                "Bank": int(row["bank"]),
                "Row": int(row["row"]),
                "Column": int(row["column"]),
            },
        }
        for row in rows
    ]


def read_hbm_timings(binary: Path, config: Path, temp: Path) -> tuple[dict[str, int], int]:
    output = temp / "hbm_timing.csv"
    completed = subprocess.run(
        [
            str(binary),
            "--config",
            str(config),
            "--requests",
            "0",
            "--dump-timing-table",
            str(output),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"hbm_sim timing dump failed:\n{completed.stderr}")
    tick_multiplier = 1
    for line in completed.stdout.splitlines():
        if line.startswith("tick_multiplier") and ":" in line:
            tick_multiplier = int(line.split(":", 1)[1].strip())
    with output.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    timings = {
        row["name"]: int(row["value_nck"]) * tick_multiplier
        for row in rows
    }
    return timings, tick_multiplier


def add(checks: list[Check], name: str, passed: bool, detail: str) -> None:
    checks.append(Check(name=name, passed=bool(passed), detail=detail))


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    config = args.config.resolve()
    ramulator_root = args.ramulator_root.resolve()
    identity_manifest = args.identity_manifest.resolve()
    if not binary.is_file():
        raise SystemExit(f"hbm_sim binary not found: {binary}")
    if not (ramulator_root / "libramulator.so").is_file():
        raise SystemExit(f"Ramulator2.1 build not found: {ramulator_root}")
    if not identity_manifest.is_file():
        raise SystemExit(f"project identity manifest not found: {identity_manifest}")

    with identity_manifest.open(newline="", encoding="utf-8") as stream:
        project_capabilities = list(csv.DictReader(stream))

    ramulator = run_ramulator(ramulator_root)
    checks: list[Check] = []
    scenario_results: dict[str, object] = {}
    with tempfile.TemporaryDirectory(prefix="hbm_ramulator_diff_") as temp_name:
        temp = Path(temp_name)
        hbm_timings, tick_multiplier = read_hbm_timings(binary, config, temp)
        timing_names = [
            "nBL",
            "nCL",
            "nCWL",
            "nRCDRD",
            "nRCDWR",
            "nRP",
            "nRAS",
            "nRC",
            "nRTP",
            "nWR",
            "nCCDS",
            "nCCDL",
            "nCCDR",
            "nRRDS",
            "nRRDL",
            "nFAW",
            "nRTW",
            "nWTRS",
            "nWTRL",
            "nRFC",
            "nRFCpb",
            "nRREFD",
            "nREFI",
            "nREFIpb",
        ]
        timing_diffs = {
            name: {
                "hbm_sim_ticks": hbm_timings.get(name),
                "ramulator_ticks": ramulator["timings"].get(name),
            }
            for name in timing_names
            if hbm_timings.get(name) != ramulator["timings"].get(name)
        }
        add(
            checks,
            "resolved_timing_table",
            not timing_diffs,
            f"compared={len(timing_names)} differences={timing_diffs}",
        )

        coordinate_names = [
            "Channel",
            "PseudoChannel",
            "Sid",
            "BankGroup",
            "Bank",
            "Row",
            "Column",
        ]
        for name, requests in SCENARIOS.items():
            hbm_events = run_hbm_scenario(binary, config, name, requests, temp)
            ram_events = ramulator["scenarios"][name]
            hbm_commands = [event["command"] for event in hbm_events]
            ram_commands = [event["command"] for event in ram_events]
            sequence_ok = hbm_commands == ram_commands
            add(
                checks,
                f"{name}_commands",
                sequence_ok,
                f"hbm_sim={hbm_commands} ramulator={ram_commands}",
            )

            coords_ok = len(hbm_events) == len(ram_events)
            coordinate_diffs = []
            cycle_deltas = []
            if coords_ok:
                for index, (hbm_event, ram_event) in enumerate(
                    zip(hbm_events, ram_events)
                ):
                    for coordinate in coordinate_names:
                        hbm_value = hbm_event["decoded"].get(coordinate)
                        ram_value = ram_event["decoded"].get(coordinate)
                        if hbm_value != ram_value:
                            coordinate_diffs.append(
                                [index, coordinate, hbm_value, ram_value]
                            )
                    cycle_deltas.append(hbm_event["cycle"] - ram_event["cycle"])
            coords_ok = coords_ok and not coordinate_diffs
            add(
                checks,
                f"{name}_coordinates",
                coords_ok,
                f"differences={coordinate_diffs}",
            )

            cycles_ok = sequence_ok and len(cycle_deltas) == len(hbm_events)
            cycles_ok = cycles_ok and all(
                abs(delta) <= args.cycle_tolerance for delta in cycle_deltas
            )
            add(
                checks,
                f"{name}_cycles",
                cycles_ok,
                f"deltas(hbm-ramulator)={cycle_deltas} "
                f"tolerance={args.cycle_tolerance}",
            )
            scenario_results[name] = {
                "hbm_sim": hbm_events,
                "ramulator2": ram_events,
                "cycle_deltas": cycle_deltas,
            }

    for check in checks:
        status = "PASS" if check.passed else "FAIL"
        print(f"{status:4} {check.name}: {check.detail}")

    report = {
        "schema_version": 1,
        "scope": "external_reference_overlap_HBM4_single_channel_commands",
        "relationship": "non_normative_external_reference",
        "project_model_authority": (
            "hbm_sim requirements, JEDEC-oriented profiles, and project-native validation"
        ),
        "config": str(config),
        "ramulator_root": str(ramulator_root),
        "cycle_tolerance_ticks": args.cycle_tolerance,
        "tick_multiplier": tick_multiplier,
        "checks": [asdict(check) for check in checks],
        "scenarios": scenario_results,
        "project_specific_capabilities": project_capabilities,
        "passed": all(check.passed for check in checks),
        "claim_boundary": (
            "Checks only the intentionally shared command/timing/address surface. "
            "Ramulator2.1 is not the implementation authority, and matching is not "
            "required for hbm_sim-specific storage, DFI, stack, power, thermal, or "
            "provenance semantics."
        ),
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    failed = [check for check in checks if not check.passed]
    print(
        f"\nRamulator2.1 reference overlap: "
        f"{len(checks) - len(failed)}/{len(checks)} checks passed"
    )
    print(
        f"Project authority: hbm_sim; preserved project-specific capabilities: "
        f"{len(project_capabilities)}"
    )
    print(
        "Reference boundary: a mismatch is investigated, not automatically changed "
        "to copy Ramulator2.1."
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
