# 校准配置与字段说明

本文集中说明 `configs/calib/` 下的全部 `.cfg`。这里的文件用于把 preset 和
profile 显式展开，方便逐项替换为目标器件数据；它们不是同名标准或器件的合规
证明。

参考依据为 HBM4 JESD270-4A 与 LPDDR6 JESD209-6；标准正文不随仓库分发。
字段来源和校准缺口见[审计](../../文档/审计.md)。HBM3、LPDDR5 当前没有随项目
提供对应 JEDEC 文档，因此只按
  Ramulator2.1-derived 研究基线解释。

## 1. 加载顺序

```text
calib/*.cfg
  -> standard 调用 make_spec_draft(name)
  -> StandardTraits 选择标准身份/能力/profile
  -> timing_profile/speed/density/stack 展开内置 profile
  -> timing_profile_file 加载外部片段
  -> calib 文件中的显式字段覆盖前述结果
  -> *_ns/*_us 按最终 tCK_ps 向上换算为 nCK
  -> finalize_spec() 生成 timing constraints/table
```

主要实现：

- [CLI 配置解析](../../src/cli/main.cpp)
- [profile 加载与来源标记](../../src/dram/profiles.cpp)
- [标准 traits 目录](../../src/dram/standard_traits.cpp)
- [统一 finalize 与 timing table](../../src/dram/spec.cpp)
- [控制器时序表](../../src/controller/timing.cpp)

来源标签必须按下面理解：

| 标签 | 含义 | 可以证明什么 |
|---|---|---|
| `jedec` | 标准明确给出的数值、范围或公式 | 对应标准约束已进入模型 |
| `derived` | 由标准值和 `tCK` 等公式换算 | 换算过程可复核 |
| `research_default` | 为使模型可运行而选取 | 只能用于架构、趋势和敏感性研究 |
| `vendor` | 声明来自目标器件资料 | 只有真实资料和出处齐全时才能作器件声明 |
| `synthetic_vendor` | 模拟厂商配置工作流 | 不能当作真实厂商数据 |

## 2. 通用运行字段

| 参数 | 作用 | 来源性质 |
|---|---|---|
| `standard` | 选择命令集、状态机和 preset | 项目入口 |
| `timing_profile` | 输出中的配置身份 | 项目元数据 |
| `timing_profile_file` | 加载目录外的 profile 片段 | 项目机制 |
| `vendor_profile` | 记录是否绑定 vendor | 审计元数据 |
| `mode_profile` | 记录 MR、保护、DVFS、突发组合 | 项目模式标签 |
| `pattern/requests/read_ratio/seed` | 生成 workload | 非 JEDEC |
| `inject_interval` | 相邻 host 请求的目标注入 tick | 非 JEDEC |
| `address_mapping` | byte 地址到 DRAM 层级的排列 | 项目策略 |
| `read/write/priority_buffer_size` | 控制器队列容量 | 控制器设计 |
| `max_cycles` | 仿真终止上限 | 实验保护 |

`line_size` 是 frontend 默认请求粒度。`dram_transaction_bytes` 是一条 DRAM
RD/WR 的默认 payload 粒度；两者不能再混为同一个概念。

## 3. `hbm3.cfg`

入口：

```bash
./build/hbm_sim --config configs/calib/hbm3.cfg
```

加载 [HBM3 profile](../profiles/hbm3/ramulator2_6400_16gb_8hi.cfg)，目的是给
HBM4 实验提供同平台对照。

| 参数组 | 当前设定 | 解释 |
|---|---|---|
| 身份 | HBM3、6400Mbps、16Gb、8Hi | Ramulator2.1-derived 标签 |
| 流量 | random、10000、75% read、seed 11 | 制造 bank/row 分散压力 |
| 组织 | 16 channel、2 PC、1 SID、4 BG×4 bank | 项目 HBM3 full-stack 基线 |
| 地址 | 32768 row、32 column、64B line | 研究组织，不是本项目已核验的 HBM3 JEDEC 表 |
| 接口 | 1024 bit、`tCK=625ps`、2 tick/nCK | 峰值口径和半周期调度 |
| 维护 | REFpb、RFMpb、阈值 256 | RFM 阈值是研究默认 |
| 可靠性 | ECC/CRC/retry 关闭 | 未声称 HBM3 RAS 完整性 |

此文件本身不逐项写 timing，而由 profile 提供。即使命令序列测试通过，也只能
说明项目内部 HBM3 路径可运行，不能说明某颗 HBM3 器件的 RL/WL、refresh 或
功耗准确。

## 4. `hbm4.cfg`

这是 JESD270-4A-oriented 的 8.0Gbps、32Gb/die、8Hi 校准模板。

### 4.1 组织和接口

| 参数 | 当前值 | 标准依据或项目解释 |
|---|---:|---|
| `channels` | 32 | JESD270-4A 概述允许完整 stack 最多 32 channel；本配置选 full-stack |
| `pseudo_channels` | 2 | 标准 3.1.2：每 channel 两个 32-DQ PC |
| `sids` | 2 | 8Hi 的 `SID[0]` 两个有效值，来自表 4 |
| `bank_groups/banks_per_group` | 2/8 | 每 SID 16 bank；表 5 的 32-bank/PC 组织在项目中的分解 |
| `rows` | 16384 | 表 4 的 `RA[13:0]` |
| `columns` | 32 | 表 4 的 `CA[4:0]` |
| `line_size` | 64B | host/cache-line 请求，项目选择 |
| `dram_transaction_bytes` | 32B | 每 PC `32 DQ × BL8 = 256bit` |
| `data_bus_bits` | 2048 | `32 channel × 64 DQ` 的 stack 统计宽度 |
| `tCK_ps` | 500 | 表 107 的 8.0Gbps/pin、CK 2GHz 档 |
| `tick_multiplier` | 2 | 一个 nCK 拆为上升/下降沿两个 simulator tick |

容量按物理 transaction 计算：

```text
32 ch × 2 PC × 2 SID × 1 rank × 2 BG × 8 bank
× 16384 row × 32 column × 32B = 32 GiB
```

一个默认 64B host 请求由 frontend 拆成两个 32B HBM transaction。

### 4.2 协议和可靠性

| 参数 | 含义 | 边界 |
|---|---|---|
| `hbm_edge_pairing` | 启用行/列双命令接口和 PRE 边沿配对 | 表 35 提供规则 |
| `hbm_strict_edge_pairing` | 控制器和 validator 拒绝非法组合 | 项目检查策略 |
| `hbm_sid_interleave` | SID 参与地址和 `tCCDR` 作用域 | 标准有 SID；具体映射是项目策略 |
| `hbm_pc_interleave` | 声明可利用两个 PC | 尚非完整 PHY 并行模型 |
| `refresh_policy/rfm_policy` | 使用 REFpb/RFMpb | 标准命令 + 控制器选择 |
| `rfm_act_threshold/decrement=256` | RAA/RFM 触发入口 | JESD270-4A 规定真实 RAAIMT/RAAMMT/RAADEC 由 vendor DEVICE_ID WDR 给出 |
| `hbm_ecc_bits_per_request=16` | 每个 32B PC transaction 的 2 ECC pin×BL8 metadata | 外部 metadata，不是片上 ECC check bits |
| `hbm_link_crc_mode=off` | 不计 link CRC/retry | 不是目标器件能力结论 |

### 4.3 timing

| 参数组 | 当前值 | 来源 |
|---|---|---|
| RL/WL | `nCL=30`, `nCWL=10` | research default；真实支持集合由 vendor/MR 决定 |
| ACT/row | `nRCDRD=30`, `nRCDWR=16`, `nRP=30`, `nRAS=54`, `nRC=84` | research default |
| recovery | `nRTP=8`, `nWR=28` | research default |
| ACT spacing | `nRRDS=8`, `nRRDL=10`, `nFAW=28` | research default |
| turn-around | `nRTW=25`, `nWTRS=9`, `nWTRL=13` | research default |
| burst/CCD | `nBL=2`, `nCCDS=2`, `nCCDL=5`, `nCCDR=3` | `nBL` 是项目 nCK 占用；CCDS/CCDL 来自表 108，CCDR 仍需 vendor |
| refresh | 450ns/280ns/8ns/3.9us/121.875ns | 表 108 的 32Gb 8Hi 分支和公式 |

换算示例：

```text
tRFCab = ceil(450ns / 0.5ns) = 900 nCK
tRFCpb = ceil(280ns / 0.5ns) = 560 nCK
tREFIpb = ceil((3.9us / 32) / 0.5ns) = 244 nCK
```

`timing_override_source=vendor` 位于文件末尾，但后面没有 timing，因此它只是为
后续追加覆盖准备，不代表前面的 research value 已变成 vendor value。

## 5. HBM4 synthetic vendor profile

`configs/profiles/hbm4/synthetic_vendor_9000_48gb_16hi.cfg` 测试“完整厂商式
profile 能否加载并通过严格检查”，不对应真实产品，也不再复制一份 calib wrapper。

| 参数组 | 当前值 | 解释 |
|---|---|---|
| 目标标签 | 9000Mbps、48Gb、16Hi | 合成场景；JESD270-4A 表 4 的公开组织不能为这组数值提供完整器件证明 |
| SID | 4 | `sid_per_4hi_slice` 项目映射 |
| CRC/RAS | CRC16、retry、16bit RAS metadata | 功能入口和接口开销测试 |
| ECC | 16bit/32B transaction | 沿用 HBM4 外部 ECC metadata 口径 |
| timing | synthetic profile 全量覆盖 | `source=vendor` 只为测试 strict 流程 |

严禁把 `synthetic_vendor` 或 profile 中的 `source=vendor` 当作厂商背书。

## 6. `lpddr5.cfg`

该文件加载 [LPDDR5 profile](../profiles/lpddr5/ramulator2_6400_16gb.cfg)。

| 参数组 | 当前设定 | 说明 |
|---|---|---|
| 身份 | LPDDR5、6400Mbps、16Gb | Ramulator2.1-derived |
| 流量 | stream、100% read、64B stride | 控制器吞吐基线 |
| 组织 | 1 channel、1 subchannel、4 BG×4 bank | 项目紧凑模型 |
| 接口 | x16、prefetch 16、`tCK=1250ps` | 研究参数 |
| WCK | `cas_sync`、ratio 4 | 项目 LPDDR5 命令路径 |
| DVFS/link | disabled/off | 对照模式 |
| refresh | per-bank | 使用项目 per-bank refresh 路径 |

项目没有随附 LPDDR5 标准或厂商表；数值结论必须在补齐对应资料后重新校准。

## 7. `lpddr6.cfg`

这是 JESD209-6-oriented 的 nominal 10667Mbps、16Gb、DVFSL enabled、
link protection off、BL24 模板。

### 7.1 标准可见结构

JESD209-6 第 2 章给出：

- 一个 x24 device 含两个 12-DQ subchannel；
- 每 subchannel 为 4 bank group×4 bank；
- BL24 的内部核心访问通常是 256bit data+32bit non-data，即 32B payload；
- WCK:CK 为 2:1，WCK 从空闲恢复时需要 WCK2CK synchronization；
- 表 2 的 16Gb/subchannel normal-mode 组织为 65536 row、64 column。

当前配置采用 `pseudo_channels=2` 表达两个 subchannel，`data_bus_bits=24` 表达
x24 device，并已经按表 2 固化以下粒度：

```text
columns = 64
dram_transaction_bytes = 32
prefetch_size = 24
line_size = 64
```

逻辑容量按 DRAM transaction 而不是 host line 计算：

```text
1 × 2 × 4 × 4 × 65536 × 64 × 32B = 4 GiB device
```

这里 `density_gb=16` 按每个 subchannel 的组织表项解释；两个 subchannel 合计
32 Gb，即 4 GiB。`line_size=64` 仅表示 frontend 默认请求大小，通用 splitter
会把它拆成两个相邻 32B LPDDR6 RD/WR 事务。

### 7.2 模式字段

| 参数 | 当前值 | 含义和来源 |
|---|---|---|
| `lpddr_dvfs_mode` | nominal | DVFSL 高速分支 |
| `lpddr_low_data_rate_mbps` | 4267 | 项目低速切换目标 |
| `lpddr_wck_mode` | cas_sync | RD/WR 前通过 CAS/WCK 路径建立同步 |
| `lpddr_wck_ratio` | 4 | 项目内部 ratio；JESD209-6 文本的 WCK:CK 物理关系需单独核对，不能直接等同 |
| `mode_register_profile` | RLSet1_WLSetA | RL/WL 集合标签 |
| `wck_training_mode` | startup_and_dvfs_retrain | 启动和 DVFS 后训练抽象 |
| `link_protection/DBI/ECC` | off | nominal 无保护分支 |
| `lpddr_dual_bank_refresh` | true | 项目用 REFdb 表达 dual-bank refresh |
| `rfm threshold=512` | PRAC/RFM 研究阈值 | 真实 MR/器件行为需 vendor 校准 |

### 7.3 timing

| 参数组 | 当前值 | 说明 |
|---|---|---|
| burst | `nBL=6` | BL24 在项目 CK 视图中的占用，不是物理 BL6 |
| RL/WL | 62/26 | 取决于 DVFSL、link protection、RL/WL set；必须对应 JESD209-6 latency 表和目标 MR |
| ACT | 56/25 | `nRCDRD/nRCDWR` |
| PRE/row | 123/132/54/177 | `nRP/nRPab/nRAS/nRC` |
| WCK/CAS | 8/22/3/22 | `nAAD/nWCK2CK/nWCKPST/nCAS`，项目简化状态机参数 |
| refresh | 747/427/10416/1302 | `nRFC/nRFCpb/nREFI/nREFIpb` |

这些值混合了标准表片段、项目换算和研究选择。特别是 RL/WL、WCK training、
DVFS guard、PRAC/RFM、link protection retry 都不能仅凭此模板声称器件准确。

## 8. LPDDR6 synthetic vendor profile

直接加载 `configs/profiles/lpddr6/synthetic_vendor_10667_16gb_linkprot.cfg`。与
`lpddr6.cfg` 的主要差异：

| 功能 | nominal | synthetic vendor |
|---|---|---|
| link protection | off | on |
| Link ECC | off | on，16bit/request |
| DBI | off | on，8bit/request |
| mode profile | standard nominal 标签 | synthetic link-protection 标签 |
| timing 来源 | JEDEC-oriented + research | synthetic `vendor` |

`interface.cpp` 对 LPDDR metadata lane 取 `max(DBI bits, Link ECC bits)`，不会把
复用同一 metadata lane 的两个功能简单相加。该规则是项目接口统计模型，不代替
真实 lane mapping、CRC polynomial、retry buffer 或纠错时序。

## 9. 校准和审计命令

```bash
# 输出最终 timing 及来源
./build/hbm_sim --config configs/calib/hbm4.cfg \
  --dump-timing-table outputs/hbm4_timing.csv

./build/hbm_sim --config configs/calib/lpddr6.cfg \
  --dump-timing-table outputs/lpddr6_timing.csv

# 真实 vendor profile 应通过；generic/research 配置预期可能失败
./build/hbm_sim --config configs/calib/hbm4.cfg --strict-timing-table
```

判断标准：

1. 可运行不等于物理正确。
2. strict 通过只说明 required 字段被标成非占位值，不验证资料真伪。
3. JEDEC 给出范围时，最终支持值仍可能依赖 density、speed-bin、MR 和 vendor。
4. 发布数值结果时，应同时保存 config、timing CSV、vendor 手册版本和命令行。
