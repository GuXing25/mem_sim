# hbm_sim 验证与校准路线

本文档回答一个核心问题：在缺少 HBM4/LPDDR6 官方厂商器件模型和真实硬件测量
的情况下，`hbm_sim` 如何建立可信度；取得外部模型或硬件数据后，又应该怎样把
它们接入项目。

结论先行：

```text
hbm_sim 需要一条验证与校准证据链。
但当前阶段不必把目标定为“一定拿到 HBM4/LPDDR6 官方 vendor model”。
更现实、更稳妥的路线是：

项目内部正确性
  -> 标准/手册一致性
  -> DRAMsim3/Ramulator2 共同面差分
  -> 公开 datasheet/IDD/论文/工具校准
  -> 真实硬件测量或厂商模型校准
```

只有完成最后一层，才能把某个配置称为 `vendor-calibrated` 或
`hardware-calibrated`。在此之前，HBM4/LPDDR6 相关实验应明确称为
`JEDEC-oriented research model` 或 `research_default calibrated model`。

## 1. 为什么需要这份文档

DRAMsim3 和 Ramulator2.1 的说服力并不只来自“代码能跑”。它们分别有不同的
验证来源：

- DRAMsim3 有成熟配置、性能/功耗/热模型路径，以及把命令 trace 转成 Micron
  Verilog model testbench 的外部验证入口。它的 README 中说明了 Verilog
  Validation 路径，本地参考在 `/home/wsl/test/DRAMsim/DRAMsim3/README.md`。
- Ramulator2.1 有系统的 smoke、latency-throughput、device_timings 和
  controller_scheduling 回归。它的 README 明确说明这些测试分别回答能否运行、
  性能形状是否合理、device timing 是否正确、controller 是否发出正确命令，本地
  参考在 `/home/wsl/test/ramulator2-2.1/README.md`。

但这不等于两者都自带 HBM4/LPDDR6 官方厂商模型或真实硬件校准。对
`hbm_sim` 来说，正确做法不是简单追求“和某个模拟器完全一样”，而是把证据分层：

```text
哪些由项目内部测试证明
哪些由 JEDEC/DFI 手册约束
哪些由 DRAMsim3/Ramulator2 差分支撑
哪些来自公开厂商/器件资料
哪些仍只是研究默认值
```

## 2. 可信度分层

| 层级 | 名称 | 证明对象 | 当前状态 | 可声明结论 |
| --- | --- | --- | --- | --- |
| V0 | 构建与 smoke | 程序能构建、能跑完基本请求 | 已有 | 软件可运行 |
| V1 | 数据正确性 | payload 写入、读回、mask、golden、后端持久化 | 已有 | 存储语义自洽 |
| V2 | 命令/时序正确性 | ACT/RD/WR/PRE/REF/RFM 等命令前置条件和 timing gate | 已有，持续补充 | 按项目配置表执行 |
| V3 | 标准一致性 | HBM4/LPDDR6 可见组织、命令、burst、refresh、DFI 抽象 | 部分已有 | JEDEC/DFI-oriented |
| V4 | 外部模拟器共同面 | 与 DRAMsim3/Ramulator2 在共同场景下对齐或差异可解释 | 部分已有 | 共同面可对照 |
| V5 | 外部模型校准 | 与 Micron/厂商 Verilog、DRAMPower、HotSpot/3D-ICE 等外部模型对齐 | 待补 | 外部模型校准 |
| V6 | 真实硬件校准 | 与真实板卡/器件的延迟、带宽、功耗、温度测量对齐 | 待补 | 硬件校准 |

当前项目最稳妥的声明是：

```text
hbm_sim 已具备真实 payload、文件后端、golden 验证、命令 trace、DFI-oriented trace、
bank/row/tile/grid 物理投影、事件级功耗和稀疏热趋势模型。

HBM4/LPDDR6 配置是标准导向和研究默认值混合模型。
它不是某个厂商料号的官方器件模型。
```

## 3. 不能混淆的四种“正确”

### 3.1 数据正确

判断标准：

```text
W addr payload
R addr actual == expected
mask write 只改变 mask 覆盖字节
未初始化读按项目规则统计
后端关闭重开后数据仍可读回
```

对应证据：

- `final_memory.txt`
- `final_memory.csv`
- `mismatch_report.txt`
- `data_checked_reads`
- `data_mismatches`
- `data_write_commits`
- `storage_lines_allocated`
- `unique_written_lines`

这是 `hbm_sim` 的强项，也是 DRAMsim3/Ramulator2 通常不作为核心目标的部分。

### 3.2 控制器/时序正确

判断标准：

```text
命令顺序满足 bank state
RD/WR 不能早于 ACT 后 timing
PRE/REF/RFM 不能破坏数据
row hit/miss/conflict 统计合理
FR-FCFS/FCFS 调度行为可解释
```

对应证据：

- `cmd_trace.csv`
- command validator
- sequence tests
- `avg_read_latency`
- `row_hits / row_misses / row_conflicts`
- `read_queue_len_avg / write_queue_len_avg / active_queue_len_avg`

### 3.3 标准语义正确

判断标准：

```text
HBM4/LPDDR6 的组织层级、命令类别、burst 粒度、refresh/RFM、
DFI MC/PHY 边界事件与手册抽象一致。
```

注意：标准语义正确不等于厂商校准。JEDEC 定义的是标准允许的行为和参数表，
真实器件还会有 speed-bin、mode register、温度、电压、封装和厂商 guardband。

### 3.4 器件级数值正确

判断标准：

```text
指定厂商
指定料号
指定 density / stack height / speed-bin
指定温度、电压、模式寄存器和刷新条件
仿真 latency / bandwidth / power / thermal 与 vendor model 或实测数据对齐
```

当前项目尚未达到这一层。

## 4. 当前项目已有证据

### 4.1 内部验证

推荐保存以下输出作为一次实验的证据包：

```bash
make test

python3 tools/model_validation.py \
  --json-out outputs/hbm_sim_model_validation.json
```

关键证据：

- 构建和 smoke 通过。
- sequence test 覆盖短命令路径。
- DFI validator 检查 command/data beat、latency、phase、payload、mask、source。
- `MemoryImage` 检查真实 payload 和 golden。
- `mmap_sparse`、`chunk_file` 能验证文件后端持久化。
- bandwidth regression 使用固定门槛防止性能形状无声退化。

### 4.2 Ramulator2.1 共同面对照

推荐保存：

```bash
make reference-validation \
  RAMULATOR2_ROOT=/home/wsl/test/ramulator2-2.1
```

该对照只比较共同面：

```text
HBM4-like timing
ACT/RD/WR/PRE 等核心命令
短序列 command issue cycle
decoded channel/PC/SID/BG/bank/row/column
row hit / conflict 行为
```

不比较：

```text
真实 payload
文件后端
DFI payload trace
ECC shadow
floorplan/tile/grid/cell
功耗/热趋势
HBM4/LPDDR6 厂商私有参数
```

差分结果应表述为：

```text
共同命令面在固定场景下可对照，偏差在已解释阈值内。
```

不要表述为：

```text
hbm_sim 与 Ramulator2.1 完全一致。
```

### 4.3 DRAMsim3 参考价值

DRAMsim3 适合作为以下方向的参考：

- 配置文件组织和 power/timing 字段设计。
- 事件级功耗和热模型路径。
- command trace 生成和外部 Verilog testbench 的思想。
- 对 DDR3/DDR4/LPDDR 类公开 Micron Verilog 模型的验证入口。

DRAMsim3 当前不是 HBM4/LPDDR6 官方校准源。用于本项目时应写成：

```text
借鉴 DRAMsim3 的验证思路、功耗/热建模结构和外部 Verilog 对接路径。
```

而不是：

```text
HBM4/LPDDR6 已由 DRAMsim3 校准。
```

## 5. 建议新增的证据包

为了让项目更有说服力，建议每次重要实验保存一个目录：

```text
results/<date>_<experiment>/
  config.cfg
  stats.txt
  cmd_trace.csv
  dfi_trace.csv
  dfi_signal_trace.csv
  final_memory.csv
  mismatch_report.txt
  thermal_map.txt
  model_validation.json
  ramulator2_diff.json
  source_manifest.csv
  notes.md
```

`notes.md` 至少记录：

```text
实验目标
标准和配置
数据来源标签
是否开启 refresh/RFM
是否开启 DFI dump
是否开启 power/thermal
外部参考版本
可声明结论
不可声明结论
```

## 6. 外部模型接入路线

### 6.1 第一阶段：共同面模拟器差分

目标：

```text
证明 hbm_sim 的基本 timing、bank state、controller scheduling 不偏离成熟模拟器太多。
```

推荐对象：

- Ramulator2.1：HBM4/HBM3/LPDDR5 的 command/timing/controller 差分。
- DRAMsim3：DDR4/HBM2/LPDDR4 等较老标准的 latency、throughput、command trace 对照。

输出：

```text
diff report
命令序列对比
latency/bandwidth 曲线
偏差原因说明
```

### 6.2 第二阶段：公开 datasheet / IDD 校准

目标：

```text
把 timing、功耗和接口带宽参数从 research_default 提升到 vendor 或 derived。
```

需要记录：

```text
vendor
part number
document name
document revision
table/page
density
speed-bin
temperature
voltage
mode register setting
```

功耗优先补：

```text
VDD / VDDQ / VPP
IDD0 / IDD2N / IDD3N / IDD4R / IDD4W / IDD5B / IDD6
tRC / tRFC / burst length
read/write energy per byte
background power
refresh power
```

### 6.3 第三阶段：外部功耗/热工具对照

可选参考：

- DRAMPower 类功耗模型。
- HotSpot、3D-ICE 或其他公开热求解器。
- DRAMsim3 thermal solver 的输入/输出结构。

目标不是完全替代当前模型，而是建立两类结果：

```text
事件能量总量是否在合理范围
热点位置和温升趋势是否与外部工具同向
```

当前 `hbm_sim` 热模型仍应声明为：

```text
sparse RC trend model
```

不能声明为：

```text
vendor package calibrated 3D thermal solver
```

### 6.4 第四阶段：真实硬件测量

如果能拿到硬件，建议从容易测量的指标开始：

```text
unloaded read latency
stream read bandwidth
stream write bandwidth
mixed read/write bandwidth
refresh-on/off 或温度模式影响
功耗曲线
外壳/板级温度趋势
```

需要同步记录：

```text
平台型号
内存型号或至少 memory generation
频率
通道数
容量
BIOS/firmware 设置
OS/kernel
benchmark 工具和版本
环境温度
采样方法
```

真实硬件数据通常无法直接给出 bank/row 级事件，因此它主要校准系统级
latency/bandwidth/power/thermal，而不是替代 command-level validator。

## 7. HBM4/LPDDR6 的当前校准边界

### 7.1 HBM4

当前可以较有把握声明：

```text
组织层级、pseudo-channel、SID、bank group、bank、row、column 在项目模型中显式存在。
HBM4-like row/column command bus、burst、DFI beat、payload 和存储后端可以联动。
```

当前不能声明：

```text
某个 HBM4 vendor part 的真实 RL/WL、IDD、ECC/RAS、TSV/floorplan、热阻网络已经校准。
```

最需要补的数据：

```text
RL/WL table
tRCD/tRP/tRAS/tRC/tRRD/tFAW/tWTR/tRTW
refresh/RFM threshold and recovery
VDD/VDDQ and IDD
ECC/RAS/link retry behavior
stack layer/floorplan/TSV/package thermal parameters
```

### 7.2 LPDDR6

当前可以较有把握声明：

```text
LPDDR6-oriented subchannel、BL24、WCK/CAS、DVFS、low-power、link protection
入口在模型中存在，部分 timing 来自标准分支或公式。
```

当前不能声明：

```text
某个 LPDDR6 vendor part 的完整 MR 编程、training、link ECC/DBI、低功耗流程和功耗曲线已校准。
```

最需要补的数据：

```text
RL/WL set 与 mode register bit
WCK ratio/training/retraining 条件
DVFS 切换流程
DBI/link ECC/CA parity 真实编码和开销
IDD/power state 电流
温度相关 refresh 和 self-refresh 行为
```

## 8. 接收标准建议

后续新增校准项时，建议使用以下门槛。

| 项目 | 建议门槛 | 说明 |
| --- | --- | --- |
| command legality | 0 错误 | 错一个就是状态机或时序 bug |
| DFI payload beat | 0 mismatch | payload 是项目核心能力 |
| golden data | 0 mismatch | 数据正确性不接受统计误差 |
| unloaded latency | 与公式完全一致或差异有固定解释 | cycle-level 模型应可解释 |
| simulator differential | 固定短序列差异 <= 约定阈值 | 阈值必须写原因 |
| sustained bandwidth | 不超过理论峰值，不低于回归门槛 | 防止性能退化 |
| power estimate | 与外部模型/数据同量级，误差目标分阶段设置 | 初期可 20%-30%，校准后收紧 |
| thermal trend | hotspot 位置/相对趋势一致 | 无封装参数前不承诺绝对温度 |
| hardware latency/bandwidth | 先目标 10%-20%，再按数据质量收紧 | 真实系统包含 CPU/cache/OS 干扰 |

## 9. 来源标签规则

项目中所有参数应尽量落入以下四类：

| 标签 | 含义 | 可以支撑的声明 |
| --- | --- | --- |
| `jedec` | 来自 JEDEC/DFI 标准表、固定规则或所选标准模式 | 标准导向 |
| `derived` | 由已知参数和公开公式推导 | 可信度继承输入 |
| `research_default` | 项目为了可运行或敏感性研究设置 | 只能做研究假设 |
| `vendor` | 来自可追溯厂商资料或实测校准 | 可声明对应料号/条件已校准 |

严禁把以下情况标成 `vendor`：

```text
来自其他模拟器默认配置
来自论文中的示例值但没有对应器件条件
来自经验估算
来自标准允许范围但没有目标料号确认
```

## 10. 推荐文档表述

可以写：

```text
hbm_sim 的真实 payload、MemoryImage、文件后端和 golden 验证已经通过项目原生测试。
HBM4/LPDDR6 timing 使用 JEDEC-oriented 配置和显式 research_default 标签。
控制器共同命令面与 Ramulator2.1 在固定短序列上进行差分验证。
功耗和热模型当前用于趋势分析，尚未完成目标器件/封装校准。
```

不要写：

```text
hbm_sim 已经等价于真实 HBM4/LPDDR6 器件。
hbm_sim 已经通过厂商模型认证。
功耗和热模型已经达到厂商级精度。
DFI 已经实现完整 PHY pin-level 协议。
```

## 11. 近期实施步骤

建议按这个顺序补强项目说服力：

1. 固化每次实验的证据包目录和 JSON/CSV 输出。
2. 在 `configs/validation/vendor_parameters.csv` 中继续补齐每个
   `research_default` 字段的缺口说明。
3. 扩大 Ramulator2.1 差分场景：row hit、row conflict、refresh、write drain、
   HBM4 dual-issue、LPDDR5/LPDDR6-like WCK 路径。
4. 为 DRAMsim3 增加一个较老共同标准的对照实验，例如 DDR4/HBM2 的命令和带宽曲线。
5. 选择一个公开资料较充分的校准锚点，例如 DDR4、LPDDR5 或 HBM2，把 timing 和功耗
   参数做成 `vendor` profile。
6. 把功耗从固定 pJ 默认值逐步升级为 IDD/VDD/timing 派生模型。
7. 把热模型输出与外部工具或文献算例做趋势对照。
8. 如果取得硬件，建立 latency/bandwidth/power/temperature 的实测数据集。

## 12. 最小可信声明包

如果当前要对外介绍项目，建议至少附带：

```text
README.md
文档/审计.md
文档/审计.md
VALIDATION_AND_CALIBRATION.md
configs/validation/vendor_parameters.csv
一次 make test 输出
一次 model_validation.json
一次 ramulator2_diff.json
一个 final_memory.csv + mismatch_report.txt
```

这样项目的可信度就不是靠口头说明，而是靠可复现证据链支撑。

