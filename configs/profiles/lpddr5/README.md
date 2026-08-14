# LPDDR5 profile 说明

本目录只有 `ramulator2_6400_16gb.cfg`。它是 LPDDR6 研究的 LPDDR5 对照组，
没有配套 LPDDR5 JEDEC 原文和真实厂商 datasheet，因此全部标记为
`source = research_default`。

## 1. 模型用途与边界

该 profile 保留了项目/Ramulator 风格的 LPDDR5 命令和层级：

- ACT1/ACT2 分裂激活。
- RD/WR 前的 CAS_RD/CAS_WR 同步。
- rank、bank group、bank、row、column 地址层级。
- per-bank 和 all-bank refresh。

它可以验证控制器命令流、调度策略和 LPDDR5/LPDDR6 相对趋势，不能宣称某个
6400 Mb/s、16 Gb LPDDR5 器件的绝对 timing 已通过标准或实物验证。

## 2. 身份与接口

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `timing_profile` | `lpddr5_ramulator2_6400_16gb` | profile 身份 |
| `vendor_profile` | `ramulator2_baseline` | 第三方模型基线标签，不是厂商 |
| `speed_bin_mbps` / `data_rate_mbps` | 6400 | 每 pin 有效数据率 |
| `density_gb` | 16 | 名义器件密度标签 |
| `data_bus_bits` | 16 | 项目中的数据接口位宽 |
| `prefetch_size` | 16 | 项目 LPDDR5 core prefetch 参数 |
| `tCK_ps` | 1250 | CK 周期 |
| `tick_multiplier` | 1 | 一个 CK 对应一个 simulator tick |

## 3. 组织与容量

```text
channel=1, pseudo-channel=1, rank=1
4 bank groups × 4 banks
32768 rows × 1024 columns
line_size=64 B
```

按项目地址公式得到：

```text
1 × 1 × 1 × 1 × 4 × 4 × 32768 × 1024 × 64 B = 32 GiB
```

它与 `density_gb = 16 Gb` 的 2 GiB 容量标签明显不同。原因是当前
`column × line_size` 是项目逻辑存储粒度，而非从 LPDDR5 x16 burst 和标准列
地址严格反推的物理组织。因此 `density_gb` 在此主要是 profile 身份信息，
不能用当前几何宣称真实器件容量。

## 4. LPDDR 模式字段

| 参数 | 当前值 | 含义 |
|---|---|---|
| `lpddr_link_protection` | false | 不模拟链路保护开销 |
| `lpddr_wck_training_mode` | `cas_sync_only` | 仅保留 CAS 同步式 WCK 抽象 |
| `lpddr_dvfs_transition_policy` | `disabled` | 不插入 DVFS 转换 |
| `lpddr_low_power_state_policy` | `controller_idle` | 由控制器空闲策略进入低功耗 |
| `lpddr_wck_training_required` | false | 不要求额外训练命令 |
| `lpddr_dbi_enabled` / `lpddr_link_ecc_enabled` | false | 不计 DBI/link ECC |
| `lpddr_ca_parity_enabled` | false | 不启用 CA parity |

`lpddr_ca_parity_bits_per_command = 1` 只是启用时的项目计数粒度；由于开关为
false，当前运行不会产生该项开销。

## 5. timing 参数

| 参数组 | 当前值 | 作用 |
|---|---|---|
| burst/RL/WL | `nBL=2`, `nCL=17`, `nCWL=9` | 数据 burst 与读写延迟 |
| ACT 到列命令 | `nRCDRD=15`, `nRCDWR=15` | ACT 后最早 RD/WR |
| PRE/row | `nRP=15`, `nRPab=17`, `nRAS=34`, `nRC=49` | 行开关约束 |
| 恢复 | `nRTP=8`, `nWR=28` | RD/WR 到 PRE |
| 列间隔 | `nCCDS=2`, `nCCDL=4` | 列命令间隔 |
| ACT 密度 | `nRRDS=4`, `nRRDL=4`, `nFAW=16` | 多 bank ACT 限制 |
| LPDDR 同步 | `nAAD=8`, `nWCK2CK=1`, `nWCKPST=8` | 分裂 ACT 与 WCK 相关延迟 |
| 总线转向 | `nWTRS=5`, `nWTRL=10` | 写转读 |
| 刷新 | `nRFC=224`, `nRFCpb=128`, `nREFI=3125`, `nREFIpb=390` | all/per-bank refresh |

`nWCKTRAIN`、`nDVFS`、`nECCSCRUB`、`nRASERR`、`nLINKRETRY` 为 0，表示这些
扩展流程没有加入该对照配置。

## 6. 使用

```bash
./build/hbm_sim --config configs/calib/lpddr5.cfg \
  --dump-timing-table outputs/lpddr5_timing.csv
```

输出应继续显示 vendor-required 项来自 `research_default`。未来引入 LPDDR5
标准或目标器件表时，应新增独立 JEDEC/vendor profile，不要直接把本文件改名为
“已校准”。
