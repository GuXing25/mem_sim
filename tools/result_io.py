"""Structured simulator results; missing/invalid metrics are never silently zero.

Diagnostic text and structured JSON share field names. Simulation tools request
JSON explicitly, independently of the user's terminal view.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import subprocess
import tempfile


def parse_stats(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        if line.lstrip().startswith("#") or ":" not in line:
            continue
        key, value = (item.strip() for item in line.split(":", 1))
        if key in fields and fields[key] != value:
            raise ValueError(f"conflicting metric {key!r}")
        fields[key] = value
    return fields


def read_result(path: Path, *, require_completed: bool = False) -> dict[str, str]:
    text = path.read_text(encoding="utf-8")
    if not text.lstrip().startswith("{"):
        if "模型 / MODEL" in text or "# ===== MODEL =====" in text:
            raise ValueError("compact report is for humans; supply --stats-json result.json "
                             "or a --stats-view diagnostic text file")
        metrics = parse_stats(text)
    else:
        def reject_constant(value: str):
            raise ValueError(f"non-finite JSON value: {value}")

        report = json.loads(text, parse_constant=reject_constant)
        if report.get("run_status") not in {"completed", "truncated", "failed"}:
            raise ValueError(f"invalid run_status in {path}")
        raw = {}
        for section in ("model", "parameters", "metrics", "validation", "diagnostics"):
            fields = report.get(section, {} if section == "diagnostics" else None)
            if not isinstance(fields, dict):
                raise ValueError(f"missing {section} object in {path}")
            for key, value in fields.items():
                if key in raw or key == "run_status":
                    raise ValueError(f"duplicate result field: {key}")
                raw[key] = value
        for stack in report.get("stacks", []):
            for key, value in stack.items():
                if key != "stack":
                    raw[f"stack_{stack['stack']}_{key}"] = value
        # null means unavailable. Callers must check completion counts before use.
        raw = {key: value for key, value in raw.items() if value is not None}
        metrics = {}
        for key, value in raw.items():
            if not isinstance(value, (str, int, float, bool)):
                raise ValueError(f"invalid metric type: {key}")
            if isinstance(value, float) and not math.isfinite(value):
                raise ValueError(f"non-finite metric: {key}")
            metrics[key] = str(value).lower() if isinstance(value, bool) else str(value)
        status = report["run_status"]
        metrics["run_status"] = status
        if require_completed and status != "completed":
            raise ValueError(f"simulation {status}: {report.get('error', '')}")
    if require_completed:
        require_valid_run(metrics)
    return metrics


def number(metrics: dict[str, str], key: str) -> float:
    if key not in metrics:
        raise ValueError(f"required metric is missing: {key}")
    try:
        result = float(metrics[key])
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid numeric metric {key}: {metrics[key]!r}") from error
    if not math.isfinite(result):
        raise ValueError(f"non-finite metric {key}: {metrics[key]!r}")
    return result


def latency_ticks(metrics: dict[str, str], kind: str = "read") -> float:
    """Convert a measured transaction latency from ns to simulator ticks."""
    tick_ps = number(metrics, "tick_duration_ps")
    if tick_ps <= 0:
        raise ValueError("tick_duration_ps must be positive")
    return number(metrics, f"avg_{kind}_latency_ns") * 1000 / tick_ps


def count(metrics: dict[str, str], key: str) -> int:
    value = metrics.get(key, "")
    # Counters must be integers, not rounded floats or boolean strings.
    if not value.isdecimal():
        raise ValueError(f"missing/invalid non-negative integer metric {key}: {value!r}")
    return int(value)


def require_valid_run(metrics: dict[str, str]) -> None:
    if metrics.get("run_status", "completed") != "completed":
        raise ValueError("simulation did not complete")
    if metrics.get("hit_cycle_limit") != "false":
        raise ValueError("missing completion evidence or cycle limit reached")
    for key in ("remaining_requests", "remaining_pending", "data_mismatches"):
        if count(metrics, key) != 0:
            raise ValueError(f"invalid experiment: {key}={metrics[key]}")
    if count(metrics, "completed_reads") + count(metrics, "completed_writes") != count(metrics, "dram_transactions"):
        raise ValueError("completed transaction count differs from submitted DRAM transactions")
    count(metrics, "host_requests")
    count(metrics, "system_cycles")


def run_simulator(command: list[str], *, cwd: Path,
                  timeout: float | None = None,
                  diagnostic: bool = False) -> tuple[dict[str, str], str]:
    with tempfile.TemporaryDirectory(prefix="hbm_result_") as temp:
        result = Path(temp) / "result.json"
        completed = subprocess.run(
            [*command, "--stats-view", "diagnostic" if diagnostic else "summary",
             "--stats-json", str(result)],
            cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, check=False)
        if completed.returncode:
            raise RuntimeError(f"simulation exited {completed.returncode}: {' '.join(command)}\n"
                               f"{completed.stdout}{completed.stderr}")
        return read_result(result, require_completed=True), completed.stdout
