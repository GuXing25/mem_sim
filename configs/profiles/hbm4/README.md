# HBM4 profile 说明

本文说明本目录四份 HBM4 profile。标准依据为用户提供的
`HBM4 JESD270-4A_dual_Kimi+DeepSeek.pdf`。页码以下优先写 JEDEC 文档正文页码；
双语 PDF 的实际 PDF 页可能有偏移。

## 1. 标准共同基础

JESD270-4A 给出的关键结构是：

- 每 channel 有两个半独立 pseudo-channel（PC）。
- 每 PC 为 32 DQ，采用 256-bit prefetch；BL8 一次传输
  `32 DQ × 8 = 256 bit = 32 B`。
- 32 Gb、8Hi 组织表给出 14 个 row address、5 个 column address 和一个 SID
  地址位，并给出 1 KB page。
- 8.0 Gb/s 工作点对应 `fCK = 2000 MHz`、`tCK = 0.500 ns`。
- `tCCDS = 2 nCK`；`tCCDL` 按 `max(4 nCK, 2.5 ns/tCK)` 一类规则约束；
  `tCCDR` 和部分 RL/WL/行时序存在器件或模式依赖。
- 32 Gb、8Hi 条件下，标准表可见 `tRFCab = 450 ns`、
  `tRFCpb = 280 ns`、`tREFI = 3.9 us` 和每 bank 刷新间隔关系。
- RFM 的 RAA 门限、递减和管理策略包含 mode/vendor 相关字段，不能凭标准
  接口定义推导出真实厂商策略效果。

因此，本目录把可直接引用的标准值标为 `jedec`，把缺少目标器件手册的值标为
`research_default`；两个 “vendor” 文件只用于测试 vendor 配置通路。

## 2. profile 加载规则

profile 从上到下解析。每次出现：

```ini
source = jedec
```

或：

```ini
source = research_default
```

都会改变其后 timing 项的 provenance。纳秒/微秒项根据当时的 `tCK_ps` 向上
取整为 nCK，并在 timing 表中保留来源和原始说明。

## 3. `jedec_8000_32gb_8hi.cfg`

这是最接近 JESD270-4A 公开标准的 HBM4 profile，但它仍不是任何厂商产品表。

### 3.1 工作点与组织

| 参数 | 当前值 | 解释 |
|---|---:|---|
| `speed_bin_mbps` | 8000 | 每 pin 8.0 Gb/s |
| `density_gb` / `stack_height` | 32 / 8 | 32 Gb die、8Hi |
| `tCK_ps` | 500 | 标准 8.0 Gb/s 工作点的 CK 周期 |
| `channels` | 32 | 完整 HBM4 stack 的 channel 数 |
| `pseudo_channels` | 2 | 每 channel 两个 PC |
| `sids` | 2 | 当前项目将 8Hi 映射为两个 SID slice |
| `bank_groups` / `banks_per_group` | 2 / 8 | 每 PC 共 16 bank |
| `data_bus_bits` | 2048 | `32 channel × 2 PC × 32 DQ` |
| `dram_transaction_bytes` | 32 | 一条 PC RD/WR 的 BL8 payload |
| `tick_multiplier` | 2 | nCK 到内部 tick 的比例 |

主配置补充 `rows = 16384`、`columns = 32`、`line_size = 64` 后，逻辑容量为：

```text
32 × 2 × 2 × 1 × 2 × 8 × 16384 × 32 × 32 B = 32 GiB
```

其中容量乘数使用 `dram_transaction_bytes = 32 B`；`line_size = 64 B` 是 host
逻辑请求粒度，一条 64 B 请求会拆为两个 32 B HBM 事务。

### 3.2 标准来源字段

| 参数 | 当前值 | 来源/作用 |
|---|---:|---|
| `nBL` | 2 | 项目 CK 视图中的 BL8 burst 占用 |
| `nCCDS` | 2 | 标准短列命令间隔 |
| `tCCDL_ns` | 2.5 ns | 标准同 bank-group 长列间隔的时间下限 |
| `nCCDR` | 3 | 项目对 paired-edge 列间隔的标准导向取值，器件发表前仍需复核模式 |
| `tRFCab_ns` | 450 ns | 32 Gb、8Hi all-bank refresh |
| `tRFCpb_ns` | 280 ns | 32 Gb per-bank refresh |
| `tRREFD_ns` | 8 ns | refresh-to-refresh 时间下限 |
| `tREFI_us` | 3.9 us | 平均刷新间隔 |
| `tREFIpb_ns` | 121.875 ns | `3.9 us / 32` |

HBM edge pairing、PC/SID interleave、32-channel stack 和 metadata
`16 bit/32 B transaction` 是按 JESD270-4A 接口组织建立的项目控制器抽象。
metadata 不能等同于 vendor 未公开的片上 ECC check-bit 布局。

### 3.3 研究默认字段

第二个 `source = research_default` 之后的 `nCL`、`nCWL`、RCD、RP、RAS、RC、
RTP、WR、RRD、FAW、RTW、WTR、MRW/MRR 和 `nECCSCRUB` 都是占位研究值。
它们保证模型可运行和时序约束闭合，但没有真实厂商 speed-bin 表支撑。

这正是该文件中两段 `source` 的原因：不能因为同在一个 profile 中，就把后半段
也称为 JEDEC 已确定值。

## 4. `ramulator2_8000_32gb_8hi.cfg`

这是 Ramulator2.1 差分基线，全部标为 `research_default`。

| 参数组 | 当前设置 | 用途 |
|---|---|---|
| 工作点 | 8000 Mb/s、32 Gb、8Hi、`tCK=500 ps` | 与 native HBM4 名义工作点对齐 |
| 访问延迟 | `nCL=20`、`nCWL=10`、`nRCDRD=39`、`nRCDWR=19` | 复现参考模型 timing |
| 行时序 | `nRP=33`、`nRAS=57`、`nRC=90` | 受控差分使用 |
| 列/转向 | `nCCDS/L/R=2/4/2`、`nRTW=25`、`nWTRS/L=9/13` | 比较调度与命令序列 |
| 刷新 | `nRFC=900`、`nRFCpb=560`、`nREFI=7800` | 已经是 nCK，不再做 ns 换算 |
| RFM | `nRFMab=900`、`nRFMpb=560` | 参考模型研究参数 |

它的价值是让两个模拟器在同一组织、请求和调度条件下比较命令/周期趋势，不是
用 Ramulator 替代 JEDEC 或厂商实测。

## 5. `synthetic_vendor_9000_48gb_16hi.cfg`

这是自动测试用的“合成厂商式”完整 profile：

- `source = vendor` 是为了让 `--strict-timing-table` 验证 vendor 数据通路。
- `vendor_profile = synthetic_vendor` 和文件注释明确说明其为虚构数据。
- 9000 Mb/s、48 Gb、16Hi、CRC16、RAS sideband、link retry、ECC scrub 等值
  都不能作为真实产品参数引用。
- 它适合检查 parser、provenance、严格审计、命令 trace、ECC/RAS/CRC 开销和
  维护命令是否连通。

当前组织对应：

```text
32 × 2 × 4 × 1 × 2 × 8 × 16384 × 32 × 32 B = 64 GiB
```

而 `48 Gb × 16Hi` 标签意味着另一种目标容量口径。两者不一致，进一步说明本
profile 是配置路径测试材料；在真实 48 Gb/16Hi 器件配置中必须按 datasheet
重建 row/column/SID 组织。

## 6. 使用与审计

查看混合来源 profile：

```bash
./build/hbm_sim --config configs/calib/hbm4.cfg \
  --dump-timing-table outputs/hbm4_jedec_timing.csv
```

验证合成 vendor 通路：

```bash
./build-clang-debug/hbm_sim --standard hbm4 --requests 32 \
  --timing-profile-file configs/profiles/hbm4/synthetic_vendor_9000_48gb_16hi.cfg \
  --strict-timing-table \
  --dump-timing-table outputs/hbm4_synthetic_timing.csv
```

解读原则：

- `jedec` 表示标准可见条件下的值。
- `derived` 表示项目按标准公式换算。
- `research_default` 表示缺少目标 vendor 数据的替代值。
- `vendor` 只有在绑定真实器件和可追踪表号后才支持厂商级结论；当前 synthetic
  文件不满足这一条件。真实用户 profile 应从它复制到新文件后逐项替换并写明表号。
