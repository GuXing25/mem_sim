# 统计实现

stats 输出是项目的稳定对外接口之一。当前字段已包含真实存储区、backend 覆盖率、burst 拆分、row buffer、floorplan、power/thermal、ECC、DFI 和带宽效率。修改字段名会影响 smoke、工具脚本和用户表格。

本目录实现统计输出。统计层负责把 `Stats` 中的字段以稳定、对齐、脚本友好的文本格式打印出来。

严格来说，`stats.cpp` 不负责“产生”这些统计状态；这些变量定义在
`include/hbm_sim/stats/stats.hpp` 的 `Stats` 结构里，由 controller、memory
system、命令执行器和维护逻辑在仿真过程中累加。`stats.cpp` 的职责是把它们按固定
顺序输出，并计算少量派生指标。

为什么要定义这些变量：

- 保留原始事件：请求进入、请求完成、命令发出、刷新/RFM/低功耗事件都需要单独计数。
- 支持性能判断：带宽、利用率、平均延迟、队列平均长度都来自这些基础计数。
- 区分统计口径：payload byte、interface byte、system cycle、controller cycle 不能混在一起。
- 方便验证回归：输出字段顺序稳定后，smoke test、脚本和实验结果对比都能直接解析。
- 暴露模型行为：行命中/冲突、双边沿命令、维护请求、WCK/DVFS/RAS 等字段能说明瓶颈来自哪里。
- 暴露堆叠存储模型：`data_*`、`storage_*`、`rowbuf_*`、`floorplan_*`、`power_*`、`thermal_*`、`dfi_*` 字段用于解释真实数据、物理位置、能量和热图行为。

主要文件：

- `stats.cpp`：统计字段格式化、带宽/利用率/队列平均值等派生指标输出。

## 输出字段速查

说明：

- 下表按 `stats.cpp` 的输出顺序排列，便于和命令行结果一一对照。
- `commands.*` 是 DRAM 命令计数，不是上层 read/write 请求计数。
- `*_bytes` 是有效数据载荷，`interface_*_bytes` 还会计入接口层传输开销。
- `*_avg_per_ctrl` 使用 `aggregate_ctrl_cycles` 作分母，更适合多 controller 对比。

| 字段 | 来源/公式 | 含义 |
| --- | --- | --- |
| `cycles` | 仿真结束时记录 | 本次运行推进到的周期数；如果命中 cycle limit，则是停止时的周期。 |
| `reads` | 请求入队 | 上层成功提交到系统的读请求数。 |
| `writes` | 请求入队 | 上层成功提交到系统的写请求数。 |
| `completed_reads` | 请求完成 | 已完成的读请求数；读转发也会算完成。 |
| `completed_writes` | 请求完成 | 已完成的写请求数；写合并也会算完成。 |
| `row_hits` | 调度时判定 | 请求访问已经打开的目标 row。 |
| `row_misses` | 调度时判定 | 请求访问的 bank 当前没有打开 row，需要激活。 |
| `row_conflicts` | 调度时判定 | bank 已打开其他 row，需要先预充电再激活目标 row。 |
| `commands.ACT` | 命令发出 | 标准激活命令，打开目标 row。 |
| `commands.ACT1` | 命令发出 | 两步激活协议中的第一拍激活。 |
| `commands.ACT2` | 命令发出 | 两步激活协议中的第二拍激活。 |
| `commands.PRE` | 命令发出 | 普通预充电，关闭目标 bank 的当前 row。 |
| `commands.PREpb` | 命令发出 | per-bank 预充电。 |
| `commands.PREab` | 命令发出 | all-bank 预充电。 |
| `commands.CAS_RD` | 命令发出 | LPDDR 类协议的读 CAS 前置命令。 |
| `commands.CAS_WR` | 命令发出 | LPDDR 类协议的写 CAS 前置命令。 |
| `commands.RD` | 命令发出 | 读数据命令，最终触发读完成延迟计算。 |
| `commands.WR` | 命令发出 | 写数据命令，当前模型写完成延迟较简化。 |
| `commands.RDA` | 命令发出 | 带 auto-precharge 的读数据命令。 |
| `commands.WRA` | 命令发出 | 带 auto-precharge 的写数据命令。 |
| `commands.REFab` | 命令发出 | all-bank refresh。 |
| `commands.REFpb` | 命令发出 | per-bank refresh。 |
| `commands.REFdb` | 命令发出 | dual-bank refresh。 |
| `commands.RFMab` | 命令发出 | all-bank row-fault/rowhammer mitigation。 |
| `commands.RFMpb` | 命令发出 | per-bank row-fault/rowhammer mitigation。 |
| `commands.MRW` | 命令发出 | mode register write。 |
| `commands.MRR` | 命令发出 | mode register read。 |
| `commands.WCK_SYNC` | 命令发出 | WCK 同步命令。 |
| `commands.WCK_TRAIN` | 命令发出 | WCK training 命令。 |
| `commands.DVFS` | 命令发出 | DVFS 频率/电压切换命令。 |
| `commands.PDE` | 命令发出 | power-down entry。 |
| `commands.PDX` | 命令发出 | power-down exit。 |
| `commands.SREFEN` | 命令发出 | self-refresh entry。 |
| `commands.SREFEX` | 命令发出 | self-refresh exit。 |
| `commands.ECC_SCRUB` | 命令发出 | ECC scrub 维护命令。 |
| `commands.RAS_ERR` | 命令发出 | RAS 错误处理相关命令。 |
| `maintenance_requests` | 维护逻辑入队 | 控制器内部生成的维护请求数量，例如 refresh/RFM。 |
| `maintenance_served` | 维护请求完成 | 已经服务完的维护请求数量。 |
| `refresh_batches` | refresh 管理器 | refresh 批次数总计。 |
| `refresh_pb_batches` | refresh 管理器 | per-bank refresh 批次数。 |
| `refresh_ab_batches` | refresh 管理器 | all-bank refresh 批次数。 |
| `refresh_postpones` | refresh 信用逻辑 | refresh 被推迟的次数。 |
| `refresh_pullins` | refresh 信用逻辑 | refresh 被提前拉入执行的次数。 |
| `refresh_credit_peak` | refresh 信用逻辑 | refresh postpone/pull-in 信用达到过的峰值。 |
| `rfm_events` | RFM 管理器 | RFM 事件总数。 |
| `rfm_pb_events` | RFM 管理器 | per-bank RFM 事件数。 |
| `rfm_ab_events` | RFM 管理器 | all-bank RFM 事件数。 |
| `dual_issue_cycles` | 命令调度 | 同一个周期同时发出 rising/falling edge 命令的周期数。 |
| `row_bus_issues` | 命令调度 | 占用 row command bus 的命令次数。 |
| `column_bus_issues` | 命令调度 | 占用 column command bus 的命令次数。 |
| `unified_bus_issues` | 命令调度 | 占用统一命令总线的命令次数。 |
| `rising_edge_ticks` | 命令调度 | rising edge 上发出的命令次数。 |
| `falling_edge_ticks` | 命令调度 | falling edge 上发出的命令次数。 |
| `write_mode_cycles` | 读写切换状态 | 控制器处于 write-drain/write-mode 的周期数。 |
| `injected_requests` | frontend/system | 成功注入到 memory system 的请求数。 |
| `injection_stall_cycles` | frontend/system | 因队列满或系统不可接收而无法注入的周期数。 |
| `read_forwards` | 写缓冲旁路 | 读请求命中尚未写回的数据，被写缓冲直接满足的次数。 |
| `write_coalesces` | 写缓冲合并 | 新写请求和已有写缓冲项合并的次数。 |
| `data_checked_reads` | 数据校验 | 携带 expected payload 并完成比较的读请求数。 |
| `data_mismatches` | 数据校验 | actual payload 与 expected payload 不一致的次数。 |
| `data_uninitialized_reads` | 数据校验 | 读到未初始化字节的次数。 |
| `data_write_commits` | 真实存储区 | WR/WRA 完成后写入 `MemoryImage` 的次数。 |
| `data_masked_write_commits` | 真实存储区 | 带 byte mask 的写提交次数。 |
| `data_forward_checks` | 数据校验 | 读转发路径上执行 expected/actual 比较的次数。 |
| `storage_lines_allocated` | 稀疏存储区 | `MemoryImage` 中实际分配的 cache-line 数。 |
| `storage_bytes_allocated` | 稀疏存储区 | 已分配 line 折算出的运行时存储字节数。 |
| `storage_channels_touched` 等 | 物理坐标覆盖 | 已访问数据覆盖的 channel/PC/SID/rank/BG/bank/row/column 数。 |
| `floorplan_tiles_touched` | floorplan | 已分配数据覆盖的 bank tile 数。 |
| `thermal_tiles_touched` | 热模型 | 已产生热事件的热单元数，默认等于 thermal grid cell 数。 |
| `thermal_grid_cells_touched` | 热模型 | 已产生热事件的 tile 内网格单元数。 |
| `storage_read_line_accesses` | 真实存储区 | `MemoryImage` line 级读访问次数。 |
| `storage_write_line_accesses` | 真实存储区 | `MemoryImage` line 级写访问次数。 |
| `rowbuf_*` | 行缓冲模型 | ACT/PRE、dirty writeback、hit/miss、lazy load、open/dirty row 等行缓冲统计。 |
| `power_events` | 功耗模型 | 进入存储模型的命令能量事件数。 |
| `power_*_energy_pJ` | 功耗模型 | 命令类别累计能量；`power_source=dramsim3_idd` 时由 VDD/IDD/timing 推导。 |
| `thermal_peak_temp_C` / `thermal_avg_temp_C` | 热模型 | 简化 RC 更新得到的历史热点温度，以及查询时刻已实例化稀疏 thermal-grid 节点的空间平均；后者不是全 die 或时间平均。 |
| `thermal_hotspot_layer/x/y` | 热模型 | 当前热点所在 layer 和 thermal-grid 坐标。 |
| `thermal_lateral_transfers` | 热模型 | 横向邻接热耦合发生次数。 |
| `thermal_vertical_transfers` | 热模型 | 上下层热耦合发生次数。 |
| `thermal_tsv_transfers` | 热模型 | 通过 TSV-aware 垂直路径计入的热耦合次数。 |
| `thermal_coupled_delta_C` | 热模型 | 热耦合导致的温度交换量累计绝对值。 |
| `ecc_shadow_updates` | ECC shadow | 写入或纠错后刷新 SECDED shadow 的次数。 |
| `ecc_checked_reads` | ECC shadow | 读路径执行 SECDED shadow 检查的次数。 |
| `ecc_corrected_errors` | ECC shadow | 成功修正单 bit 数据错误的次数。 |
| `ecc_uncorrectable_errors` | ECC shadow | 被判定为不可纠正错误的次数。 |
| `ecc_injected_errors` | ECC shadow | 由 `ecc_inject_period` 注入的错误次数。 |
| `ecc_parity_repairs` | ECC shadow | 修复 ECC parity/shadow 不一致的次数。 |
| `dfi_read_beats` / `dfi_write_beats` | DFI beat 视图 | 每条 RD/WR 的实际 payload 按 DFI beat byte 数拆分后的 beat 数。默认 beat byte 由 `dram_transaction_bytes / nBL` 推导。 |
| `dfi_forwarded_read_beats` / `dfi_masked_write_beats` | DFI beat 视图 | 读转发和 masked write 对应的 beat 数。 |
| `dfi_data_bytes` / `dfi_beat_bytes` | DFI beat 视图 | DFI 视图中的数据字节和每 beat 字节数。 |
| `row_policy_ap_upgrades` | row policy | row policy 把普通 RD/WR 升级成 RDA/WRA 的次数。 |
| `row_policy_precharges` | row policy | row policy 主动插入 PRE 的次数。 |
| `wck_syncs` | WCK 管理 | 实际执行 WCK sync 的次数。 |
| `wck_sync_skips` | WCK 管理 | 因已经满足条件而跳过 WCK sync 的次数。 |
| `wck_training_events` | WCK 管理 | WCK training 事件数。 |
| `mode_register_ops` | mode register | MRW/MRR 类模式寄存器操作数。 |
| `dvfs_transitions` | DVFS 管理 | DVFS 状态切换次数。 |
| `ras_ecc_events` | RAS/ECC 管理 | RAS 或 ECC 维护事件数。 |
| `act2_deadline_forced` | ACT1/ACT2 约束 | 因 ACT2 deadline 被强制优先调度的次数。 |
| `rfm_decrements` | RFM 管理 | RFM 计数器递减事件数。 |
| `low_power_entries` | 低功耗管理 | 进入低功耗状态的次数。 |
| `low_power_exits` | 低功耗管理 | 退出低功耗状态的次数。 |
| `low_power_cycles` | 低功耗管理 | 处于低功耗状态的总周期数。 |
| `low_power_exit_blocked` | 低功耗管理 | 退出低功耗但仍被时序/状态阻塞的次数。 |
| `interface_command_bits` | 接口统计 | 命令保护的记账 bit 数；当前不增加 CA 总线拍。 |
| `interface_overhead_bits` | 接口统计 | 非 payload 的记账接口开销 bit 数。 |
| `host_requests` | frontend | 拆分前的普通 host/cache-line 请求数，不含维护请求。 |
| `dram_transactions` | frontend | 拆分后送入控制器的物理 RD/WR 请求数，不含维护请求。 |
| `controller_count` | MemorySystem | 系统中 controller 的总数。 |
| `active_controllers` | MemorySystem | 本次运行中实际接收过请求或产生过活动的 controller 数。 |
| `stack_count` | MemorySystem | 配置的独立 stack 数。 |
| `active_stacks` | MemorySystem | 本次运行实际接收请求的 stack 数。 |
| `stack_ingress_stalls` | MemorySystem | 目标 stack ingress 满导致的反压次数。 |
| `stack_ingress_peak` | MemorySystem | 所有 stack 中观察到的最大入口占用。 |
| `qos_priority_dispatches` | MemorySystem | `qos>0` 请求被 stack ingress 分发的次数。 |
| `system_cycles` | MemorySystem | system 级别经过的周期数。 |
| `aggregate_ctrl_cycles` | controller 累加 | 所有 controller 的有效周期数之和。 |
| `read_bytes` | `completed_reads * burst_bytes` | 完成读请求对应的 payload 字节数。 |
| `write_bytes` | `completed_writes * burst_bytes` | 完成写请求对应的 payload 字节数。 |
| `interface_read_bytes` | 接口统计 | 读方向 payload 加协议开销的等效需求字节数。 |
| `interface_write_bytes` | 接口统计 | 写方向 payload 加协议开销的等效需求字节数。 |
| `if_xfer_rate_Gbps` | 配置参数 | 接口侧传输速率，用于估算峰值带宽。 |
| `peak_bandwidth_GBps` | 配置推导 | 理想峰值带宽。 |
| `achieved_bw_GBps` | payload/time | 按 payload byte 计算的实际带宽。 |
| `achieved_if_bw_GBps` | interface/time | 按 payload+协议开销计算的记账等效需求带宽；不是 pin-accurate 实测值。 |
| `bandwidth_util_pct` | `achieved_bw_GBps / peak_bandwidth_GBps` | payload 带宽占峰值带宽的百分比。 |
| `payload_efficiency_pct` | `payload_bytes / interface_bytes` | 有效 payload 在接口传输中的占比。 |
| `remaining_requests` | 队列状态 | 仿真结束时仍在读/写/优先队列里的请求数。 |
| `remaining_pending` | pending 状态 | 已发出末级命令但尚未完成统计的请求数。 |
| `hit_cycle_limit` | 终止条件 | 是否因为达到最大 cycle limit 而停止。 |
| `avg_read_latency` | `read_latency_sum / completed_reads` | 平均读延迟，从请求进入系统到读完成。 |
| `read_queue_len_avg` | `read_queue_len_sum / cycles` | 全系统读队列平均占用。 |
| `write_queue_len_avg` | `write_queue_len_sum / cycles` | 全系统写队列平均占用。 |
| `priority_queue_avg` | `priority_queue_len_sum / cycles` | 全系统高优先级/维护队列平均占用。 |
| `active_queue_len_avg` | `active_queue_len_sum / cycles` | 全系统活动队列平均占用。 |
| `read_q_avg_per_ctrl` | `read_queue_len_sum / aggregate_ctrl_cycles` | 单 controller 视角的读队列平均占用。 |
| `write_q_avg_per_ctrl` | `write_queue_len_sum / aggregate_ctrl_cycles` | 单 controller 视角的写队列平均占用。 |
| `priority_q_avg_per_ctrl` | `priority_queue_len_sum / aggregate_ctrl_cycles` | 单 controller 视角的优先队列平均占用。 |
| `active_q_avg_per_ctrl` | `active_queue_len_sum / aggregate_ctrl_cycles` | 单 controller 视角的活动队列平均占用。 |
| `read_bytes_per_cycle` | `read_bytes / cycles` | 每周期完成的读 payload 字节数。 |
| `write_bytes_per_cycle` | `write_bytes / cycles` | 每周期完成的写 payload 字节数。 |
| `total_bytes_per_cycle` | `(read_bytes + write_bytes) / cycles` | 每周期完成的总 payload 字节数。 |

修改建议：

- 新增字段时保持 `name : value` 对齐输出，方便人工阅读和脚本解析。
- system-level 与 controller-aggregate-level 指标要分开命名。
- 不要在多个模块重复计算同一个统计口径。

## 新增统计字段流程

1. 在 `include/hbm_sim/stats/stats.hpp` 增加字段，并给出安全默认值。
2. 在真正产生事件的模块中更新字段，例如 controller、executor、refresh manager 或 memory system。
3. 在 `src/stats/stats.cpp` 中按稳定顺序输出字段。
4. 如果字段会被回归依赖，在 `tests/smoke.sh` 中检查它。
5. 如果字段适合作为 golden 指标，在 `tools/compare_stats.py` 的使用说明或测试中加入它。

## 口径提醒

- `cycles` 和 `system_cycles` 适合看系统运行时间。
- `aggregate_ctrl_cycles` 适合做多 controller 平均队列长度。
- `read_bytes/write_bytes` 是 payload。
- `interface_read_bytes/interface_write_bytes` 是接口记账量；当前不反向延长总线占用。
- `achieved_bw_GBps` 看有效载荷带宽。
- `achieved_if_bw_GBps` 看包含协议 bit 的等效需求带宽，不能单独用于判断物理链路饱和。
- `payload_efficiency_pct` 用于观察 ECC/CRC/DBI/metadata 等开销。
- `data_mismatches == 0` 是 payload 正确性的基本通过条件。
- `mismatch_report.txt` 用于定位具体 expected/actual 差异；统计字段只给出数量。
- `ecc_*` 字段描述当前 cache-line SECDED shadow，不等价于厂商级完整 RAS 流程。
