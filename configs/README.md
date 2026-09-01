# 配置模型

项目对普通使用者只提供两个主入口：

- `configs/hbm.cfg`：HBM 家族，选择 HBM3 或 HBM4。
- `configs/lpddr.cfg`：LPDDR 家族，选择 LPDDR5 或 LPDDR6。

它们包含系统拓扑、workload、控制器、调度、行策略、地址映射、PHY、
organization、timing、刷新/RFM、ECC、存储后端、功耗、热模型和输出参数。
标准参数只在这两份主配置中定义；其他 cfg 通过 `[meta] extends` 继承主配置：

```text
2 份标准主配置：hbm.cfg、lpddr.cfg
4 份验证配置：validation/hbm3.cfg、hbm4.cfg、lpddr5.cfg、lpddr6.cfg
4 份用例配置：usecases/hbm.cfg、lpddr.cfg、hbm_nstacks.cfg、lpddr_nstacks.cfg
1 份开发配置：developer.cfg
```

`profile_index.csv` 记录参数集的位置、来源和适用边界，`validation/*.csv` 是不参与
模型解析的来源清单。

## 1. 最常用的命令

```bash
# 检查配置，不运行流量
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm4 --check-config

# 运行 HBM3、HBM4、LPDDR5 或 LPDDR6
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm3 --requests 10000
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm4 --requests 10000
./build-clang-debug/hbm_sim --config configs/lpddr.cfg \
  --standard lpddr5 --requests 10000
./build-clang-debug/hbm_sim --config configs/lpddr.cfg \
  --standard lpddr6 --requests 10000

# 多 stack 和自由探索
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm4 --stack-count 6 --scheduler fcfs
```

配置文件里的 `[model] base_standard` 是默认选择；`--standard` 始终覆盖它。
命令行参数无论写在 `--config` 前还是后，都具有最终优先级。

## 2. 配置分层

schema-v2 的固定合并顺序为：

```text
程序内置默认
  < 公共 section（如 [system]、[controller]）
  < [family.*]
  < [standard.<标准>.*]
  < [preset.<标准>.<名称>.*]
  < [override]
  < 命令行
```

常见 section：

| section | 内容 |
| --- | --- |
| `[model]` | 模型名、基础标准、preset |
| `[validation]` | `exploratory/standard/device` 校验模式 |
| `[system]` | stack 数量、跨 stack 映射、入口队列、QoS、响应模式和队列容量 |
| `[workload]` | 请求数、读写比例、trace、随机地址上限、注入/进度和统计视图 |
| `[controller]` | buffer、水位、调度器和行策略 |
| `[controller.scheduler]` | `type = fcfs/frfcfs` |
| `[controller.row_policy]` | `open_page/closed_page/closed_cap` |
| `[mapping]` | DRAM 地址映射和 channel mapper |
| `[phy]` | Direct/Behavioral PHY、DFI、FIFO 和训练流水线 |
| `[architecture]` | 速率、容量、stack height、channel/bank/row/column |
| `[timing.*]` | nCK 或 ns/us timing 及数值来源 |
| `[maintenance]` | refresh、RFM、温度和低功耗策略 |
| `[storage]` | payload backend 与物理拓扑 |
| `[reliability*]` | 接口开销和 payload SECDED |
| `[power]`、`[thermal]` | 功耗与热模型输入参数 |
| `[outputs]` | trace、验证和 dump 路径 |
| `[override]` | 当前研究实验相对 preset 的显式修改 |

例如只想研究一个非标准 HBM4 组织和时序：

```ini
[override]
stack_count = 6
channels = 24
bank_groups = 4
nCL = 28
scheduler = fcfs
```

这会生成一个“以 HBM4 命令/状态语义为基础的自定义模型”，不应称作某个真实
HBM4 器件。建议同步修改 `[model] name`，并保持 `validation_mode = exploratory`。

长任务与小型后端常用的运行项：

```ini
[workload]
pattern = random
random_address_space_bytes = 1048576  # 0 表示模型总容量；非零须按 line_size 对齐
progress_interval = 10000             # 写 stderr；0 关闭
stats_view = summary                  # summary 或 full

[system]
response_delivery_mode = both         # disabled/host/transaction/both
host_response_queue_capacity = 16     # 0 表示无限
transaction_response_queue_capacity = 64

[outputs]
response_trace = outputs/host.csv
transaction_response_trace = outputs/transactions.csv
```

`random_address_space_bytes` 限制的是多 Stack 聚合系统地址范围。使用容量较小的
`mmap_sparse/chunk_file` 实验后端时应显式给出，以免默认 full-model 随机地址超出
实验文件容量。响应 trace 会自动推导所需模式；若又显式写了不兼容模式，配置检查会失败。

## 2.1 配置继承、验证集和四个用例

```bash
./build-clang-debug/hbm_sim --config configs/validation/hbm4.cfg \
  --preset ramulator2_reference_1ch --check-config
./build-clang-debug/hbm_sim --config configs/usecases/hbm_nstacks.cfg \
  --stack-count 4 --check-config
```

继承规则示例：

```ini
[meta]
extends = ../hbm.cfg  # 相对当前 cfg 所在目录解析
```

解析器先加载基础配置，再加载当前文件；循环继承会直接报错。覆盖优先级先看 section 层级，
只有在同一层级内才由子文件覆盖父文件，所以用例差异应写在 `[override]`，不要写进普通
`[system]` 去对抗父配置的 `[override]`。`--dump-resolved-config`
可展开真正执行的全部值。HBM/LPDDR 主配置不再要求 `baseline` preset；选择标准即可运行。
`ramulator2_reference_1ch`、`dramsim3_hbm2_common` 等只存在于四份验证配置，合成
9000 Mbps 和链路保护压力参数只存在于 `developer.cfg`。

四个可直接运行的 cfg：

```bash
./build-clang-debug/hbm_sim --config configs/usecases/hbm.cfg
./build-clang-debug/hbm_sim --config configs/usecases/lpddr.cfg
./build-clang-debug/hbm_sim --config configs/usecases/hbm_nstacks.cfg
./build-clang-debug/hbm_sim --config configs/usecases/lpddr_nstacks.cfg
```

兼容的四标准包装脚本仍可使用，它们统一读取两份多实例用例配置：

```bash
bash examples/multistack_demos/hbm3_nstack.sh
bash examples/multistack_demos/hbm4_nstack.sh
bash examples/multistack_demos/lpddr5_nstack.sh
bash examples/multistack_demos/lpddr6_nstack.sh
```

## 3. 基础共有与协议特有参数

共有部分包括系统拓扑、流量、controller buffer、调度、行策略、地址映射、
Behavioral PHY、通用 organization/timing、存储后端、payload ECC、功耗和热模型。

HBM 特有部分包括 SID/PC interleave、双命令总线相关 pairing、link CRC/RAS
metadata，以及 stack/layer/TSV 热拓扑。LPDDR 特有部分包括 split ACT、WCK、
DVFS、DBI、link protection/ECC、CA parity、REFdb 和低功耗行为。

另一协议族的字段可以安全地放在 inactive 的 `[standard.*]` 或 `[preset.*]`
section 中；如果把 `lpddr_*` 键放进正在运行的 HBM `[override]`，程序会直接报
“没有可执行语义”，避免静默忽略。

## 4. 可配置不等于已实现

数值、容量和布尔参数只要在配置白名单中，就可以自由改变；算法名称则必须有
C++ 实现。配置文件不是脚本，不会根据一个陌生字符串自动生成调度器、行策略、
PHY 或存储后端。

当前注册项：

| 类型 | 已实现值 |
| --- | --- |
| scheduler | `fcfs`、`frfcfs` |
| row policy | `open_page`、`closed_page`、`closed_cap` |
| memory backend | `sparse`、`mmap_sparse`、`chunk_file` |
| PHY mode | `direct`、`behavioral` |

查询运行程序自身的注册表：

```bash
./build-clang-debug/hbm_sim --list-schedulers
./build-clang-debug/hbm_sim --list-row-policies
./build-clang-debug/hbm_sim --list-backends
./build-clang-debug/hbm_sim --list-phy-modes
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 --list-presets
```

例如 `type = blis` 而代码没有 BLISS 实现时，配置解析立即失败，错误会带配置
文件和行号，并列出 `fcfs, frfcfs`。这比回退到默认策略更安全，因为结果不会在
用户不知情时使用错误算法。要增加新策略，需要实现枚举/解析、候选排序逻辑、
帮助/注册列表和测试，然后该名称才能出现在配置中。

## 5. 三种可信度模式

| 模式 | 用途 | 行为 |
| --- | --- | --- |
| `exploratory` | 自由架构和参数探索 | 允许偏离 preset；仍拒绝未知算法、无语义字段和非法范围 |
| `standard` | 标准级基线 | 拒绝 `[override]`/CLI 对标准模型参数的偏离；拒绝 research/external timing |
| `device` | 目标器件数值比较 | 包含 standard 检查，并要求具名 `vendor_profile` 和 vendor 来源 timing |

当前 HBM4 的若干 RL/WL 和行时序仍是 research-default，因此它能在
`exploratory` 模式运行，但不能通过 `standard`。HBM3/LPDDR5 的部分基线来自
Ramulator2.1，标为 `external_reference`，同样不能冒充 JEDEC 或厂商数据。
LPDDR6 主配置的当前标准项可以用于 JEDEC 级检查，但 `device` 仍需厂商参数。

## 6. timing 单位和来源

- `nXXX` 是 nCK，例如 `nCL = 30`。
- `tXXX_ns`、`tXXX_us` 由最终 `tCK_ps` 向上取整换算成 nCK。
- `source`/`timing_override_source` 可取 `jedec`、`vendor`、`derived`、
  `external_reference`、`research_default`。
- `external_reference` 表示来自 Ramulator/DRAMsim 等参考实现，不等于标准或器件值。
- 固定 JEDEC/vendor 表必须记录 speed bin、density、stack height/mode 和来源说明。

建议把不同来源放在独立 subsection：

```ini
[standard.hbm4.timing_jedec]
source = jedec
tCCDL_ns = 2.5

[standard.hbm4.timing_research]
source = research_default
nCL = 30
```

## 7. 审计最终生效模型

```bash
# 导出包含内置值、选中标准/preset、公共段、override 和 CLI 的最终快照
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --stack-count 6 --dump-resolved-config outputs/hbm4_resolved.cfg --check-config

# 查看相对 preset 的修改
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --speed-bin-mbps 9000 --compare-preset --check-config

# 解释某个配置文件字段的覆盖链
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --explain-config nCL --check-config
```

`--dump-resolved-config` 的结果可再次作为 `--config` 加载；timing 按来源分组，
因此重新加载后不会丢失 JEDEC/vendor/research provenance。`--check-config` 只完成
解析、分层、模型构造和一致性校验，不产生流量。

## 8. 后端、ECC、功耗和热模型

- `sparse` 只为访问过的 transaction 分配内存，适合通常的仿真。
- `mmap_sparse` 提供 mmap 文件后端。
- `chunk_file` 用有界 chunk cache 访问文件后端。
- `[reliability.payload]` 是实际 payload 上的 SECDED shadow；接口 ECC/metadata
  统计位于 `[reliability]`，二者不是同一层。
- `configured_pj` 使用逐命令能量；`dramsim3_idd` 使用 VDD/IDD 风格公式。
- HBM 配置采用 stack/layer/TSV 参数解释热拓扑；LPDDR 主配置采用较薄的
  device/package 拓扑参数。两者目前仍是行为级研究模型，需要厂商 IDD/封装参数校准。

完整可用键直接查看两个主配置中的注释；CLI 等价项查看 `hbm_sim --help`。

## 9. 自定义实验的定位

解析器仍能读取旧式平面 `key=value`，但仓库不再维护这种副本。普通自定义模型应修改
两份主配置的 `[override]` 或使用 CLI；需要长期保存的实验可以新建一个带 `extends`
的短配置。验证条件进入四份 validation cfg，非标准压力参数进入 `developer.cfg`，不要
把它们重新放回标准 section。仓库测试要求 `configs/` 恰好保留公开的 11 份 cfg，因此个人
实验文件应放在 `experiments/<名称>/` 或仓库外。每次运行都应用
`--dump-resolved-config` 保存实际生效模型。

## 10. 从头编写一份 cfg

新文件通常不需要重写几百个默认值，只要继承对应家族并写差异。下面每个有效配置行都带
同行注释，也是仓库测试要求的格式：

```ini
[meta]
schema_version = 2  # 本解析器的配置语法版本；不代表 HBM/LPDDR 协议版本
extends = ../hbm.cfg  # 父配置路径；相对当前 cfg 文件解析

[model]
name = my_hbm4_case  # 本次模型的可读名称，不参与算法选择
base_standard = hbm4  # 基础标准；继承 hbm.cfg 时可选 hbm3/hbm4

[override]
requests = 4096  # Host 请求数量；一个请求可能拆成多个 DRAM transaction
seed = 11  # 随机数种子；相同配置和种子产生相同随机请求序列
stack_count = 2  # 独立存储实例数；每个实例有独立 ingress/后端/热状态
stack_mapping = interleaved  # 系统地址按固定条带轮转到各 Stack
stack_interleave_bytes = 256  # 地址条带大小，Byte；须与 line/transaction 粒度兼容
mem_phy_mode = behavioral  # 启用带状态、FIFO、流水和 DFI event 的行为级 PHY
dfi_version = 6.0.1  # 接口语义参考标签；本身不启用功能，也不表示 DFI 合规
```

若文件位于仓库外，`extends` 可写主配置的绝对路径；若需要便携，应保持相对路径并把文件放
在稳定目录。HBM3/HBM4 不能继承 `lpddr.cfg`，LPDDR5/LPDDR6 不能继承 `hbm.cfg`。
标准参数的长期修改应放在自己的 cfg；临时扫描可用 CLI。推荐执行：

```bash
./build-clang-debug/hbm_sim --config path/to/my_case.cfg \
  --check-config --dump-resolved-config outputs/my_case/resolved.cfg
```

重点核对 `selected_standard`、`selected_preset`、`timing_profile`、`timing_source`、
`stack_count`、`controller_count`、`mem_phy_mode`、刷新策略、后端和热模型。`seed` 只固定
伪随机流量，不会固定线程调度或外部仿真器版本；本项目核心是单线程确定性事件循环，因此
在同一二进制和配置下主要用于复现实验、定位首个异常事件和公平比较两个参数方案。

## 11. 验证配置与外部仿真器的关系

`configs/validation/*.cfg` 是本程序的对齐配置，不是 Ramulator2/DRAMsim3 配置。真正差分时：

```text
同一场景定义
  ├─ hbm_sim validation cfg → 启动 hbm_sim → hbm_sim trace/stats
  └─ 外部工具生成参考配置 → 启动 Ramulator2/DRAMsim3 → reference trace/stats
                                         ↓
                           规范化共同字段后再比较
```

报告必须记录 `external_engine_executed=true`、外部可执行文件、Git commit/dirty 状态和双方
原始产物。仅在 hbm_sim 上运行一个名为 `ramulator2_reference_1ch` 的 preset，只能证明本项目
能加载共同参数面，不能称为外部差分。参考模型不支持的 HBM4/LPDDR6、PHY、真实 payload、
ECC、功耗热或多 Stack 特性应标记为 unsupported，不能为了数值相同而删掉。
