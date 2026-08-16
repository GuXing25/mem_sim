# 工具

工具目录用于放置不参与仿真热路径的辅助脚本。当前主要用途是比较文本统计输出；memory image 的 txt/CSV/bin 读写由模拟器本身完成，不放在 tools 中重新实现，避免出现第二套不一致的数据格式。

本目录保存辅助工具脚本，不属于模拟器核心库。工具应尽量保持独立、可从命令行直接运行。

主要文件：

- `compare_stats.py`：比较两次仿真输出中的关键统计字段，用于轻量 golden/baseline 对比。
- `model_validation.py`：运行项目原生 HBM4 配置，检查分析公式、性能阈值、真实存储、full-stack、DFI、来源和 project identity。
- `timing_boundary_validation.py`：审计 C++ probe 生成的四标准 `t-1/t` Timing 边界矩阵。
- `ramulator2_differential.py`：将本地 Ramulator2.1 作为非规范性外部参考，检查四标准共同命令面。
- `performance_curve.py`：生成注入率—延迟—吞吐量 CSV/JSON 曲线。
- `sensitivity_uncertainty.py`：生成 Timing 单因素敏感性和联合输入扰动区间。
- `dramsim3_aux_validation.py`：执行较老 HBM 公共命令、IDD 功耗公式和热趋势辅助验证。

修改建议：

- 工具输出应适合 CI 和人工阅读，失败时说明字段、baseline、candidate 和阈值。
- 不要把核心仿真逻辑写进 tools；核心行为应在 `src/`。
- Python 工具产生的 `__pycache__/` 已被 `.gitignore` 忽略。
- 后续适合放在这里的工具包括：`final_memory.txt` 转 CSV/Excel、checkpoint diff、`mismatch_report.txt` 汇总、`thermal_map.txt` 热点排序/绘图，以及更广的 DRAMsim3/Ramulator2.1 差分矩阵。

## `compare_stats.py`

这个工具用于比较两份 `hbm_sim` 文本输出中的统计字段。典型用途：

- 对比本次修改前后的 smoke 输出。
- 对比某个配置在不同 scheduler/channel mapper 下的关键指标。
- 作为通用 golden/baseline 文本统计检查；结构化 Ramulator2.1 命令差分使用 `ramulator2_differential.py`。

使用方式示例：

```bash
python3 tools/compare_stats.py baseline.txt candidate.txt \
  --keys completed_reads,commands.ACT --abs-tol 0 --rel-tol 0

python3 tools/compare_stats.py baseline.txt candidate.txt \
  --keys achieved_bw_GBps --rel-tol 0.05
```

## `model_validation.py`

```bash
python3 tools/model_validation.py \
  --binary ./build-clang-debug/hbm_sim \
  --json-out outputs/hbm_sim_model_validation.json
```

它也由 `ctest --test-dir build-clang-debug --output-on-failure` 自动运行。Makefile
兼容入口为 `make CXX=/usr/bin/clang++-18 model-validation`。

当前项目原生持续负载基线为 64.5%，默认允许 4.5 个百分点的回归余量，因此门槛为
60%。它是固定 workload/controller/profile 的经验回归基线，不是从 `nCCDR` 单独
推导的器件效率。脚本还检查同地址顺序、mismatch 退出码、DFI expected payload、
file-backed 重开、四标准入口、burst、物理/功耗/热路径和 identity 证据绑定。

## `ramulator2_differential.py`

```bash
python3 tools/ramulator2_differential.py \
  --binary ./build-clang-debug/hbm_sim \
  --ramulator-root /path/to/ramulator2 \
  --json-out outputs/hbm_sim_ramulator2_diff.json
```

Makefile 兼容入口为
`make CXX=/usr/bin/clang++-18 reference-validation RAMULATOR2_ROOT=/path/to/ramulator2`。

当前参考检查覆盖 HBM3/HBM4/LPDDR5/LPDDR6 的 timing、命令顺序、decoded 坐标、
逐命令周期、same/different BG、双向总线换向、tFAW、refresh/RFM 和自动预充电。
局部容差与规范化规则逐场景记录，不能表述为实现或器件等价。

字段比较建议：

- 请求完成数、命令计数：通常使用绝对误差 `0`。
- 带宽、延迟、利用率：可使用相对误差，例如 `0.01` 或 `0.05`。
- 队列平均值：通常允许一定误差，因为调度微调可能影响排队过程。

## 工具维护原则

- 工具只读取文件和打印比较结果，不调用内部 C++ API。
- 失败输出要说明字段名、baseline、candidate、差值和阈值。
- 用户希望保留的验证产物写到项目内 `outputs/`；测试内部的短生命周期夹具可使用
  系统临时目录，并在退出前删除。
- 不要提交 `__pycache__`、临时输出、实验日志。

## 适合新增的工具

- `memory_image_to_csv.py`：把 `final_memory.txt` 的 `key=value` 文本转成标准 CSV，便于 Excel 打开。
- `memory_image_diff.py`：比较两个 memory image dump 的 `address/data/init/version/last_writer`。
- `mismatch_summary.py`：统计 mismatch report 中的地址、请求号、bank/row/column 和 expected/actual 差异。
- `thermal_hotspots.py`：按温度或能量排序 `thermal_map.txt`，输出热点坐标。
