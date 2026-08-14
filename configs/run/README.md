# 运行配置与字段说明

本文覆盖 `configs/run/` 下六个主 `.cfg`。`run` 文件是可直接执行的实验入口，
重点是复现 workload 和功能组合；标准数值审计应回到 `configs/calib/` 和
`configs/profiles/`。

标准依据为 HBM4 JESD270-4A 与 LPDDR6 JESD209-6。标准正文不随仓库分发；
可公开复核的字段来源和仍待厂商校准的边界见[审计](../../文档/审计.md)。

## 1. 继承关系

大多数 run 文件只覆盖实验相关字段，未写出的组织和 timing 来自：

```text
standard
  -> make_spec_draft(name)
  -> StandardTraits 身份/能力/profile 选择
  -> 内置 timing profile
  -> timing_profile_file
  -> run/*.cfg 显式覆盖
  -> CLI 覆盖
  -> finalize_spec() 生成 constraints/table
```

因此不能只看 run 文件的一小段就判断最终数值。运行时应同时查看 CLI 输出中的
`timing_profile`、`timing_source_*`、`line_size`、`dram_transaction_bytes`、
`memory_capacity_bytes` 和 `hit_cycle_limit`。

## 2. `hbm3.cfg`

```bash
./build/hbm_sim --config configs/run/hbm3.cfg
```

| 项目 | 当前值 | 作用 |
|---|---|---|
| profile | `ramulator2_6400_16gb_8hi.cfg` | HBM3 研究基线 |
| workload | random、10000、75% read、seed 11 | 增加 row conflict 和 bank 分散 |
| queue | 128/128/2048 | 每 channel controller 缓冲 |
| refresh/RFM | per-bank | 使用 REFpb/RFMpb 路径 |
| max cycle | 100M | 防止高压运行无限等待 |

最终组织、64B transaction 和 timing 均来自 HBM3 profile。没有 HBM3 标准/厂商
表支撑时，只适合与同项目 HBM4 进行架构趋势对比。

## 3. `hbm4.cfg`

这是默认 HBM4 full-stack 快速实验：

```bash
./build/hbm_sim --config configs/run/hbm4.cfg
```

| 参数组 | 当前值 | 解释 |
|---|---|---|
| profile | 8000Mbps、32Gb、8Hi | JESD270-4A-oriented |
| workload | stream、10000、90% read、64B stride | 默认 host line 压力 |
| transaction | 32B | 每个 64B host 请求拆成两个 HBM transaction |
| stack | 32 channel、2048 data bit | profile 提供 full-stack 口径 |
| ECC metadata | 16bit/32B | 每 PC 2 ECC pin×BL8 |
| RFM | per-bank、threshold/decrement 256 | 策略有效，阈值仍为研究默认 |
| CRC/retry | off | 不计链路保护 |

理论峰值：

```text
8000 Mbps/pin × 2048 bit / 8000 = 2048 GB/s
```

`achieved_bw_GBps` 使用完成的 payload；`achieved_if_bw_GBps` 还包含配置的
metadata/ECC。外部时间必须使用 `system_cycles`，不能使用所有 controller 相加的
`aggregate_ctrl_cycles`。

## 3.1 `hbm4_6stack.cfg`

这是主动 6-stack 系统入口，不是只保存六份 payload 的被动数组：

```bash
./build/hbm_sim --config configs/run/hbm4_6stack.cfg
```

每颗 stack 使用完整 HBM4 profile、32 个 channel-local Controller 和独立
`MemoryImage`，因此共创建 192 个 controller。系统地址先按 256B 条带选择 stack，
再用 stack-local 地址选择 channel/PC/SID/bank。每颗 stack 有 256-entry ingress，
每拍最多分发 4 个请求，并使用数字越大优先级越高的 strict-priority QoS；同优先级
保持 FCFS。

输出中的 `stack_N_*` 是 per-stack 完成量、带宽、平均读延迟、功耗和温度；
`peak_bandwidth_GBps` 与 `aggregate_capacity_bytes` 是 6-stack 聚合口径。
command/DFI trace 同时记录 `stack_id`、stack-local `address` 和全局
`system_address`。详细语义见 [多 Stack 系统模型](../../文档/多Stack及存储后端.md)。

## 4. `lpddr5.cfg`

| 项目 | 当前值 | 解释 |
|---|---|---|
| profile | Ramulator2.1-derived 6400/16Gb | LPDDR6 对照 |
| workload | stream、100% read、64B stride | 读带宽基线 |
| WCK | `cas_sync` | 通过项目 CAS_RD/CAS_WR 路径 |
| DVFS | nominal | run 文件覆盖 calib 中的 disabled，但 profile 仍是研究基线 |
| link/metadata | off/0 | 不计 Link ECC/DBI |
| low power | off | 不进入 PDE/SREF |

该文件不是 LPDDR5 器件校准配置。

## 5. `lpddr6.cfg`

```bash
./build/hbm_sim --config configs/run/lpddr6.cfg
```

加载 nominal 10667Mbps、16Gb、DVFSL、link protection off、BL24 profile。

| 参数 | 当前值 | 含义 |
|---|---|---|
| `lpddr_efficiency_mode` | normal | 两个 subchannel 正常模式 |
| `lpddr_dvfs_mode` | nominal | 高速 DVFSL 分支 |
| `lpddr_wck_mode` | cas_sync | 按需建立 WCK2CK sync |
| `link_protection/DBI/Link ECC` | false | 无保护基线 |
| refresh/RFM | per-bank | REFdb/PRAC-RFM 项目路径 |

JESD209-6 的 x12、BL24 是 32B data access。本配置现已固定为
`rows=65536`、`columns=64`、`dram_transaction_bytes=32` 和
`prefetch_size=24`；64B `line_size` 会拆成两个 32B transaction。两个
16Gb subchannel 的项目逻辑容量为 4GiB。RL/WL、训练、链路保护和功耗仍需
目标 vendor/profile 校准，因此组织与 payload 对齐不等于 pin-level 器件已校准。

## 6. `hbm4_storage.cfg`

该配置展示 `MemoryImage` 的真实 payload 路径：

```text
examples/memory_image.txt
  -> 初始 payload/init mask
examples/data_check.trace
  -> W/R/expect/mask 请求
Controller
  -> ACT/RD/WR/PRE
MemoryImage
  -> row buffer、backend、ECC shadow、功耗/热事件
```

```bash
./build/hbm_sim --config configs/run/hbm4_storage.cfg
```

主要输出：

| 文件 | 内容 |
|---|---|
| `outputs/hbm_sim_final_memory.txt` | 最终 payload、init mask、writer/version、物理坐标 |
| `outputs/hbm_sim_mismatch_report.txt` | expected/actual、地址和最后写者 |
| `outputs/hbm_sim_thermal_map.txt` | 访问过的稀疏热 tile/grid |

未显式配置 backend 时使用 `sparse`。`requests=0` 表示读取完整 trace。

## 7. mmap 后端配方

在相同数据正确性语义下，把 payload 后端换成 `mmap_sparse`：

```bash
rm -f outputs/hbm_sim_hbm4_mmap.bin{,.init,.meta,.present}
./build-clang-debug/hbm_sim --config configs/run/hbm4_storage.cfg \
  --memory-backend mmap_sparse --memory-capacity-bytes 1073741824 \
  --memory-data-file outputs/hbm4_mmap.bin
```

| 参数 | 作用 |
|---|---|
| `memory_capacity_bytes=0` | 自动使用 HBM4 spec 容量，当前为 32GiB |
| `memory_data_file` | payload sparse file |
| `.init` | 逐 byte 初始化 bitmap |
| `.meta` | header 和 line metadata |
| `.present` | allocated/written bitmap |
| `topology_stats_scan_limit` | 限制详细坐标扫描成本 |

当前 HBM4 backend block 为 32B。旧的 64B 文件头不兼容，必须换路径或删除四个
sidecar 后重建。

## 8. chunk-file 后端配方

与 mmap 配置的协议行为相同，但后端使用固定数量的 chunk cache：

| 参数 | 当前值 | 作用 |
|---|---:|---|
| `memory_chunk_size` | 2MiB | 每次载入/写回的文件块 |
| `memory_chunk_cache_entries` | 16 | 最多驻留 16 个 chunk |
| 理论主要 cache 数据量 | 32MiB | 不含 metadata 和容器开销 |

```bash
rm -f outputs/hbm_sim_hbm4_chunk.bin{,.init,.meta,.present}
./build-clang-debug/hbm_sim --config configs/run/hbm4_storage.cfg \
  --memory-backend chunk_file --memory-capacity-bytes 1073741824 \
  --memory-data-file outputs/hbm4_chunk.bin \
  --memory-chunk-size 2097152 --memory-chunk-cache-entries 16
```

`chunk_file` 更适合满容量覆盖、长时间运行和虚拟地址空间受限环境。

## 9. 三种存储运行配置的公平比较

必须固定以下条件：

```text
相同 standard/profile
相同 trace 和 requests
相同 inject_interval/max_cycles
相同初始 memory image
相同 power/thermal/ECC 选项
全新的后端文件
```

三种后端只改变宿主机保存方式，不应改变 DRAM 命令和完成 payload。比较结果至少
记录：

```text
system_cycles
completed_reads/writes
read_bytes/write_bytes
data_mismatches
storage_lines_allocated
unique_written_lines
宿主 RSS、磁盘实际占用和运行时间
```

## 10. DRAMsim3 IDD 与细分热网格配方

该文件验证 DRAMsim3-style 功耗公式和细分热网格：

```bash
./build-clang-debug/hbm_sim --config configs/run/hbm4_storage.cfg \
  --power-source dramsim3_idd \
  --thermal-grid-cols-per-tile 4 --thermal-grid-rows-per-tile 4
```

| 参数组 | 当前设定 | 来源/边界 |
|---|---|---|
| power source | `dramsim3_idd` | 项目借鉴 DRAMsim3 的计算路径 |
| VDD/IDD | 1.2V、IDD0/2N/3N/4R/4W/5AB/5PB/6X | 来自 DRAMsim3 HBM2 示例，不是 HBM4 vendor 表 |
| thermal grid | 每 tile 4×4 | 项目稀疏 RC 网格 |
| chip size | 10mm×10mm | research default |
| TSV radius | 5um | research default |

简化能量路径使用类似：

```text
energy ~ VDD × current_delta × command_time × devices_per_rank
```

它可验证命令分类、能量累计和热事件传播，不能给出 HBM4 绝对功耗或封装温度结论。

## 11. 输出有效性检查

每次运行至少检查：

```text
hit_cycle_limit=false
remaining_requests=0
remaining_pending=0
data_mismatches=0            # 数据正确性实验
cmd_validation=pass          # 开启 validator 时
dfi_validation=pass          # 开启 DFI validator 时
```

推荐保存完整命令：

```bash
./build/hbm_sim --config configs/run/hbm4.cfg \
  --cmd-trace outputs/hbm4_commands.csv \
  --validate-cmd-trace
```

完整 command/DFI trace 会随请求数增长，只应在代表性调试窗口启用。
