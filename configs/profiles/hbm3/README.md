# HBM3 profile 说明

本文说明本目录中的 HBM3 timing profile。它是被 `configs/calib/*.cfg` 或
`configs/run/*.cfg` 通过 `timing_profile_file` 引用的配置片段，不建议脱离主配置
单独运行。

## 1. 来源边界

本目录没有配套的 HBM3 JEDEC 原文或具体厂商 speed-bin 手册。因此
`ramulator2_6400_16gb_8hi.cfg` 的全部时序以 `source = research_default` 标记：

- 命令、层级和时序字段参考项目 HBM3 preset 与 Ramulator2.1 HBM3 模型。
- 它适合做 HBM3/HBM4 控制器算法对照和回归测试。
- 它不能证明某个真实 6400 Mb/s、16 Gb、8Hi HBM3 器件的绝对延迟或带宽。
- 发布器件级结论前，需要用目标 HBM3 JEDEC 表和厂商 datasheet 重新校准。

## 2. `ramulator2_6400_16gb_8hi.cfg`

### 2.1 身份与工作点

| 参数 | 当前值 | 含义与来源 |
|---|---:|---|
| `source` | `research_default` | 后续 timing 项均记为研究默认值 |
| `timing_profile` | `hbm3_ramulator2_6400_16gb_8hi` | profile 身份标签，只用于输出和追踪 |
| `vendor_profile` | `ramulator2_baseline` | 表示第三方研究模型基线，不是真实 vendor |
| `mode_profile` | `default` | 项目默认 HBM3 工作模式 |
| `speed_bin_mbps` / `data_rate_mbps` | `6400` | 每 pin 有效数据率 6400 Mb/s |
| `density_gb` | `16` | profile 标签中的单 die 密度 |
| `stack_height` | `8` | 8Hi 堆叠标签 |
| `tCK_ps` | `625` | 控制器 CK 周期；项目按 DDR 等效关系采用 625 ps |
| `tick_multiplier` | `2` | 一个 nCK timing 在内部换算为两个 simulator tick |
| `data_bus_bits` | `1024` | 16 个 HBM channel 合计的项目接口位宽 |

`speed_bin_mbps` 是器件标签，真正参与 ns 到 nCK 换算的是 `tCK_ps`。

### 2.2 组织结构

| 参数 | 当前值 | 作用 |
|---|---:|---|
| `channels` | `16` | 完整堆叠的 channel 数 |
| `pseudo_channels` | `2` | 每 channel 两个 pseudo-channel |
| `sids` / `ranks` | `1` / `1` | 当前地址层级只建一个 SID 和 rank |
| `bank_groups` | `4` | 每 pseudo-channel 的 bank group 数 |
| `banks_per_group` | `4` | 每 bank group 的 bank 数 |
| `rows` / `columns` | `32768` / `32` | 每 bank 的逻辑 row/column 数 |
| `line_size` | `64 B` | 项目 host 逻辑数据块和默认事务粒度 |

当前几何计算的逻辑容量是：

```text
16 × 2 × 1 × 1 × 4 × 4 × 32768 × 32 × 64 B = 32 GiB
```

该结果与 `16 Gb/die × 8Hi = 16 GiB/stack` 的标签并不一致，说明这里的
`line_size = 64 B` 和组织参数是项目/Ramulator 风格研究抽象，不应解释为已恢复
真实 HBM3 芯片容量。若以后做物理容量校准，应同时核对 PC burst payload、
column 定义、SID/stack-height 映射和 `dram_transaction_bytes`，不能只修改
`density_gb` 标签。

### 2.3 HBM 专用行为

| 参数 | 含义 |
|---|---|
| `hbm_edge_pairing = true` | 启用 HBM 命令边沿配对约束 |
| `hbm_edge_pairing_matrix = hbm3_row_col_pre_pairing` | 选择项目内 HBM3 行、列、PRE 配对矩阵 |
| `hbm_sid_mapping = single_sid` | 地址映射仅使用一个 SID |
| `hbm_ecc_scheme = none` | 不计 HBM ECC metadata |
| `hbm_ras_policy = counter_only` | RAS 只做事件计数，不模拟真实恢复链路 |

这些字符串用于选择或记录项目模型模式，不等于 JEDEC mode register 的逐位镜像。

### 2.4 核心 timing

所有 `n*` 值单位为 CK：

| 参数 | 当前值 | 控制的命令间隔 |
|---|---:|---|
| `nBL` | 2 | 一次 burst 占用的 CK 数 |
| `nCL` / `nCWL` | 26 / 8 | RD/WR 到数据阶段的读/写延迟 |
| `nRCDRD` / `nRCDWR` | 26 / 14 | ACT 到 RD/WR |
| `nRP` | 26 | PRE 到下一次 ACT |
| `nRAS` / `nRC` | 48 / 74 | ACT 最短保持时间与同 bank ACT 周期 |
| `nRTP` / `nWR` | 6 / 24 | RD/WR 后到 PRE 的恢复约束 |
| `nCCDS` / `nCCDL` / `nCCDR` | 2 / 4 / 3 | 不同 bank-group/同组/配对列命令间隔 |
| `nRRDS` / `nRRDL` / `nFAW` | 8 / 8 / 24 | ACT 密度限制 |
| `nRTW` | 18 | 读转写 |
| `nWTRS` / `nWTRL` | 8 / 12 | 写转读短/长路径 |
| `nMRW` / `nMRR` | 8 / 8 | mode-register 操作延迟 |

`nECCSCRUB`、`nRASERR` 和 `nLINKRETRY` 为 `0`，表示该 HBM3 基线未启用对应
维护/RAS/链路恢复延迟。

### 2.5 刷新 timing

| 参数 | 当前值 | 作用 |
|---|---:|---|
| `tRFCab_ns` | 350 ns | all-bank refresh recovery |
| `tRFCpb_ns` | 200 ns | per-bank refresh recovery |
| `tRREFD_ns` | 8 ns | 相邻 refresh 的额外间隔 |
| `tREFI_us` | 3.9 us | all-bank refresh 平均间隔 |
| `tREFIpb_ns` | 121.875 ns | per-bank refresh 调度间隔 |

加载 profile 时，时间值按 `ceil(time / tCK)` 换算为 nCK。这里虽然使用
JEDEC 风格字段名和单位，但数值来源仍是 `research_default`。

## 3. 使用方式

```bash
./build/hbm_sim --config configs/calib/hbm3.cfg \
  --dump-timing-table outputs/hbm3_timing.csv
```

输出中的 `timing_source_research_default` 应当被保留。对该 profile 使用
`--strict-timing-table` 失败是合理的：它是在提醒这些 vendor-required 时序尚未
由真实器件表替换。
