# configs 配置体系说明

本目录命令示例默认使用当前 Clang/CMake/VS Code 主构建产物
`./build-clang-debug/hbm_sim`。完整工具安装、构建目录和 Makefile 兼容方式见
[项目根 README](../README.md#开发与构建环境)。

本目录保存 `hbm_sim` 的实验配置文件。配置文件使用简单的 `key = value`
格式，既可以描述一次完整实验，也可以作为 JEDEC/vendor timing 表片段的入口。

当前配置体系已经覆盖标准/profile、controller、trace、真实存储区、三种 memory
backend、floorplan、功耗、热模型、ECC shadow、DFI trace 和 vendor/source
标注。若只想选择大容量存储后端，可直接看
[多 Stack 及存储后端](../文档/多Stack及存储后端.md) 和 [运行配置](run/README.md)。

各目录逐配置说明：

- [calib 配置](calib/README.md)
- [run 配置](run/README.md)
- [HBM3 profiles](profiles/hbm3/README.md)
- [HBM4 profiles](profiles/hbm4/README.md)
- [LPDDR5 profiles](profiles/lpddr5/README.md)
- [LPDDR6 profiles](profiles/lpddr6/README.md)
- [validation 配置](validation/README.md)

本轮新增两类标准相关配置入口：

- LPDDR6 dual-bank refresh 的专用 timing：`nREFDB2ACT`、`nREFDB2REFDBS`、`nREFDB2REFDBL`，也可用 `tDBR2ACT_ns`、`tREFdb2REFdb_S_ns`、`tREFdb2REFdb_L_ns` 这类手册风格 key 覆盖。
- DFI5.0-oriented beat/signal trace：`dfi_phase_count`、`dfi_data_lane_bytes`、`dfi_read_latency_nck`、`dfi_write_latency_nck`，配合 CLI 的 `--dfi-trace` 导出 command/data beat CSV，配合 `--dfi-signal-trace` 导出 `dfi_reset_n/dfi_cs_n/dfi_cke/dfi_odt/dfi_address/dfi_bank/dfi_rddata_en/dfi_wrdata_en/dfi_rddata_valid/dfi_wrdata_mask/dfi_wrdata/dfi_rddata` 和 payload source/init-mask 视图。
- 验证专用配置：`configs/validation/` 分开保存项目原生验收配置、外部参考重叠面配置、vendor 参数清单和 project identity 清单，不用于替代日常 run 配置或真实厂商 profile。

## 当前新增：存储模型、功耗和热参数

除了标准、组织结构和 timing 参数，配置文件现在也可以直接控制真实堆叠存储模型：

```ini
# 是否启用 bank -> layer/tile 的物理放置
floorplan = true

# 是否记录命令能量
power_model = true
power_source = configured_pj
power_scale = 1.0

# 是否把命令能量注入 sparse 3D RC 热模型
thermal_model = true
thermal_ambient_c = 40
thermal_cooling_per_cycle = 0.00025
thermal_rise_c_per_pj = 0.00002
thermal_grid_cols_per_tile = 1
thermal_grid_rows_per_tile = 1
thermal_coupling = true
thermal_lateral_coupling = 0.02
thermal_vertical_coupling = 0.012
thermal_tsv_coupling_scale = 0.03
thermal_tsvs_per_grid = 4
thermal_tsv_radius_m = 5e-6

# 细粒度物理坐标，不分配完整 cell array，只给访问过的数据线打坐标标签
subarrays_per_bank = 16
mats_per_subarray_x = 4
mats_per_subarray_y = 4
cells_per_mat_x = 512
cells_per_mat_y = 512
microbumps_x = 8
microbumps_y = 8

# 第一版 payload SECDED shadow
ecc_shadow = true
ecc_check_on_read = true
ecc_correct_single_bit = true
ecc_inject_period = 0
```

如果需要做更细的功耗校准，也可以覆盖命令能量表：

```ini
power_act_pj = 520
power_act1_pj = 280
power_act2_pj = 280
power_pre_pj = 190
power_preab_pj = 1200
power_cas_pj = 45
power_read_pj = 340
power_read_per_byte_pj = 2.0
power_write_pj = 390
power_write_per_byte_pj = 2.4
power_refpb_pj = 950
power_refdb_pj = 1500
power_refab_pj = 4200
power_rfmpb_pj = 1250
power_rfmab_pj = 5200
power_control_pj = 120
```

这些值仍然是 research-default 级别的可运行参数，不代表任何真实 HBM 器件的 vendor power table。做数值级论文实验时，应把它们整理成单独 profile 或校准配置，并在 README 或配置注释中写清来源。

也可以使用 DRAMsim3-style `VDD/IDD/timing` 推导命令能量：

```ini
power_source = dramsim3_idd
power_vdd = 1.2
idd0 = 65
idd2n = 40
idd3n = 55
idd4r = 390
idd4w = 500
idd5ab = 250
idd5pb = 5
idd_devices_per_rank = 1
```

该模式参考 DRAMsim3 `configuration.cc` 中的
`VDD * current_delta * time * devices_per_rank` 计算方式。统一从
[`hbm4_storage.cfg`](run/hbm4_storage.cfg) 加后置 `--power-source dramsim3_idd`
及 IDD 参数启用；示例 IDD 值只验证公式路径，不是 HBM4 厂商功耗表。

## 0. 目录层级

`configs/` 现在按用途分成三层：

```text
configs/
  run/       可直接运行的日常实验配置
  calib/     面向 JEDEC/vendor 数值校准的模板配置
  profiles/  可复用 timing/profile 片段，按标准再分类
```

三类文件的区别：

| 目录 | 文件特点 | 是否直接跑 | 主要用途 |
| --- | --- | --- | --- |
| `run/` | 配置完整、名字短、面向常用实验 | 是 | 快速跑 HBM3/HBM4/LPDDR5/LPDDR6 baseline |
| `calib/` | 配置完整、字段展开更显式 | 是 | 从 JEDEC/vendor 表逐项校准，常配合 `--strict-timing-table` |
| `profiles/` | 只保存某个 speed-bin/density/mode 的 timing 片段 | 通常不单独跑 | 被 `timing_profile_file` 引用，避免重复抄表 |

常见命令：

```bash
./build-clang-debug/hbm_sim --config configs/run/hbm4.cfg
./build-clang-debug/hbm_sim --config configs/calib/hbm4.cfg --dump-timing-table outputs/hbm4.csv
./build-clang-debug/hbm_sim --config configs/run/lpddr6.cfg \
  --timing-profile-file configs/profiles/lpddr6/jedec_linkprot_eff_10667_16gb.cfg
```

本文档说明 `configs/` 中常用字段的含义、单位、取值和使用建议。更细的
时序配置片段说明见 [profiles/README.md](profiles/README.md)。

## 1. 配置文件语法

基本格式：

```ini
key = value
# 以 # 开头的是注释
```

规则：

- key 不区分大小写；代码内部通常会转成小写处理。
- `true/false`、`yes/no`、`on/off`、`1/0` 都可作为布尔值。
- 普通 timing 字段以 `nXXX` 表示 nCK 周期数，例如 `nRCDRD = 32`。
- JEDEC 单位字段以 `_ns`、`_us`、`_ps` 结尾，例如 `tRFCab_ns = 450`。
- 如果同时配置 `tCK_ps` 和 ns/us timing，程序会用最终的 `tCK_ps` 换算成 nCK。
- 同一字段被多次赋值时，后出现的覆盖先出现的；命令行参数最终覆盖配置文件。

加载顺序：

1. `standard` 或 `--standard` 调用 `make_spec_draft(name)`，只应用标准身份、
   协议能力和默认 profile 选择。
2. `timing_profile`、`speed_bin_mbps`、`density_gb`、`stack_height`、`mode_profile`
   展开完整内置 organization/timing profile。
3. `timing_profile_file` 加载外部 profile 片段。
4. 当前 config 文件中的逐项字段覆盖前面的默认值。
5. 命令行参数做最终覆盖。
6. `finalize_spec()` 统一生成 timing constraints/table。

职责边界：

- `StandardTraits` 只提供标准身份、协议能力、scope、名称别名和默认 profile 选择。
- 内置 profile 提供最小可运行 organization/timing；traits 不保存这两类数值。
- `configs/profiles/` 表达 speed bin、density、stack height、mode 和 vendor 数值。
- `configs/run/` 表达 workload、调度、输出和实验策略。
- `configs/calib/` 显式展开待审计的 JEDEC/vendor/research 数值。
- 修改 timing 后统一重建 timing constraints/table；配置不能自行定义新的命令状态机。

## 2. 主要配置文件

`run/` 下是推荐的日常入口：

- `run/hbm4.cfg`：HBM4 32-channel full-stack 研究配置。
- `run/hbm4_6stack.cfg`：六 stack 主动系统配置。
- `run/hbm4_storage.cfg`：数据、三种后端、功耗和热网格统一入口。
- `run/hbm3.cfg`：HBM3 full-stack baseline 配置。
- `run/lpddr6.cfg`：LPDDR6 默认研究配置，包含 WCK、DVFS、DBI/ECC、link protection、低功耗等入口。
- `run/lpddr5.cfg`：LPDDR5 baseline 配置，用于和 LPDDR6 对比。

`calib/` 下是校准入口：

- `calib/hbm3.cfg`：HBM3 Ramulator2.1-derived baseline 校准入口。
- `calib/hbm4.cfg`：HBM4 JEDEC/vendor timing 校准模板。
- `calib/lpddr5.cfg`：LPDDR5 Ramulator2.1-derived baseline 校准入口。
- `calib/lpddr6.cfg`：LPDDR6 JEDEC/vendor timing 校准模板。

`profiles/` 下是复用片段：

- `profiles/hbm3/`：HBM3 profile 片段。
- `profiles/hbm4/`：HBM4 profile 片段，包括 vendor 示例。
- `profiles/lpddr5/`：LPDDR5 profile 片段，当前为 Ramulator2.1-derived baseline。
- `profiles/lpddr6/`：LPDDR6 nominal、link protection、low DVFS、synthetic vendor profile 片段。
- `profiles/index.csv`：profile 目录索引。

快速运行：

```bash
./build-clang-debug/hbm_sim --config configs/run/hbm4.cfg
./build-clang-debug/hbm_sim --config configs/run/hbm4_storage.cfg
./build-clang-debug/hbm_sim --config configs/run/hbm3.cfg
./build-clang-debug/hbm_sim --config configs/run/lpddr6.cfg
./build-clang-debug/hbm_sim --config configs/run/lpddr5.cfg
./build-clang-debug/hbm_sim --config configs/calib/hbm4.cfg
./build-clang-debug/hbm_sim --config configs/calib/hbm3.cfg
./build-clang-debug/hbm_sim --config configs/calib/lpddr6.cfg
./build-clang-debug/hbm_sim --config configs/calib/lpddr5.cfg
./build-clang-debug/hbm_sim --standard hbm4 --requests 32 \
  --timing-profile-file configs/profiles/hbm4/synthetic_vendor_9000_48gb_16hi.cfg \
  --strict-timing-table
./build-clang-debug/hbm_sim --standard lpddr6 --requests 32 \
  --timing-profile-file configs/profiles/lpddr6/synthetic_vendor_10667_16gb_linkprot.cfg \
  --strict-timing-table
```

## 3. 基础实验字段

这些字段决定一次实验使用哪个标准、跑多少请求、输入流量是什么。

| 字段 | 类型/单位 | 常见取值 | 说明 |
| --- | --- | --- | --- |
| `standard` | 字符串 | `hbm4`、`hbm3`、`lpddr6`、`lpddr5` | 选择标准 traits 和默认 profile。`hbm` 映射到 HBM4；`lpddr`/`ldppr` 映射到 LPDDR6。 |
| `pattern` | 字符串 | `stream`、`random` | 合成流量类型。trace 模式下由 `trace` 指定输入文件。 |
| `trace` / `trace_path` | 路径 | `examples/sample.trace` | trace 文件路径。每行可写 `R addr`、`W addr` 或 `cycle R/W addr`；多 stack 可追加 `stack=N qos=N`。 |
| `requests` | 个数 | `10000` | 请求数量。trace 模式下 `0` 表示读取完整 trace。 |
| `read_ratio` | 百分比 | `100`、`80`、`50` | 合成流量中读请求比例，仅对 synthetic pattern 有意义。 |
| `seed` | 整数 | `1` | random pattern 的随机种子。 |
| `addr_stride` | byte | `64` | stream pattern 的地址步长，通常等于 cache line 大小。 |
| `inject_interval` | cycle | `0`、`1`、`4` | frontend 注入请求的间隔。`0` 表示尽可能快注入。 |
| `init_sequence` | 字符串 | `none`、`auto`、`hbm4`、`lpddr6`、`lpddr6_full` | 在普通 workload 前注入初始化/训练/控制命令。 |
| `init_sequence_interval` | cycle | `1` | 初始化/训练序列中相邻控制请求的注入间隔。 |
| `max_cycles` | cycle | `100000000` | 仿真最大 cycle。到达后仍未完成会输出 `hit_cycle_limit=true`。 |

示例：

```ini
standard = hbm4
pattern = stream
requests = 10000
read_ratio = 100
addr_stride = 64
max_cycles = 100000000
```

## 4. profile 与来源字段

这些字段用于说明 timing 和组织参数来自哪个 profile、哪个速率档、哪个器件维度。

| 字段 | 类型/单位 | 说明 |
| --- | --- | --- |
| `timing_profile` | 字符串 | 内置 profile 名称，例如 `hbm4_jedec_8g_32gb_8hi`。 |
| `timing_profile_file` | 路径 | 外部 timing profile 片段路径，通常位于 `configs/profiles/`。 |
| `vendor_profile` | 字符串 | vendor profile 标签，用来标记目标厂商/器件配置。 |
| `mode_profile` | 字符串 | mode 分支标签，例如 LPDDR6 的 DVFS/link-protection/BL 组合。 |
| `speed_bin_mbps` | Mbps/pin | 速率档，例如 `8000`、`9000`、`10667`。 |
| `density_gb` | Gb | die density，例如 `16`、`32`、`48`。 |
| `stack_height` | 层数 | HBM stack height，例如 `8`、`12`、`16`；LPDDR 通常为 `0`。 |
| `source` | 字符串 | timing profile 片段中的来源标签：`jedec`、`vendor`、`derived`、`research_default`。 |
| `note` | 字符串 | timing profile 片段注释，用来记录表格出处或研究假设。 |
| `strict_timing_table` | bool | 如果为 true，存在 vendor-required 默认值时会报错。常用于真实数值级对比前检查。 |

推荐真实器件校准写法：

```ini
standard = hbm4
timing_profile = hbm4_jedec_8g_32gb_8hi
timing_profile_file = configs/profiles/hbm4/jedec_8000_32gb_8hi.cfg
vendor_profile = example_vendor_hbm4_8g
speed_bin_mbps = 8000
density_gb = 32
stack_height = 8
```

## 5. 组织结构和接口字段

这些字段描述存储器组织、数据接口宽度和基础访问粒度。

| 字段 | 类型/单位 | 说明 |
| --- | --- | --- |
| `channels` | 个数 | channel 数。HBM4 full stack 常用 `32`，HBM3 常用 `16`。 |
| `pseudo_channels` | 个数 | 每 channel 的 pseudo-channel 数。HBM 常用 `2`。 |
| `sids` | 个数 | stack ID 或 SID 维度数量，HBM4 中用于更细堆叠建模。 |
| `ranks` | 个数 | rank 数。当前 configs 中通常不显式写，默认由 preset 给出。 |
| `bank_groups` | 个数 | 每 rank/SID/PC 下的 bank group 数。 |
| `banks_per_group` | 个数 | 每 bank group 中 bank 数。 |
| `rows` | 行数 | 每 bank row 数。用于地址映射容量和 row 命中行为。 |
| `columns` | 列数 | 每 row column 数。用于地址映射。 |
| `line_size` | byte | frontend/host 默认请求粒度；HBM4 和 LPDDR6 当前使用 `64`。它不是 JEDEC 物理 row。 |
| `dram_transaction_bytes` | byte | 一条 RD/WR 命令的 payload 粒度；`0` 表示沿用 `line_size`。HBM4 x32/BL8 与 LPDDR6 x12/BL24 均使用 `32`，因此一个默认 64B host 请求拆成两个事务。 |
| `data_rate_mbps` | Mbps/pin | 数据接口速率。 |
| `data_bus_bits` | bit | stack/device 级数据总线宽度。 |
| `prefetch_size` / `internal_prefetch_size` | beat | 内部预取长度。 |
| `dfi_phase_count` | phase | DFI beat trace 的 phase 数；`0` 表示按 `tick_multiplier` 派生。没有厂商 PHY 相位资料时使用 project-defined 配置。 |
| `dfi_data_lane_bytes` | byte | DFI trace 每个 data beat 的 payload 字节数；`0` 表示按 `dram_transaction_bytes/nBL` 向上取整。实际导出时优先按请求真实 payload 长度切 beat。 |
| `dfi_read_latency_nck` | nCK | RD/RDA 到 READ_DATA beat 的 DFI trace 延迟；`0` 使用 `nCL`。厂商 PHY 对齐延迟未知时保持可配置默认。 |
| `dfi_write_latency_nck` | nCK | WR/WRA 到 WRITE_DATA beat 的 DFI trace 延迟；`0` 使用 `nCWL`。厂商 PHY 对齐延迟未知时保持可配置默认。 |
| `tick_multiplier` | 倍数 | 一个 nCK 拆成几个 simulator tick。HBM edge 模型常用 `2` 表示半周期。 |
| `tCK_ps` | ps | 时钟周期。ns/us timing 会用它换算成 nCK。 |
| `full_stack_model` | bool | 是否按 stack-level 模型输出和计算带宽。 |

示例：

```ini
channels = 32
pseudo_channels = 2
sids = 2
bank_groups = 4
banks_per_group = 4
line_size = 64
dram_transaction_bytes = 32
data_rate_mbps = 8000
data_bus_bits = 2048
tick_multiplier = 2
tCK_ps = 500
dfi_phase_count = 2
dfi_data_lane_bytes = 16
```

HBM4 和 LPDDR6 的普通 synthetic 请求及没有显式 payload 长度的 trace 请求会按
`64B host -> 2 x 32B transaction` 执行。为保留 byte-mask/局部读写的数据调试
能力，显式 `data=`、`expect=` 或 `mask=` 小于 32B 时，当前模型仍按给出的有效
字节数生成一个局部事务；这种用法不是完整 BL8/BL24 引脚占用模型。做物理带宽或接口
开销实验时，应让 trace 请求长度为 32B 的整数倍。

## 6. controller 和调度字段

这些字段决定 controller 内部队列、调度、行策略和地址分发行为。

| 字段 | 类型/单位 | 常见取值 | 说明 |
| --- | --- | --- | --- |
| `scheduler` | 字符串 | `frfcfs`、`fcfs` | 请求选择策略。`frfcfs` 优先 row hit/timing-ready，再按到达时间。 |
| `row_policy` | 字符串 | `open_page`、`closed_page`、`closed_cap` | 行策略。`closed_cap` 会在列访问达到 cap 后关闭行。 |
| `row_policy_cap` | 次数 | `4` | ClosedCAP 中同一行/同一 bank 连续列访问上限。 |
| `address_mapping` / `addr_mapping` | 字符串 | `default`、`RoBaRaCoCh`、`ChRaBaRoCo`、`RoCoRaBaCh` | 地址位排列模板。模板名按高位到低位理解。 |
| `channel_mapper` | 字符串 | `decoded`、`round_robin`、`xor` | 请求到 channel 的分发策略。 |
| `stack_count` | 个 | `1`、`6` | 独立 memory stack 数；HBM4 设为 6 时创建 6×32 个 channel controller。 |
| `stack_mapping` | 字符串 | `interleaved`、`blocked` | 系统全局地址到 stack 的条带或连续区间映射。 |
| `stack_interleave_bytes` | byte | `256` | interleaved 映射的条带大小，必须为 `line_size` 的整数倍。 |
| `stack_ingress_buffer_size` | entry/stack | `256` | 每颗 stack 的独立入口队列容量，满时产生目标 stack 反压。 |
| `stack_dispatch_width` | request/cycle/stack | `4` | 每颗 stack 每拍最多分发到 channel controller 的请求数。 |
| `stack_qos_policy` | 字符串 | `fcfs`、`strict_priority` | stack 入口仲裁；trace 用 `qos=N` 指定优先级。 |
| `read_buffer_size` | entry | `32` | 读队列容量。 |
| `write_buffer_size` | entry | `32` | 写队列容量。 |
| `priority_buffer_size` | entry | `2048` | refresh/RFM/PRE 等维护请求队列容量。 |
| `write_low_watermark` | 比例 | `0.2` | write-drain 退出水位。 |
| `write_high_watermark` | 比例 | `0.8` | write-drain 进入水位。 |
| `single_controller` | bool | `false` | 是否使用旧单 controller 聚合模型。默认多 controller。 |

建议：

- HBM stream 流量如果只激活少数 channel，可以尝试 `channel_mapper = xor`。
- 研究行命中/行冲突时，重点比较 `open_page`、`closed_page` 和 `closed_cap`。
- 对齐 Ramulator2.1 风格实验时，通常使用 `scheduler = frfcfs`。
- 多 stack 的 `strict_priority` 只作用于 stack ingress；进入 channel 后仍使用 `scheduler`。
- trace 的 `stack=N` 表示显式 stack-local 地址；不写时由系统全局地址自动映射。
- 多 stack 文件后端中的 `memory_capacity_bytes` 按每颗 stack 解释；文件路径按
  `{stack}` 或自动 `.stackN` 后缀隔离。

## 7. refresh 和 RFM 字段

这些字段控制 refresh/RFM 维护请求的生成、轮转和仲裁压力。

| 字段 | 类型/单位 | 常见取值 | 说明 |
| --- | --- | --- | --- |
| `supports_refresh` | bool | `true` | 是否启用 refresh。 |
| `refresh_policy` | 字符串 | `per_bank`、`all_bank` | refresh 策略。HBM 常用 per-bank；all-bank 会产生全 bank 维护压力。 |
| `lpddr_dual_bank_refresh` | bool | `true`/`false` | LPDDR6 是否启用 REFdb dual-bank refresh。 |
| `refresh_temperature_mode` | 字符串 | `normal`、`high`、`extended` | refresh 温度模式，用于调整 refresh interval。 |
| `refresh_high_temp_multiplier` | 倍数 | `2` | 高温 refresh 倍频系数。 |
| `refresh_postpone_limit` | 次数 | `0`、`8` | refresh 可延后次数上限。 |
| `refresh_pullin_limit` | 次数 | `0`、`8` | refresh 可提前次数上限。 |
| `refresh_credit_limit` | credit | `0`、`8` | refresh credit 上限。 |
| `supports_rfm` | bool | `true`/`false` | 是否启用 RFM/PRAC。 |
| `rfm_policy` | 字符串 | `per_bank`、`all_bank` | RFM 策略。 |
| `rfm_act_threshold` | ACT 次数 | `256`、`1024` | ACT/RAA 计数达到阈值后触发 RFM。 |
| `rfm_decrement` | 计数 | `256` | RFM 发出后 RAA 计数递减量。 |

示例：

```ini
supports_refresh = true
refresh_policy = per_bank
refresh_temperature_mode = high
refresh_postpone_limit = 8
refresh_pullin_limit = 8
refresh_credit_limit = 8
supports_rfm = true
rfm_policy = per_bank
rfm_act_threshold = 256
rfm_decrement = 256
```

## 8. HBM 专用字段

这些字段主要用于 HBM3/HBM4，尤其是 HBM4 stack、SID/PC、edge pairing、RAS/ECC/link 行为。

| 字段 | 类型/单位 | 说明 |
| --- | --- | --- |
| `hbm_full_32_channel_stack` | bool | 是否按 HBM4 32-channel full stack 建模。 |
| `hbm_sid_interleave` | bool | 地址映射是否让 SID 参与 interleave。 |
| `hbm_pc_interleave` | bool | 地址映射是否让 pseudo-channel 参与 interleave。 |
| `hbm_edge_pairing` | bool | 是否启用 HBM rising/falling edge pairing。 |
| `hbm_strict_edge_pairing` | bool | 是否使用更严格的 HBM4 PRE pairing 检查。 |
| `hbm_edge_pairing_matrix` | 字符串 | pairing 规则标签，例如 `hbm4_jedec270_4a_row_col_pre_matrix`。 |
| `hbm_sid_mapping` | 字符串 | SID 映射策略标签，例如 `sid_pair_8hi`、`stack_height_div4`。 |
| `hbm_ecc_scheme` | 字符串 | ECC 建模策略标签，例如 sideband ECC。 |
| `hbm_ras_policy` | 字符串 | RAS 策略标签，例如 `counter_only`、`metadata_only`。 |
| `hbm_link_crc_mode` | 字符串 | link CRC 模式，例如 `off`、`crc16`。 |
| `hbm_link_retry_enabled` | bool | 是否启用 HBM link retry 入口。 |
| `hbm_link_crc_bits_per_request` | bit/transaction | 每个物理 RD/WR 事务计入的 link CRC bit 开销。字段名为兼容旧配置保留。 |
| `hbm_ras_metadata_bits_per_request` | bit/transaction | 每个物理 RD/WR 事务计入的 RAS metadata bit 开销。字段名为兼容旧配置保留。 |
| `hbm_ecc_bits_per_request` | bit/transaction | 每个物理 HBM RD/WR 事务计入的外部 ECC/metadata 开销；HBM4 默认每 PC 的 32B 事务使用 16 bit。片上 ECC check bits 不应混入此字段。 |
| `supports_ecc` | bool | 是否启用 ECC 相关能力入口。 |

注意：

- 当前 ECC/RAS/link 字段主要影响接口开销和命令/统计入口，不等价于完整错误注入、重试和修正延迟模型。
- `hbm_edge_pairing_matrix` 是规则标签，真正的命令合法性由 controller 和 validator 中的 pairing 检查执行。

## 9. LPDDR 专用字段

这些字段主要用于 LPDDR5/LPDDR6，描述 WCK、CAS、DVFS、DBI/ECC、link protection 和低功耗行为。

| 字段 | 类型/单位 | 常见取值 | 说明 |
| --- | --- | --- | --- |
| `lpddr_efficiency_mode` | 字符串 | `normal`、`static`、`dynamic` | LPDDR efficiency mode，影响 subchannel/interface 行为。 |
| `lpddr_dvfs_mode` | 字符串 | `nominal`、`low`、`disabled` | DVFS 模式。 |
| `lpddr_low_data_rate_mbps` | Mbps/pin | `3200`、`4267` | 低速 DVFS 档数据率。 |
| `lpddr_wck_mode` | 字符串 | `cas_sync`、`always_on`、`burst_sync` | WCK 模式。 |
| `lpddr_wck_ratio` | 倍数 | `4` | WCK:CK 比率。 |
| `lpddr_mode_register_profile` | 字符串 | `default`、自定义标签 | mode register 组合标签。 |
| `lpddr_wck_training_mode` | 字符串 | `startup_only`、自定义标签 | WCK training 策略标签。 |
| `lpddr_wck_training_required` | bool | `true` | DVFS 后是否要求重新 WCK training。 |
| `lpddr_dvfs_transition_policy` | 字符串 | `idle_channel`、自定义标签 | DVFS transition 策略标签。 |
| `lpddr_link_protection` | bool | `false`/`true` | 是否启用 link protection 总开关。 |
| `lpddr_link_protection_mode` | 字符串 | `off`、自定义标签 | link protection 模式标签。 |
| `lpddr_link_ecc_enabled` | bool | `false`/`true` | 是否启用 LPDDR link ECC 入口。 |
| `lpddr_link_ecc_bits_per_request` | bit/request | `0`、`8`、`16` | 每请求 link ECC bit 开销。 |
| `lpddr_dbi_enabled` | bool | `false`/`true` | 是否启用 DBI 开销入口。 |
| `lpddr_dbi_bits_per_request` | bit/request | `0`、`8` | 每请求 DBI bit 开销。 |
| `lpddr_ca_parity_enabled` | bool | `false`/`true` | 是否启用 CA parity。启用时通常要求 WCK always-on。 |
| `lpddr_ca_parity_bits_per_command` | bit/command | `1` | 每条 LPDDR 命令的 CA parity bit 开销。 |
| `lpddr_low_power_state_policy` | 字符串 | `controller_idle`、自定义标签 | 低功耗状态策略标签。 |
| `low_power_mode` | 字符串 | `off`、`power_down`、`self_refresh` | 空闲后进入的低功耗模式。 |
| `low_power_entry_cycles` | cycle | `0`、`100` | 空闲多少 cycle 后进入低功耗。 |
| `low_power_exit_cycles` | cycle | `0`、`32` | power-down 退出阻塞周期。 |
| `self_refresh_exit_cycles` | cycle | `0`、`128` | self-refresh 退出阻塞周期。 |

示例：

```ini
lpddr_dvfs_mode = nominal
lpddr_wck_mode = cas_sync
lpddr_wck_ratio = 4
lpddr_link_protection = true
lpddr_link_ecc_enabled = true
lpddr_dbi_enabled = true
low_power_mode = power_down
low_power_entry_cycles = 512
low_power_exit_cycles = 32
```

## 10. 通用接口开销字段

这些字段不绑定某个具体标准，可用于研究 payload 带宽和实际接口占用之间的差异。

| 字段 | 类型/单位 | 说明 |
| --- | --- | --- |
| `metadata_bits_per_request` | bit/request | 每个请求额外 metadata bit。 |
| `ecc_bits_per_request` | bit/request | 每个请求通用 ECC bit。 |

HBM/LPDDR 也有自己的专用 bit 字段，例如：

- `hbm_link_crc_bits_per_request`
- `hbm_ras_metadata_bits_per_request`
- `hbm_ecc_bits_per_request`
- `lpddr_dbi_bits_per_request`
- `lpddr_link_ecc_bits_per_request`

这些开销会进入输出中的：

- `interface_overhead_bits`
- `interface_read_bytes`
- `interface_write_bytes`
- `achieved_if_bw_GBps`
- `payload_efficiency_pct`

## 11. nCK timing 字段

这些字段直接使用 nCK 周期数，不需要单位换算。适合已经从 JEDEC/vendor 表换算好后的配置。

| 字段 | 说明 |
| --- | --- |
| `nBL` | burst length 对应的命令/数据窗口。 |
| `nCL` | CAS read latency。 |
| `nCWL` | CAS write latency。 |
| `nRCDRD` | ACT 到 RD 的行到列读延迟。 |
| `nRCDWR` | ACT 到 WR 的行到列写延迟。 |
| `nRP` | per-bank precharge recovery。 |
| `nRPab` | all-bank precharge recovery。 |
| `nRAS` | ACT 后 row active 最小时间。 |
| `nRC` | 同一 bank ACT 到下一次 ACT 的行周期。 |
| `nRTP` | RD 到 PRE 的延迟。 |
| `nWR` | WR 到 PRE 的写恢复延迟。 |
| `nCCDS` | short column-to-column delay。 |
| `nCCDL` | long column-to-column delay。 |
| `nCCDR` | read/write 或 rank/方向相关 column delay。 |
| `nRRDS` | short ACT-to-ACT delay。 |
| `nRRDL` | long ACT-to-ACT delay。 |
| `nFAW` | four activate window。 |
| `nAAD` | LPDDR ACT1 到 ACT2 相关 deadline/delay。 |
| `nWCK2CK` | LPDDR WCK to CK sync 延迟。 |
| `nWCKPST` | LPDDR WCK postamble/window 延迟。 |
| `nCAS` | LPDDR CAS 到 RD/WR 的同步延迟。 |
| `nCS` | chip select 或命令间隔相关延迟。 |
| `nPPD` | power-down 或 precharge power-down 相关延迟。 |
| `nWTRS` | write-to-read short delay。 |
| `nWTRL` | write-to-read long delay。 |
| `nRTW` | read-to-write delay。 |
| `nRFC` | all-bank refresh recovery。 |
| `nRFCpb` | per-bank refresh recovery。 |
| `nRFMab` | all-bank RFM recovery。 |
| `nRFMpb` | per-bank RFM recovery。 |
| `nRREFD` | LPDDR REFdb dual-bank pair 相关间隔。 |
| `nREFDB2ACT` | LPDDR6 REFdb 到不同 bank ACT 的恢复间隔。 |
| `nREFDB2REFDBS` | LPDDR6 REFdb 到 REFdb short pair 间隔。 |
| `nREFDB2REFDBL` | LPDDR6 REFdb 到 REFdb long pair 间隔；当前调度器使用该值做保守 REFdb->REFdb 约束。 |
| `nREFI` | refresh interval。 |
| `nREFIpb` | per-bank refresh interval。 |
| `nMRW` | mode register write 延迟。 |
| `nMRR` | mode register read 延迟。 |
| `nWCKSYNC` | WCK sync 命令延迟。 |
| `nWCKTRAIN` | WCK training 延迟。 |
| `nDVFS` | DVFS transition 延迟。 |
| `nPDEX` | power-down exit 延迟。 |
| `nSREFEX` | self-refresh exit 延迟。 |
| `nECCSCRUB` | ECC scrub 抽象延迟。 |
| `nRASERR` | RAS error 抽象恢复延迟。 |
| `nLINKRETRY` | link retry 抽象延迟。 |

示例：

```ini
nCL = 32
nCWL = 24
nRCDRD = 32
nRCDWR = 28
nRP = 32
nRAS = 64
nRC = 96
nFAW = 80
```

## 12. JEDEC 单位 timing 字段

这些字段使用手册中常见的 ns/us 单位。程序会根据最终 `tCK_ps` 自动向上取整换算为 nCK。

| 字段 | 单位 | 换算目标 | 说明 |
| --- | --- | --- | --- |
| `tCK_ps` | ps | `tCK_ps` | 时钟周期，是其他单位字段换算的基础。 |
| `tRC_ns` | ns | `nRC` | row cycle time。 |
| `tRAS_ns` | ns | `nRAS` | row active time。 |
| `tRCD_RD_ns` / `tRCDRD_ns` | ns | `nRCDRD` | ACT 到 RD。 |
| `tRCD_WR_ns` / `tRCDWR_ns` | ns | `nRCDWR` | ACT 到 WR。 |
| `tRP_ns` / `tRPpb_ns` | ns | `nRP` | per-bank precharge recovery。 |
| `tRPab_ns` | ns | `nRPab` | all-bank precharge recovery。 |
| `tRTP_ns` | ns | `nRTP` | RD 到 PRE。 |
| `tWTP_ns` / `tWR_ns` | ns | `nWR` | write recovery/write-to-precharge。 |
| `tRRD_ns` / `tRRDS_ns` | ns | `nRRDS` | ACT-to-ACT short。 |
| `tRRDL_ns` | ns | `nRRDL` | ACT-to-ACT long。 |
| `tFAW_ns` | ns | `nFAW` | four activate window。 |
| `tWTR_S_ns` / `tWTRS_ns` | ns | `nWTRS` | write-to-read short。 |
| `tWTR_L_ns` / `tWTRL_ns` | ns | `nWTRL` | write-to-read long。 |
| `tRTW_ns` | ns | `nRTW` | read-to-write。 |
| `tCCDS_ns` | ns | `nCCDS` | column-to-column short。 |
| `tCCDL_ns` | ns | `nCCDL` | column-to-column long。 |
| `tCCDR_ns` | ns | `nCCDR` | column direction/rank delay。 |
| `tRFCab_ns` / `tRFC_ns` | ns | `nRFC` | all-bank refresh recovery。 |
| `tRFCpb_ns` | ns | `nRFCpb` | per-bank refresh recovery。 |
| `tRFCdb_ns` | ns | `nRFCpb` | LPDDR6 dual-bank refresh recovery。 |
| `tRFMab_ns` | ns | `nRFMab` | all-bank RFM recovery。 |
| `tRFMpb_ns` | ns | `nRFMpb` | per-bank RFM recovery。 |
| `tRREFD_ns` | ns | `nRREFD` | LPDDR REFdb pair 间隔。 |
| `tDBR2ACT_ns` / `tREFdb2ACT_ns` | ns | `nREFDB2ACT` | LPDDR6 REFdb 到不同 bank ACT。 |
| `tDBR2DBR_S_ns` / `tREFdb2REFdb_S_ns` | ns | `nREFDB2REFDBS` | LPDDR6 REFdb 到 REFdb short pair。 |
| `tDBR2DBR_L_ns` / `tREFdb2REFdb_L_ns` | ns | `nREFDB2REFDBL` | LPDDR6 REFdb 到 REFdb long pair。 |
| `tREFI_us` | us | `nREFI` | refresh interval。 |
| `tREFIpb_ns` / `tREFIpb_us` | ns/us | `nREFIpb` | per-bank refresh interval。 |
| `tREFIdb_ns` | ns | LPDDR REFdb interval | LPDDR6 dual-bank refresh interval。 |
| `tAAD_ns` | ns | `nAAD` | LPDDR split activate 相关延迟。 |
| `tWCK2CK_ns` | ns | `nWCK2CK` | WCK to CK sync。 |
| `tWCKPST_ns` | ns | `nWCKPST` | WCK postamble/window。 |
| `tCAS_ns` | ns | `nCAS` | LPDDR CAS sync 延迟。 |
| `tMRW_ns` | ns | `nMRW` | mode register write。 |
| `tMRR_ns` | ns | `nMRR` | mode register read。 |
| `tWCKSYNC_ns` | ns | `nWCKSYNC` | WCK sync。 |
| `tWCKTRAIN_ns` | ns | `nWCKTRAIN` | WCK training。 |
| `tDVFS_ns` | ns | `nDVFS` | DVFS transition。 |
| `tPDEX_ns` / `tXP_ns` | ns | `nPDEX` | power-down exit。 |
| `tSREFEX_ns` / `tXS_ns` | ns | `nSREFEX` | self-refresh exit。 |
| `tECCSCRUB_ns` | ns | `nECCSCRUB` | ECC scrub 抽象延迟。 |
| `tRASERR_ns` | ns | `nRASERR` | RAS error 抽象恢复延迟。 |
| `tLINKRETRY_ns` | ns | `nLINKRETRY` | link retry 抽象延迟。 |

示例：

```ini
tCK_ps = 500
tRCD_RD_ns = 16.0
tRCD_WR_ns = 14.0
tRP_ns = 16.0
tRAS_ns = 32.0
tRFCab_ns = 450
tREFI_us = 3.9
```

## 13. trace、验证和导出字段

这些字段也可以写在 config 里，等价于相应 CLI 参数。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `cmd_trace` / `cmd_trace_path` | 路径 | 导出 issued command CSV；多 stack 时含 `stack_id`、局部 `address` 和全局 `system_address`。 |
| `dfi_trace` / `dfi_trace_path` / `dump_dfi_trace` | 路径 | 导出 DFI command/data beat CSV；多 stack 时保留 stack/local/global 地址。 |
| `dfi_signal_trace` / `dfi_signal_trace_path` / `dump_dfi_signal_trace` | 路径 | 导出 DFI-like signal CSV，包含 `dfi_reset_n`、`dfi_cs_n`、`dfi_cke`、`dfi_odt`、`dfi_address`、`dfi_bank`、`dfi_rddata_en`、`dfi_wrdata_en`、`dfi_rddata_valid`、`dfi_wrdata_mask`、`dfi_wrdata`、`dfi_rddata` 和 payload source/init-mask 审计字段。 |
| `validate_cmd_trace` / `validate_command_trace` | bool | 仿真后运行离线 command validator。 |
| `validate_dfi_trace` | bool | 仿真后运行离线 DFI validator；失败时非零退出。 |
| `dump_timing_table` / `timing_table_path` | 路径 | 导出 timing table CSV。 |

示例：

```ini
cmd_trace = outputs/hbm4_cmd_trace.csv
dfi_trace = outputs/hbm4_dfi_trace.csv
dfi_signal_trace = outputs/hbm4_dfi_signal_trace.csv
validate_cmd_trace = true
validate_dfi_trace = true
dump_timing_table = outputs/hbm4_timing.csv
```

## 14. 字段别名和兼容写法

为了方便从 JEDEC/vendor 表抄录，部分字段支持别名：

- `addr_mapping` 等价于 `address_mapping`。
- `trace_path` 等价于 `trace`。
- `cmd_trace_path` 等价于 `cmd_trace`。
- `dfi_trace_path`、`dump_dfi_trace` 等价于 `dfi_trace`。
- `timing_table_path` 等价于 `dump_timing_table`。
- `validate_command_trace` 等价于 `validate_cmd_trace`。
- `internal_prefetch_size` 等价于 `prefetch_size`。
- `tRCDRD_ns`、`tRCD_RD_ns` 都会写入 `nRCDRD`。
- `tRCDWR_ns`、`tRCD_WR_ns` 都会写入 `nRCDWR`。
- `tWTRS_ns`、`tWTR_S_ns` 都会写入 `nWTRS`。
- `tWTRL_ns`、`tWTR_L_ns` 都会写入 `nWTRL`。
- `tPDEX_ns`、`tXP_ns` 都会写入 `nPDEX`。
- `tSREFEX_ns`、`tXS_ns` 都会写入 `nSREFEX`。

建议在新配置中优先使用本文档列出的规范字段名，别名主要用于兼容手册表格或旧配置。

## 15. 新增配置文件建议

- 做真实数值级对比时，优先复制 `configs/calib/hbm4.cfg` 或 `configs/calib/lpddr6.cfg`；HBM3/LPDDR5 在没有手册表前先使用 Ramulator2.1-derived baseline。
- 如果某组 timing 来自固定 JEDEC/vendor 表，优先放入 `configs/profiles/<standard>/`，主配置只引用 `timing_profile_file`。
- 配置中的研究默认值要用注释或 `note` 写清楚，不要伪装成官方数值。
- 文件名建议体现标准和用途，例如 `hbm4_vendor_x_9000_48gb_16hi.cfg`。
- 新增配置后建议至少跑一次：

```bash
./build-clang-debug/hbm_sim --config configs/your_config.cfg --requests 128 --validate-cmd-trace
```

- 如果用于真实对比，建议再加：

```bash
./build-clang-debug/hbm_sim --config configs/your_config.cfg --strict-timing-table --dump-timing-table outputs/timing.csv
```

## 16. 主配置和 profiles 的关系

主配置文件适合描述一次实验的完整设置；`profiles/` 适合保存可复用的表格片段。

推荐组合：

```ini
standard = hbm4
timing_profile = hbm4_jedec_8g_32gb_8hi
timing_profile_file = configs/profiles/hbm4/jedec_8000_32gb_8hi.cfg
vendor_profile = example_vendor
channel_mapper = xor
scheduler = frfcfs
row_policy = open_page
```

这样可以把“器件 timing 来源”和“本次实验策略”分开维护：同一份 timing profile 能被多个实验配置复用，而同一个 workload 也可以切换不同 vendor profile 做对比。

## 17. 配置维护流程

新增或修改配置时，建议按下面流程检查：

1. 先确认该字段是“器件属性”还是“实验策略”。
2. 器件属性优先放到 preset/profile，实验策略优先放到主 cfg 或 CLI。
3. 如果字段来自手册，写清楚单位，并尽量保留原始 ns/us/ps 字段。
4. 如果字段是 nCK，确认它使用的 `tCK_ps` 与目标 speed-bin 一致。
5. 运行一次小请求量 smoke，确认不会 hit cycle limit。
6. 导出 timing table，检查来源和 vendor-required 标记。

推荐命令：

```bash
./build-clang-debug/hbm_sim --config configs/your_config.cfg --requests 128 --validate-cmd-trace
./build-clang-debug/hbm_sim --config configs/your_config.cfg --dump-timing-table outputs/your_timing.csv
```

## 18. 真实对比前检查

如果目标是和 Ramulator2.1 或真实器件做数值级对比，至少确认：

- `standard`、`speed_bin_mbps`、`density_gb`、`stack_height` 与目标一致。
- `data_bus_bits` 和 `data_rate_mbps` 对应同一个接口粒度。
- `line_size`、`prefetch_size`、burst 长度和 trace 请求粒度一致。
- 地址映射和 channel mapper 与对比对象一致或有明确说明。
- refresh/RFM 策略与对比对象一致。
- ECC/CRC/DBI/metadata 是否计入接口带宽有明确口径。
- `--strict-timing-table` 不再报告 vendor-required research default。

配置文件本身不是标准来源；它只是把标准/厂商/研究假设转成仿真输入。做实验记录时，
建议保存配置文件、timing table dump、命令行参数和 git commit。
