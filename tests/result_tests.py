#!/usr/bin/env python3
"""Result contract: terminal view cannot change simulation/tool input data."""
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from result_io import count, number, read_result, require_valid_run, run_simulator

BINARY = Path(sys.argv.pop(1)).resolve()


class ResultContract(unittest.TestCase):
    def test_tool_helpers_do_not_collect_diagnostics_by_default(self):
        command = [str(BINARY), '--standard', 'hbm4', '--requests', '8']
        public, _ = run_simulator(command, cwd=ROOT, timeout=20)
        self.assertIn('row_hit_pct', public)
        self.assertNotIn('row_hits', public)
        diagnostic, _ = run_simulator(command, cwd=ROOT, timeout=20, diagnostic=True)
        self.assertIn('row_hits', diagnostic)
        for key in ('completed_reads', 'system_cycles', 'avg_read_latency_ns'):
            self.assertEqual(public[key], diagnostic[key])
        disabled, _ = run_simulator(command + ['--power-model', 'false',
                                               '--thermal-model', 'false'], cwd=ROOT, timeout=20)
        self.assertNotIn('power_energy_pJ', disabled)
        self.assertNotIn('thermal_peak_temp_C', disabled)

    def test_direct_standard_edits_and_derived_capacity_are_visible(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_changes_") as temp:
            temp = Path(temp)
            content = (ROOT / "configs/hbm.cfg").read_text()
            start = content.index('[standard.hbm3.organization]')
            end = content.index('\n[', start + 1)
            section = content[start:end]
            self.assertIn('columns = 32', section)
            edited = section.replace('columns = 32', 'columns = 16')
            cfg = temp / 'copied.cfg'
            cfg.write_text(content[:start] + edited + content[end:])
            result = temp / 'result.json'
            run = subprocess.run([str(BINARY), '--config', str(cfg), '--standard', 'hbm3',
                                  '--requests', '8', '--read-ratio', '0',
                                  '--stats-json', str(result)], cwd=ROOT,
                                 capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)
            raw = json.loads(result.read_text())
            changes = {c['key']: c for c in raw['changes']}
            self.assertEqual(changes['architecture.columns']['baseline'], '32')
            self.assertEqual(changes['architecture.columns']['value'], '16')
            self.assertIn('architecture.density_gb', changes)
            model = raw['model']
            capacity = 1
            for key in ('channels', 'pseudo_channels', 'sids', 'bank_groups',
                        'banks_per_group', 'rows', 'columns', 'dram_transaction_bytes'):
                capacity *= model[key]
            self.assertEqual(model['capacity_per_instance_bytes'], capacity)
            self.assertEqual(model['aggregate_capacity_bytes'], capacity * model['stack_count'])
            self.assertIsNone(raw['metrics']['avg_read_latency_ns'])
            self.assertIn('N/A ns', run.stdout)
            self.assertIn('built-in HBM3', raw['comparison_baseline'])

    def test_diagnostic_opt_in_and_public_units(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_units_") as temp:
            result = Path(temp) / 'result.json'
            for standard in ('hbm3', 'hbm4', 'lpddr5', 'lpddr6'):
                command = [str(BINARY), '--standard', standard, '--requests', '16',
                           '--stack-count', '2', '--stats-view', 'diagnostic',
                           '--stats-json', str(result)]
                run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                raw = json.loads(result.read_text())
                d, m = raw['diagnostics'], raw['metrics']
                ns = raw['parameters']['tick_duration_ps'] / 1000
                self.assertAlmostEqual(m['simulation_time_ns'], d['system_cycles'] * ns)
                self.assertAlmostEqual(m['avg_read_latency_ns'], d['avg_read_latency'] * ns)
                denominator = d['row_hits'] + d['row_misses'] + d['row_conflicts']
                if denominator:
                    self.assertAlmostEqual(m['row_hit_pct'], 100 * d['row_hits'] / denominator)
                self.assertEqual(len(raw['stacks']), 2)
                self.assertEqual(sum(s['completed_reads'] for s in raw['stacks']), m['completed_reads'])
                self.assertNotIn('read_bytes_per_cycle', d)
                self.assertNotIn('read_queue_len_avg', d)
                if standard.startswith('lpddr'):
                    self.assertNotIn('sids', raw['model'])
                    self.assertIn('lpddr_wck_ratio', raw['parameters'])
                else:
                    self.assertNotIn('ranks', raw['model'])
                    self.assertNotIn('lpddr_wck_ratio', raw['parameters'])

    def test_old_results_and_compact_input_error(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_legacy_") as temp:
            path = Path(temp) / 'stats.txt'
            path.write_text('completed_reads: 9007199254740993\n')
            self.assertEqual(count(read_result(path), 'completed_reads'), 9007199254740993)
            path.write_text(json.dumps({'schema_version': 1, 'run_status': 'completed',
                                       'metrics': {'completed_reads': 9007199254740993}}))
            self.assertEqual(count(read_result(path), 'completed_reads'), 9007199254740993)
            path.write_text('# ===== 模型 / MODEL =====\n')
            with self.assertRaisesRegex(ValueError, 'stats-json'):
                read_result(path)

    def test_visualization_example_keeps_complete_results(self):
        with tempfile.TemporaryDirectory(prefix="hbm_visual_example_") as temp:
            env = dict(os.environ, HBM_SIM_BIN=str(BINARY), HBM_SIM_SOURCE_DIR=str(ROOT),
                       HBM_SIM_VIS_OUT=temp)
            run = subprocess.run(["bash", str(ROOT / "tools/visualize_example.sh")],
                                 cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = read_result(Path(temp) / "result.json", require_completed=True)
            self.assertEqual(result["cmd_validation"], "pass")
            self.assertEqual(result["dfi_validation"], "pass")
            self.assertIn("tick_multiplier", result)
            self.assertIn("模型 / MODEL", (Path(temp) / "stats.txt").read_text())
            self.assertTrue((Path(temp) / "dashboard.html").is_file())

    def test_single_stack_output_marker(self):
        with tempfile.TemporaryDirectory(prefix="hbm_single_marker_") as temp:
            temp = Path(temp)
            run = subprocess.run([str(BINARY), "--config", "configs/hbm.cfg", "--requests", "2",
                                  "--dump-thermal-map", str(temp / "thermal_{stack}.txt"),
                                  "--dump-memory-image", str(temp / "final_{stack}.txt")],
                                 cwd=ROOT, capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertTrue((temp / "thermal_0.txt").is_file())
            self.assertTrue((temp / "final_0.txt").is_file())
            self.assertFalse((temp / "thermal_{stack}.txt").exists())

    def test_small_geometry_payload_backend_phy_matrix(self):
        with tempfile.TemporaryDirectory(prefix="hbm_geometry_matrix_") as temp:
            temp = Path(temp)
            for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
                family = "lpddr" if standard.startswith("lpddr") else "hbm"
                config = temp / f"{standard}.cfg"
                config.write_text((ROOT / "configs" / f"{family}.cfg").read_text() +
                    "\nchannels=1\nsids=1\nbank_groups=2\nbanks_per_group=2\n"
                    "rows=16\ncolumns=16\nsupports_refresh=false\nsupports_rfm=false\n")
                golden = None
                for backend in ("sparse", "mmap_sparse", "chunk_file"):
                    for phy in ("direct", "behavioral"):
                        case = temp / f"{standard}_{backend}_{phy}"
                        case.mkdir()
                        final, result = case / "final.txt", case / "result.json"
                        cmd = [str(BINARY), "--config", str(config), "--standard", standard,
                               "--requests", "0", "--trace", "examples/data_check.trace",
                               "--mem-phy", phy, "--memory-backend", backend,
                               "--memory-data-file", str(case / "data.bin"),
                               "--memory-chunk-size", "4096", "--memory-chunk-cache-entries", "2",
                               "--dump-memory-image", str(final), "--stats-json", str(result),
                               "--validate-cmd-trace", "--validate-dfi-trace", "--max-cycles", "10000"]
                        run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=20)
                        self.assertEqual(run.returncode, 0, (standard, backend, phy, run.stderr))
                        metrics = read_result(result, require_completed=True)
                        self.assertGreater(count(metrics, "data_checked_reads"), 0)
                        self.assertEqual(metrics["cmd_validation"], "pass")
                        self.assertEqual(metrics["dfi_validation"], "pass")
                        # PHY modes deliberately differ in commit latency. Keep
                        # all payload/init/ECC checks but exclude that timestamp.
                        image = re.sub(r"last_write_cycle=\d+", "last_write_cycle=<mode-dependent>",
                                       final.read_text())
                        if golden is None:
                            golden = image
                        self.assertEqual(image, golden, (standard, backend, phy))

    def test_schema3_geometry_and_resolved_replay(self):
        with tempfile.TemporaryDirectory(prefix="hbm_config_replay_") as temp:
            temp = Path(temp)
            for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
                family = "lpddr" if standard.startswith("lpddr") else "hbm"
                # A copied standalone master is the supported user workflow.
                content = (ROOT / "configs" / f"{family}.cfg").read_text()
                content += "\ncolumns = 16  # smaller research geometry\n"
                config = temp / f"{standard}.cfg"
                config.write_text(content)
                snapshot = temp / f"{standard}_resolved.cfg"
                result = temp / "result.json"
                command = [str(BINARY), "--config", str(config), "--standard", standard,
                           "--requests", "8", "--stats-json", str(result),
                           "--dump-resolved-config", str(snapshot),
                           "--validate-cmd-trace", "--validate-dfi-trace"]
                run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                first = read_result(result, require_completed=True)
                require_valid_run(first)
                self.assertEqual(count(first, "columns"), 16)
                replay = subprocess.run([str(BINARY), "--config", str(snapshot)], cwd=ROOT,
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(replay.returncode, 0, replay.stderr)
                second = read_result(result, require_completed=True)
                for key in ("aggregate_capacity_bytes", "density_gb", "tCK_ps", "system_cycles",
                            "completed_reads", "completed_writes", "cmd_validation", "dfi_validation"):
                    self.assertEqual(first[key], second[key], (standard, key))

    def test_golden_metrics_are_in_machine_result(self):
        with tempfile.TemporaryDirectory(prefix="hbm_golden_result_") as temp:
            image, result = Path(temp) / "memory.txt", Path(temp) / "result.json"
            setup = subprocess.run([str(BINARY), "--standard", "hbm4", "--requests", "0",
                                    "--trace", "examples/data_check.trace",
                                    "--dump-memory-image", str(image)], cwd=ROOT,
                                   capture_output=True, text=True, timeout=20)
            self.assertEqual(setup.returncode, 0, setup.stderr)
            for preload in (True, False):
                cmd = [str(BINARY), "--standard", "hbm4", "--requests", "0",
                       "--verify-golden", str(image), "--stats-json", str(result)]
                if preload:
                    cmd += ["--memory-image", str(image)]
                run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=20)
                fields = read_result(result)
                self.assertEqual(run.returncode == 0, preload)
                self.assertEqual(count(fields, "golden_mismatches") == 0, preload)
                self.assertEqual(fields["run_status"], "completed" if preload else "failed")

    def test_views_and_failed_rerun(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_test_") as temp:
            result = Path(temp) / "result.json"
            base = [str(BINARY), "--config", "configs/hbm.cfg", "--requests", "16",
                    "--stats-json", str(result)]
            reports = []
            for view in ("summary", "full"):
                run = subprocess.run([*base, "--stats-view", view], cwd=ROOT,
                                     capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                report = read_result(result, require_completed=True)
                require_valid_run(report)
                self.assertIn("row_hit_pct", report)
                self.assertNotIn("row_hits", report)
                raw = json.loads(result.read_text())
                self.assertEqual(raw["schema_version"], 2)
                self.assertEqual(raw["validation"]["dfi_validation"], "not_run")
                self.assertNotIn("diagnostics", raw)
                self.assertLessEqual(len(raw["metrics"]), 24)
                self.assertLessEqual(len(run.stdout.splitlines()), 34)
                for section in ("模型 / MODEL", "参数 / PARAMETERS", "结果 / RESULTS"):
                    self.assertIn(section, run.stdout)
                self.assertNotIn("row_hits", run.stdout)
                reports.append(report)
            self.assertEqual(*reports)
            truncated = subprocess.run([*base, "--max-cycles", "1"], cwd=ROOT,
                                       capture_output=True, timeout=20)
            self.assertEqual(truncated.returncode, 2)
            self.assertEqual(read_result(result)["run_status"], "truncated")
            with self.assertRaises(ValueError):
                read_result(result, require_completed=True)
            failed = subprocess.run([*base, "--read-ratio", "101"], cwd=ROOT,
                                    capture_output=True, timeout=20)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(read_result(result)["run_status"], "failed")

    def test_metrics_fail_closed(self):
        for value in ("nan", "inf", "not-a-number"):
            with self.assertRaises(ValueError):
                number({"metric": value}, "metric")
        with self.assertRaises(ValueError):
            number({}, "metric")
        for value in ("", "-1", "1.5", "false"):
            with self.assertRaises(ValueError):
                count({"counter": value}, "counter")
        with self.assertRaises(ValueError):
            require_valid_run({"hit_cycle_limit": "false", "system_cycles": "10"})

    def test_no_input_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_input_") as temp:
            config = Path(temp) / "model.cfg"
            config.write_text("standard = hbm4\nrequests = 1\n")
            original = config.read_bytes()
            run = subprocess.run([str(BINARY), "--config", str(config),
                                  "--stats-json", str(config)], cwd=ROOT,
                                 capture_output=True, timeout=20)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(config.read_bytes(), original)
            data = Path(temp) / "data_0.bin"
            data.write_bytes(b"existing storage must not become result JSON")
            original_data = data.read_bytes()
            run = subprocess.run([str(BINARY), "--standard", "hbm4", "--stack-count", "2",
                                  "--memory-backend", "mmap_sparse", "--memory-data-file",
                                  str(Path(temp) / "data_{stack}.bin"),
                                  "--stats-json", str(data)], cwd=ROOT,
                                 capture_output=True, timeout=20)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(data.read_bytes(), original_data)


if __name__ == "__main__":
    unittest.main()
