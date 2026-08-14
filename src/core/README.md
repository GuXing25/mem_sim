# 核心实现

core 层当前包含真实存储区热路径：`data.cpp` 实现 `MemoryImage`、row buffer、物理坐标、ECC/power/thermal；`stack_model.cpp` 实现外部 MC 可驱动的被动多 stack 器件数组；`memory_backend.cpp` 实现三类 payload 后端；`system.cpp` 实现主动多 stack、多 channel controller、入口反压/QoS 和 streaming injection。

本目录实现跨模块核心逻辑，主要是地址映射、多 controller memory system 和真实堆叠存储模型。

主要文件：

- `addr_map.cpp`：系统地址的 interleaved/blocked stack 映射，以及 stack-local Ramulator 风格 channel/pseudo-channel/SID 映射。
- `data.cpp`：真实稀疏存储区、payload 读写、SECDED shadow、数据校验辅助、row buffer、bank/row/column/subarray/mat/cell/microbump 索引、floorplan、tile 内 thermal grid、DRAMsim3-style IDD/VDD 功耗校准、TSV-aware sparse 3D RC 热耦合和热事件。
- `stack_model.cpp`：被动多 stack 器件数组实现。默认实例化 6 个 stack；显式 `stack_count` 构造保留给对比实验。它只根据外部给出的 `stack_id` 分发 transaction/command，并维护每个 stack 独立的 `MemoryImage`、物理坐标、功耗、热和统计；Bridge-MC Frontend 的地址选择、QoS、reorder、credit 和跨 stack 调度不在这里实现。
- `system.cpp`：每 stack 一套多 channel controller 的创建、stack ingress 反压/QoS、请求分发、并行 tick、per-stack 与全局统计合并。

修改建议：

- 新增地址映射模板时，保持模板名语义清楚，例如 `RoBaRaCoCh`。
- 多 controller 行为应保持一个 channel 一个 controller 的结构，便于对齐 Ramulator2.1。
- system 层只合并统计和 trace，不应实现 DRAM 命令状态机。
- `data.cpp` 是模型层入口，不应反向依赖 CLI；txt load/dump 只作为 checkpoint/report，不参与 RD/WR 热路径的文件 I/O。

## `addr_map.cpp`

地址映射把线性地址拆成 channel、rank、SID、pseudo-channel、bank group、bank、row、column。
它直接影响：

- stream 流量是否均匀分布到多个 channel。
- row hit、row miss、row conflict 的比例。
- HBM pseudo-channel/SID interleave 是否被激活。
- LPDDR efficiency mode 下 secondary subchannel 是否被折叠。

新增映射模板时，要明确模板名的位序含义。项目中类似 `RoBaRaCoCh` 的名字按高位到低位解释。

## `data.cpp`

`data.cpp` 把“地址发生了读写”提升为“真实数据落在堆叠存储区域里”：

- `MemoryImage` 使用稀疏 map 保存访问过的 line，避免分配完整 HBM 容量。
- `DataBlock` 保存 bytes、mask、initialized mask、version、last writer、checksum 和物理坐标。
- `StorageKey` 提供 stack / channel / pseudo-channel / SID / rank / bank group / bank / row / column 视图，保证多个 stack 中相同局部 bank/row/column 不会被当成同一物理位置。
- `RowBufferEntry` 模拟 ACT 打开 row、RD/WR 访问 row buffer、PRE/flush 写回 dirty row。
- `PhysicalAddress` 把存储键映射到 stack / die / layer / tile_x / tile_y / tile_z，并继续给出 tile 内 `thermal_x/thermal_y` 网格坐标、subarray、mat、cell 和 microbump 坐标。
- `StorageModelOptions` 控制 floorplan、power、thermal、命令能量、DRAMsim3-style IDD/VDD 校准参数、sparse 3D RC 热耦合、TSV 垂直耦合、细粒度几何坐标和 SECDED shadow。
- `DataBlock` 除 payload/mask/version 外，还保存 line 级 SECDED shadow。写入后刷新 shadow，读出前可检查并修正单 bit 错误；`ecc_inject_period` 用于回归测试错误注入。
- `dump_text()`、`dump_csv()`、`dump_binary()`、`load_binary()`、`read_initialized_mask()`、`dump_thermal_text()` 和 `DataValidator::dump_text()` 提供 checkpoint/report/golden verification 支撑。txt 适合代码审计和 diff，CSV 适合 Excel/WPS 直接打开检查 address、data、init mask、version、last writer、bank/row/column、tile 和 ECC shadow，binary 适合大规模稀疏 checkpoint。
- 运行时不直接读写文本或 CSV；输入文件只在启动时加载，输出文件只在结束后
  生成。HBM/LPDDR 容量较大，不适合默认创建完整稠密后端文件。

当前热模型的实现边界很清楚：`thermal_grid_cols_per_tile` 和 `thermal_grid_rows_per_tile` 让 bank tile 内部可以按 row/column 切成网格，`power_source = dramsim3_idd` 会按 DRAMsim3 的 `VDD * IDD * time` 思路推导 ACT/RD/WR/REF 能量；温度更新已经加入横向邻接、上下层邻接和 TSV-aware 垂直耦合，但仍是轻量显式 RC 近似，不是完整 SuperLU 3D 稳态/瞬态求解器。

## `stack_model.cpp`

`stack_model.cpp` 是面向未来 RTL MC 后端的被动器件接口：

- `StackModel` 表示一个 stack device。它有自己的 `MemoryImage`，因此同一个局部地址在不同 stack 中可以保存不同 payload。
- `MultiStackMemoryModel` 表示多个 stack device 的数组，默认 6 个 stack，只按 `stack_id` 做边界检查、分发和统计汇总。
- transaction-level `read/write` 用于当前软件 workload 或快速验证。
- command-level `StackCommand` 用于未来 MC slice 驱动：外部 RTL MC 产生 `ACT/PRE/RD/WR/REF`，模型记录命令事件、维护数据/row buffer、输出功耗和热统计。
- 这里不做 UCIe flit、Bridge-MC Frontend、全局 QoS、reorder、credit 或 stack 选择策略；这些属于上游 RTL 设计。

## `system.cpp`

`MemorySystem` 管理 `stack_count * channels_per_stack` 个 controller。它负责：

- 把全局地址映射成 stack_id 和 stack-local 地址。
- 通过 per-stack ingress queue 表达反压，并按 FCFS/strict-priority QoS 分发。
- 根据 stack-local decoded channel 或 channel mapper 选择 controller。
- 让所有 stack/channel controller 在同一个 system cycle 并行 tick。
- 合并 per-stack/全局统计和带 `stack_id` 的 command trace。

它不负责决定单个 controller 内本周期发什么 DRAM 命令，这个职责仍在
`src/controller/`；也不宣称模拟完整 UCIe flit/credit/link 仲裁。

## 调试建议

- 如果 `active_controllers` 很小，优先检查 address mapping 和 channel mapper。
- 如果总带宽很低但单 controller 很忙，可能是流量没有分散到足够 channel。
- 如果多 controller 合并统计异常，检查 system-level 和 aggregate-controller-level 分母是否混用。
