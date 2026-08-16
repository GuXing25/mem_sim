# LPDDR6 profile 说明

本文说明本目录四份 LPDDR6 profile。标准依据为
`JESD209-6:2025 LPDDR6 Standard` 双语 PDF。由于 LPDDR6 的 RL/WL、burst、
链路保护、efficiency mode、DVFSL 和 WCK 行为互相耦合，不能脱离
`mode_profile` 只看某一个 timing 数字。

## 1. JESD209-6 共同结构

标准中的 x24 LPDDR6 器件由两个 x12 subchannel 构成：

- 每 subchannel 有 4 bank groups，每组 4 banks，共 16 banks。
- WCK:CK 采用 2:1 关系。
- BL24 在 x12 subchannel 上传输 288 bit，其中 256 bit 是数据、32 bit 是
  non-data，因此每 subchannel 的数据 payload 为 32 B。
- 16 Gb/subchannel 组织表给出 65536 rows、64 columns、256-bit prefetch 和
  2048 B page。
- RL/WL 根据数据率、DVFSL、link protection、efficiency mode 和 mode-register
  set 选择；不能把单一 `nCL/nCWL` 当成所有模式的标准常数。
- CA parity、WCK training、DVFS、PRAC/RFM 和链路保护均有独立模式与流程。

## 2. 当前项目组织与容量

四份 LPDDR6 profile 和内置 preset 已统一使用：

```text
1 channel × 2 subchannels × 4 BG × 4 banks
65536 rows × 64 column transactions
32 B/transaction, 64 B/host line, BL24
```

容量为：

```text
1 × 2 × 4 × 4 × 65536 × 64 × 32 B
= 4,294,967,296 B
= 4 GiB
```

`density_gb = 16` 在这些 profile 中表示每个 subchannel 的 16 Gb 组织表项；
两个 subchannel 合计 32 Gb。`line_size = 64` 不参与物理容量乘法，它是 host
请求粒度；frontend 将其拆成两个 32 B subchannel RD/WR 事务。MemoryImage、
地址映射、DFI payload 和接口字节统计均按 32 B transaction 工作。

`prefetch_size = 24` 在项目字段中记录 BL24 的接口 burst beat 数。标准中的
core data prefetch 是 256 bit，另有 32 bit non-data；不能把 24 误读为
“24-byte data prefetch”。

## 3. 时间单位与换算

- `n*` 字段直接以 CK 为单位。
- `t*_ns` 和 `tREFI_us` 由 loader 按 `ceil(time / tCK)` 换算为 nCK。
- 10667 Mb/s profile 使用 `tCK_ps = 375`。
- 4267 Mb/s profile 使用 `tCK_ps = 937`。

例如 10667 profile 的 `tRCD_RD_ns = 20.7` 会得到
`ceil(20.7 / 0.375) = 56 nCK`。最终值应以
`--dump-timing-table` 输出为准。

## 4. `jedec_nominal_10667_16gb.cfg`

这是 10667 Mb/s、DVFSL 开启、link protection 关闭、BL24 的名义标准片段。

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `mode_profile` | `dvfsl_linkprot_off_bl24` | 决定 timing 所属模式组合 |
| `lpddr_mode_register_profile` | `RLSet1_WLSetA_BL24` | RL/WL/BL 的 MR 选择标签 |
| `rows` / `columns` | 65536 / 64 | 16 Gb/subchannel 组织 |
| `line_size` / `dram_transaction_bytes` | 64 B / 32 B | host line 与单命令 payload |
| `data_bus_bits` / `prefetch_size` | 24 / 24 | x24 device 与 BL24 |
| `nBL` | 6 | 项目 CK 视图中的 BL24 时长 |
| `nCL` / `nCWL` | 62 / 26 | 当前模式的 RL/WL 选择 |
| `tRCD_WR_ns` / `tRCD_RD_ns` | 9.2 / 20.7 ns | ACT 到 WR/RD |
| `nRP` / `nRPab` | 123 / 132 | per-bank/all-bank PRE 恢复 |
| `tRAS_ns` / `tRTP_ns` | 20.0 / 1.5 ns | 行保持与 RD-to-PRE |
| `tWTP_ns` | 13.8 ns | WR-to-PRE |
| `tWTR_S_ns` / `tWTR_L_ns` | 7.2 / 13.8 ns | 写转读短/长路径 |
| `nCCDS` / `nCCDL` | 6 / 10 | 列命令间隔 |
| `nAAD` | 8 | ACT1/ACT2 相关延迟 |
| `nWCK2CK` / `nWCKSYNC` | 22 / 22 | WCK 同步延迟 |

刷新片段：

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `tRFCab_ns` | 280 ns | all-bank refresh recovery |
| `tRFCdb_ns` | 160 ns | dual-bank refresh recovery，映射到模型 per-bank 槽位 |
| `tRREFD_ns` | 7.5 ns | refresh 间额外约束 |
| `tREFI_us` | 3.906 us | 平均刷新间隔 |
| `tREFIdb_ns` | 488 ns | dual-bank refresh 调度间隔 |

训练和状态切换：

- `tWCKTRAIN_ns = 64`：WCK training 占用。
- `tDVFS_ns = 128`：DVFS 转换保护时间。
- `nPDEX = 8`：power-down exit。
- `nSREFEX = 256`：self-refresh exit。

这些值来自所选 JESD209-6 表片段，真实配置仍必须确认 mode register、温度、
密度和 speed-bin 条件完全一致。

## 5. `jedec_lowdvfs_4267_16gb.cfg`

该文件描述低速 DVFS 分支：

- 数据率降为 4267 Mb/s，`tCK = 937 ps`。
- mode 改为 `dvfsl_low_bl24`；物理访问仍是 32B BL24，`nBL = 4` 是该低速
  WCK/CK 分支在项目 timing 表中的 CK 占用值，不表示 BL16。
- RL/WL 改为 `nCL = 46`、`nCWL = 18`。
- WCK/CAS 相关值改为 `nWCK2CK = 18`、`nCAS = 18`。
- link protection、link ECC 和 CA parity 均关闭。

虽然很多 ns 项与 10667 profile 相同，换成更长的 tCK 后得到的 nCK 会更小。
这正是保存 ns 原值而由 loader 换算的意义：同一物理时间约束可以随工作点正确
变化。

## 6. `jedec_linkprot_eff_10667_16gb.cfg`

这是 10667 Mb/s 下启用 DVFSL、link protection 和 efficiency mode 的标准
导向分支。与 nominal profile 相比：

- `lpddr_link_protection = true`。
- `lpddr_link_ecc_enabled = true`。
- `lpddr_link_protection_mode = link_ecc_crc_retry`。
- `tWTP` 从 13.8 ns 增为 20.7 ns。
- `tWTR_S/L` 从 7.2/13.8 ns 增为 14.1/20.7 ns。
- 增加 `nECCSCRUB=86`、`nRASERR=64`、`nLINKRETRY=43` 的项目流程延迟。

`lpddr_link_ecc_bits_per_request = 16` 是项目接口开销统计值。标准 BL24 中的
32 bit non-data 并不必然全部等于外部 link ECC；其具体分配随模式/实现变化。
因此该字段用于模拟链路开销，不能直接推导真实芯片片上 ECC 布局或纠错能力。

## 7. `synthetic_vendor_10667_16gb_linkprot.cfg`

这是虚构 vendor 测试 profile，不对应真实厂商：

- `source = vendor` 仅用于测试严格 provenance 通路。
- 工作点和主要 timing 接近 link-protection profile。
- 额外启用 `lpddr_dbi_enabled = true`，按每请求 8 bit 统计 DBI 开销。
- 启用 link ECC、CRC/retry 风格模式以及 RFM、ECC scrub、RAS error 流程。
- `nRFMab=747`、`nRFMpb=427` 等值为合成测试值。

未来录入真实器件时，应替换：

```text
RL/WL 与 MR set
行、列、刷新 timing
WCK training 和 DVFS 流程
DBI/link ECC/CRC/retry 的实际开销
PRAC/RFM 门限与恢复策略
IDD/电压/功耗和温度条件
物理 row/column/density 组织
```

## 8. 使用与对比

名义 profile：

```bash
./build/hbm_sim --config configs/calib/lpddr6.cfg \
  --dump-timing-table outputs/lpddr6_nominal.csv
```

合成 vendor 通路：

```bash
./build-clang-debug/hbm_sim --standard lpddr6 --requests 32 \
  --timing-profile-file configs/profiles/lpddr6/synthetic_vendor_10667_16gb_linkprot.cfg \
  --strict-timing-table \
  --dump-timing-table outputs/lpddr6_synthetic.csv
```

比较不同 LPDDR6 profile 时，必须同时记录 `mode_profile`、`tCK_ps`、BL、
link-protection、efficiency/DVFSL 和 WCK training 状态；只比较 `nCL` 没有
物理意义。
