# 测试

当前测试覆盖不只检查程序能否运行，也守住真实存储区语义：payload 写读、read-forward、masked write、row buffer writeback、多 controller storage placement、txt/bin checkpoint、file-backed backend、burst trace、golden initialized-mask 验证和 command/DFI validation 都应在测试中有入口。

`sequence_tests` 还覆盖异步 frontend 响应：request-ready 反压重试、Controller
transaction completion、64 B host request 的多子事务重组、多 Stack 路由元数据、
回调通知、Direct/Behavioral PHY 的 ECC correction 状态，以及有限 response queue
在上层不 ready 时保持响应且不丢数据。契约测试还覆盖 Maintenance 专用入口、
重复/提前复用 host tag 拒绝、HostOnly 不保留 transaction 副本、`idle/quiescent`
边界和批处理在线 response consumer。

本目录保存回归测试。测试覆盖 CLI 主路径、统计字段、命令序列、trace validation 和若干协议边界。

主要文件：

- `smoke.sh`：端到端 CLI smoke regression。
- `sequence_tests.cpp`：C++ 命令序列测试，检查关键命令展开、timing 和 validator 行为。
- `timing_boundary_tests.cpp`：枚举 active TimingConstraint，生成 `t-1/t` 和 scope 边界矩阵。
- `../tools/model_validation.py`：项目级分析验收，检查公式、硬性阈值、DFI 数据路径和来源 manifest。
- `phy_tests.cpp`：Behavioral PHY 生命周期、FIFO/负例、DVFS/WCK、四标准数据回环和 DFI completion。
- `phy_smoke.sh`：四个主配置的在线 PHY/DFI 验证及六 Stack、192 Controller 路径。
- `visualization_smoke.sh`：从实际 CLI command/DFI/thermal 输出生成离线 HTML 仪表盘。

修改建议：

- 新增 CLI 参数或输出字段时，优先补 smoke test。
- 新增协议语义、命令状态或 timing 规则时，优先补 sequence test。
- 测试应保持可在 Makefile 和 CMake/CTest 两条路径中运行。
- 新增真实存储区、floorplan、power、thermal 或 DFI 统计时，要同时覆盖 `sequence_tests.cpp` 的精确断言和 `smoke.sh` 的端到端字段检查。DFI payload 相关修改必须同时检查 `request_payload` 写 beat、`memory_image` 读 beat、`payload_init_mask` 和 `dfi_wrdata_mask` 默认导出语义。
- 涉及数据正确性的修改要检查 `data_checked_reads`、`data_mismatches`、`data_write_commits`、`storage_lines_allocated`，并至少覆盖同地址 `R -> W` 顺序、退出码 3、`--allow-data-mismatch`、`--dump-memory-image`、`--dump-memory-csv`、`--verify-golden` 和 `--mismatch-report`。

## 测试分层

默认 CTest 当前有 10 个入口：

- `smoke.sh`：黑盒测试，运行真实 CLI，检查主要配置、输出字段、trace dump、validator 和工具脚本。
- `sequence_tests.cpp`：白盒/半白盒测试，直接构造请求或命令序列，检查具体命令展开和状态规则。
- `model_validation.py`：固定 HBM4 单 channel 配置执行 28 项项目验收，检查理论峰值、回归带宽门槛、精确延迟、同地址顺序、DFI expected payload、后端重开、timing 来源和 manifest 证据绑定。
- `timing_boundary_tests` 与 `timing_boundary_validation.py`：C++ 边界执行和独立 CSV 审计。
- `performance_curve.py`：CI 规模负载曲线形状检查。
- `sensitivity_uncertainty.py`：CI 规模敏感性和输入区间检查。
- `phy_tests.cpp` / `phy_smoke.sh`：在线 Behavioral PHY 的生命周期、FIFO、DFI completion
  与四标准、多 stack 回归。
- `visualization_smoke.sh`：验证后处理仪表盘兼容当前 trace 和 thermal map 格式；它不
  参与仿真热路径，也不要求浏览器、Node.js 或图形库。

当前 Clang 18 + CMake/Ninja 主路径：

```bash
cmake -S . -B build-clang-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18 \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-clang-debug --parallel
ctest --test-dir build-clang-debug --output-on-failure
```

VS Code 可直接运行任务 `hbm_sim: run all tests`。只调试 C++ 序列测试时，选择
`hbm_sim: sequence tests` CodeLLDB 配置。

Makefile 兼容路径：

```bash
make clean
make CXX=/usr/bin/clang++-18 test
make sanitize-test
```

Make 会对编译器和编译/链接 flags 建立构建签名，模式改变时自动重编译，防止
普通链接复用 sanitizer object。`make list-builds` 可先审查构建目录，确认后用
`make clean-build` 删除根目录下全部 `build`/`build-*`；它不会删除运行输出。

本地安装了 Ramulator2.1 后，可额外运行不属于默认回归的外部参考检查：

```bash
python3 tools/ramulator2_differential.py \
  --binary ./build-clang-debug/hbm_sim \
  --ramulator-root /path/to/ramulator2
```

该检查只比较固定配置下双方刻意共享的 timing、命令顺序、地址坐标、周期和维护
场景。它不等同于实现规范或真实器件校准；外部参考仍不属于默认 CTest。

测试或手动仿真后清理根目录 CSV/TXT/BIN 输出：

```bash
make delete
```

## 什么时候补 smoke test

- 新增 CLI 参数。
- 新增输出字段。
- 新增 config 文件。
- 修改 `Stats` 输出格式。
- 修改 command trace CSV 或 validation report。
- 修改 memory image、binary checkpoint、golden verification、mismatch report、thermal map、DFI trace/signal trace 的导出格式。
- 修改理论带宽、延迟口径、timing source 或 vendor manifest。

## 什么时候补 sequence test

- 新增命令。
- 修改 ACT/PRE/RD/WR/REF/RFM 合法性。
- 修改 HBM edge pairing。
- 修改 LPDDR WCK/CAS/DVFS/低功耗状态。
- 修改 HBM4/LPDDR6 host line、DRAM transaction 或地址几何时，要保留
  64B host 拆成两个 32B 事务以及 32GiB/4GiB 容量断言。
- 修改 timing constraint 或 tFAW/WCK window。
- 修改 `MemoryImage`、row buffer、masked write、read-forward、SECDED shadow 或物理坐标映射。
- 修改 DFI beat、phase、latency、signal 或 payload 绑定时，必须同时保留 validator 正例和故障注入反例。

## 测试设计原则

- 正例和反例都要有，尤其是 validator 规则。
- 单个测试只关注一个协议点，避免失败后难以定位。
- 对输出字段的 grep 应检查字段名和关键结果，不要依赖整段输出完全一致。
- 修改命令序列后，注意同时检查在线 controller 和离线 validator。
- 修改真实存储区后，注意同时检查在线读写路径、txt/CSV dump 和 mismatch report 是否仍能人工核对。
