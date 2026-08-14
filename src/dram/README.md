# DRAM 模型实现

dram 层当前负责把 HBM3/HBM4/LPDDR5/LPDDR6 标准语义转成可运行 `DramSpec`、timing table、命令语义、状态机和接口开销。真实 payload 存储、文件后端和 CSV/bin checkpoint 不属于 dram 层。

本目录实现 DRAM 标准和器件描述层。它负责把 HBM/LPDDR 的组织结构、timing、
命令语义、状态合法性和接口开销表达成 controller 可消费的数据。

主要文件：

- `standard_traits.cpp`：标准身份、别名、协议能力和默认 profile 选择。
- `profiles.cpp`：按 speed-bin、density、stack-height、mode、vendor 展开完整组织和 timing。
- `spec.cpp`：统一 `make_spec()`、timing table、constraint 派生和 finalize。
- `jedec.cpp`：JEDEC 单位换算和常用公式。
- `semantics.cpp`：命令分类、bus/scope 元数据。
- `state.cpp`：ACT/PRE/RD/WR/REF/RFM/MR/WCK/DVFS/低功耗等合法性检查。
- `interface.cpp`：payload、ECC、DBI、CRC、metadata lane 的接口占用计算。

修改建议：

- 手册或厂商表数值优先落在 `profiles.cpp` 或 `configs/profiles/`。
- 如果某个行为会影响命令能不能发，改 `state.cpp`；如果影响命令发出后的状态，改 `controller/executor.cpp`。
- 带宽、接口传输率和 payload efficiency 相关开销统一从 `interface.cpp` 进入统计。
- dram 层不保存真实 payload，也不直接计算热图；真实存储区、floorplan、power 和 thermal 由 `core/data.cpp` 根据 `DramSpec` 的组织结构消费。

## `spec.cpp`

`make_spec(name)` 是完整默认模型的便利入口，内部执行
`traits -> default profile -> finalize`。CLI 为了让配置优先级唯一，使用
`make_spec_draft(name) -> profile selectors -> profile -> overrides -> finalize`。

`StandardTraits` 不保存 `Organization` 或 `Timing`，只承载标准身份、别名、协议
能力、scope 和默认 profile 选择。四个私有 `apply_*_profile()` 是标准表/公式的数值
策略，不是四套公开模型构造器。命令是否合法、发出后如何迁移状态仍由
`semantics.cpp`、`state.cpp` 和 controller 实现。因此：

- 新 speed bin、density、stack height、mode 或 vendor 数据优先进入
  `profiles.cpp`/`configs/profiles/`。
- 新标准若仍属于现有 HBM/LPDDR 协议族，先增加 traits/profile 数据项。
- 只有出现新的命令语义或状态迁移时，才修改协议实现。

这里需要特别注意 timing 来源：

- JEDEC 固定值或公式换算应标为 `JEDEC` 或 `derived`。
- 厂商表应标为 `vendor`。
- 临时研究值应标为 `research_default`，并在 strict timing table 中可见。

LPDDR6 当前按用户提供的 JESD209-6 路线补入了 REFdb 相关字段：

- `nREFDB2ACT`：REFdb 后到 ACT 的间隔。
- `nREFDB2REFDBS`：dual-bank refresh 到短间隔下一次 REFdb。
- `nREFDB2REFDBL`：dual-bank refresh 到长间隔下一次 REFdb。

调度器目前还没有 bank-pair 级 REFdb short/long predicate，因此保守地使用长间隔约束
REFdb 到 REFdb。这样数值偏保守，但不会比标准短。后续如果补充 bank-pair 拓扑，可在
`state.cpp` 或 timing constraint predicate 层把 short/long 精确拆开。

## `profiles.cpp`

`profiles.cpp` 用于把 `speed_bin_mbps + density_gb + stack_height + mode_profile + vendor_profile`
展开成一组具体参数。它适合放通用规则或经常复用的 profile。单个实验临时表格更适合放在
`configs/profiles/`。

## `state.cpp`

`state.cpp` 判断命令是否合法，例如：

- bank 是否 closed/opened/activating。
- REF/RFM 是否要求 all-bank idle。
- LPDDR CAS/RD/WR 是否需要 WCK ready。
- power-down/self-refresh 状态是否阻塞普通命令。
- mode register、training、DVFS、RAS/ECC 命令是否满足状态条件。

它不执行状态变化。状态变化在 `controller/executor.cpp`。

## `interface.cpp`

`interface.cpp` 负责把 payload byte 和协议开销分开。这里会影响：

- `interface_read_bytes` / `interface_write_bytes`。
- `interface_overhead_bits`。
- `achieved_if_bw_GBps`。
- `payload_efficiency_pct`。

HBM CRC/ECC/RAS metadata、LPDDR DBI/link ECC/CA parity 都应通过这里进入接口统计。

## DFI 字段

`DramSpec` 现在包含第一版 DFI5.0 相关抽象字段：

- `dfi_phase_count`
- `dfi_data_lane_bytes`
- `dfi_read_latency_nck`
- `dfi_write_latency_nck`

这些字段描述 DFI 后处理所需的相位、data beat 粒度和读写数据延迟。`validation/dfi.cpp`
会基于它们生成两类输出：

- beat CSV：`COMMAND`、`READ_DATA`、`WRITE_DATA` 事件。
- signal-like CSV：`dfi_reset_n`、`dfi_cs_n`、`dfi_cke`、`dfi_odt`、`dfi_address`、
  `dfi_bank`、`dfi_rddata_en`、`dfi_wrdata_en`、`dfi_rddata_valid`、`dfi_wrdata_mask`、
  `dfi_wrdata`、`dfi_rddata` 等字段。

这些字段只描述 controller/PHY 边界的 trace 视图。真实 write payload/read payload 由
controller 在 `IssuedCommand` 上回填，DFI builder 按真实 payload 长度切分 beat；没有 payload
快照时才使用 `synthetic_fallback`。它们仍不代表完整 DFI5.0 pin-level 协议，也不会改变
DRAM bank 状态机；状态合法性仍由 `state.cpp` 和 controller executor 负责。没有厂商 PHY
资料支撑的 lane 拓扑、训练流程和精确 pin packing 暂时保持 project-defined。
