# 配置片段

本目录保存可复用的 timing/profile 片段。profile 不是完整 workload 配置，通常由
`configs/run/*.cfg` 或 `configs/calib/*.cfg` 通过 `timing_profile_file` 引用。

真实存储区、memory backend、floorplan、功耗和热模型的运行选择不放在 profiles
下，而应放在 `configs/run` 或命令行中。profiles 只描述器件/标准/模式相关参数，
避免把 workload 或 backend 选择混入器件定义。

## 与 power/thermal 参数的边界

本目录只保存 timing 和器件组织 profile。当前 CLI 只有一个
`timing_profile_file` 槽位，不会自动合并若干 power/thermal 片段；
`power_*_pj`、`thermal_*`、`floorplan` 应放入主 run/calib 配置或作为后置 CLI
覆盖。这样不会让不存在的“多片段继承”看起来像已经实现。

## 目录结构

```text
profiles/
  hbm3/    HBM3 profile；当前为 Ramulator2.1-derived baseline
  hbm4/    HBM4 JEDEC/vendor/Ramulator2.1 differential profile 片段
  lpddr5/  LPDDR5 profile；当前为 Ramulator2.1-derived baseline
  lpddr6/  LPDDR6 JEDEC/mode/profile 片段
  index.csv
```

当前项目只围绕四种存储展开：HBM3、HBM4、LPDDR5、LPDDR6。HBM3 和 LPDDR5
是基于ramulator，因此它们的 profile 用 `ramulator2_*` 命名，并标为
`source = research_default`。

逐目录详细说明：

- [HBM3](hbm3/README.md)
- [HBM4](hbm4/README.md)
- [LPDDR5](lpddr5/README.md)
- [LPDDR6](lpddr6/README.md)

## 来源标注

profile 文件必须写 `source` 和 `note`。推荐含义如下：

- `jedec`：来自标准固定值或标准公式换算值。
- `vendor`：来自厂商 datasheet、speed-bin 表或目标器件手册。
- `derived`：由其他字段推导得到的值。
- `research_default`：研究默认值或 Ramulator2.1-derived baseline，不应用于真实器件结论。

同一个 profile 可以分段切换来源。例如 HBM4 的 refresh 字段可以是 JEDEC-oriented，
但行时序、RL/WL、RAS/ECC/link retry 仍可能是 `research_default`。

## 推荐命名

```text
hbm3/ramulator2_6400_16gb_8hi.cfg
hbm4/jedec_8000_32gb_8hi.cfg
hbm4/ramulator2_8000_32gb_8hi.cfg
hbm4/synthetic_vendor_9000_48gb_16hi.cfg
lpddr5/ramulator2_6400_16gb.cfg
lpddr6/jedec_nominal_10667_16gb.cfg
lpddr6/synthetic_vendor_10667_16gb_linkprot.cfg
lpddr6/jedec_lowdvfs_4267_16gb.cfg
```

命名规则：

- `ramulator2_*`：借鉴 Ramulator2.1 的研究基线。
- `jedec_*`：从标准表或标准公式整理出的 profile。
- `vendor_*`：目标厂商或目标器件 profile。
- `synthetic_vendor_*`：人工构造的 vendor-like 测试 profile，可用于跑通
  `--strict-timing-table` 和验证流程，但不代表任何真实厂商器件。
- 文件名尽量包含 rate、density、stack height 或 mode，便于后续对比。

HBM4 的 `ramulator2_8000_32gb_8hi.cfg` 只用于
`configs/validation/hbm4_reference_1ch.cfg` 的外部参考重叠面。它显式标为
`research_default`，不是项目默认模型；即使参考检查通过也不会变成 JEDEC 或 vendor
profile。

## profile 内部顺序

建议按下面顺序写字段：

1. 元数据：`source`、`note`、`timing_profile`、`vendor_profile`、`mode_profile`。
2. 选择条件：`speed_bin_mbps`、`density_gb`、`stack_height`。
3. 接口和组织：`data_rate_mbps`、`data_bus_bits`、`prefetch_size`、`tCK_ps`、`channels`。
4. 行列 timing：`nCL`、`nCWL`、`nRCD*`、`nRP`、`nRAS`、`nCCD*`、`nWTR*`、`nRTW`。
5. refresh/RFM timing：`tRFC*`、`nRFM*`、`tREFI*`。
6. 控制/训练/RAS timing：`nMRW`、`nMRR`、`nWCKTRAIN`、`nDVFS`、`nECCSCRUB`。
7. 协议开关：HBM edge/SID/ECC/link 或 LPDDR WCK/DVFS/link/DBI 字段。

## 使用示例

```bash
./build-clang-debug/hbm_sim \
  --config configs/run/hbm4.cfg \
  --timing-profile-file configs/profiles/hbm4/synthetic_vendor_9000_48gb_16hi.cfg \
  --vendor-profile synthetic_vendor \
  --strict-timing-table
```

```bash
./build-clang-debug/hbm_sim \
  --config configs/run/lpddr5.cfg \
  --timing-profile-file configs/profiles/lpddr5/ramulator2_6400_16gb.cfg
```

```bash
./build-clang-debug/hbm_sim \
  --config configs/run/lpddr6.cfg \
  --timing-profile-file configs/profiles/lpddr6/synthetic_vendor_10667_16gb_linkprot.cfg \
  --vendor-profile synthetic_vendor \
  --strict-timing-table
```
