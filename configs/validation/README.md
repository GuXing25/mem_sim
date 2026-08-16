# 验证配置与证据说明

本目录不是新的器件 speed-bin，而是模型验证口径。它回答两个不同问题：

1. 项目原生 HBM4 数据、DFI、ECC 和存储路径是否内部一致。
2. 在统一组织和 workload 后，项目与 Ramulator2.1 参考模型的命令/周期趋势
   是否可比较。

这两类验证都不能替代真实 HBM4 器件测量。

当前还包含 HBM3/HBM4/LPDDR5/LPDDR6 四个 Ramulator reference 配置，以及
`dramsim3_hbm2_common.cfg`。后者只把 DRAMsim3 HBM2 的公共 ACT/PRE/RD/WR/REF 和
IDD 输入映射到项目状态机，明确不是 HBM3 产品 profile。

## 1. `hbm4_native_1ch.cfg`

该文件加载 `profiles/hbm4/jedec_8000_32gb_8hi.cfg`，但把运行范围缩为一个
channel，适合项目原生功能验证。

### 1.1 组织与传输

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `channels` | 1 | 只模拟一个 channel，不是完整 32-channel stack |
| `pseudo_channels` | 2 | 两个 PC |
| `sids` | 2 | 8Hi 的两个 SID slice |
| `bank_groups` / `banks_per_group` | 2 / 8 | 每 PC 16 banks |
| `rows` / `columns` | 16384 / 32 | 验证用地址组织 |
| `line_size` | 32 B | 前端请求和 HBM 事务都按一个 PC BL8 payload |
| `data_bus_bits` | 64 | 一个 channel 的两个 32-bit PC 合计 |

profile 已设置 `dram_transaction_bytes = 32`，所以该配置的逻辑容量为：

```text
1 × 2 × 2 × 1 × 2 × 8 × 16384 × 32 × 32 B = 1 GiB
```

该 1 GiB 是“单 channel 验证切片”的项目地址空间，不代表完整 8Hi stack 容量。

### 1.2 控制器隔离条件

- `single_controller = true`：所有请求进入一个控制器实例。
- `scheduler = frfcfs`：FR-FCFS 调度。
- `row_policy = open_page`：保持命中行打开。
- `address_mapping = default`、`channel_mapper = decoded`：使用直接解码映射。
- read/write buffer 均设为 16384，降低小型差分测试中的队列反压干扰。
- `requests = 0`：由 trace 决定请求数。
- `max_cycles = 2000000`：防止错误场景无限运行。

### 1.3 关闭维护的原因

`supports_refresh = false`、`supports_rfm = false` 用于隔离普通
ACT/RD/WR/PRE 序列。否则两个模型的刷新插入点或 RFM 策略差异会掩盖基础命令
对比。这不是说 HBM4 不需要 refresh/RFM。

### 1.4 DFI 与数据正确性

| 参数 | 当前值 | 作用 |
|---|---:|---|
| `dfi_phase_count` | 2 | 项目 DFI 抽象的 phase 数 |
| `dfi_data_lane_bytes` | 16 B | 一个 DFI data beat 携带 16 B |
| `dfi_read_latency_nck` | 30 | RD 命令到读数据相位 |
| `dfi_write_latency_nck` | 10 | WR 命令到写数据相位 |
| `memory_backend` | sparse | 使用内存稀疏 payload 后端 |
| `ecc_shadow` | true | 维护 ECC shadow metadata |
| `ecc_check_on_read` | true | 读回时执行项目 ECC shadow 检查 |

32 B transaction 会生成两个 16 B DFI data beat。这里的 latency/phase 是项目
MC/PHY trace 抽象，需要真实 PHY/vendor 数据才能校准。

## 2. `hbm4_reference_1ch.cfg`

该文件加载 Ramulator2.1 HBM4 profile，保留与 native 配置相同的单 channel
组织、32 B 请求、FR-FCFS、open-page 和大队列，以便做受控差分。

关键差异：

| 项目 | native | reference | 原因 |
|---|---|---|---|
| timing profile | JESD-oriented 混合来源 | Ramulator2.1 baseline | 比较两套 timing |
| `dfi_read_latency_nck` | 30 | 20 | 分别保留项目/参考口径 |
| ECC | 开启 | 关闭 | reference 先隔离协议时序 |
| floorplan | 开启 | 关闭 | reference 不比较项目物理映射 |
| memory backend | 显式 sparse | 默认 | reference 关注命令差分 |

差分时不能把总周期差全部归因于“谁更准确”；首先要区分 timing table、DFI
latency、ECC 和项目扩展是否已统一。

## 3. `vendor_parameters.csv`

该清单记录“真实 vendor 数据缺口”，每列含义如下：

| 列 | 说明 |
|---|---|
| `area` | 参数所属领域 |
| `parameters` | 具体参数或参数组 |
| `current_source` | 当前项目使用 JEDEC、derived 还是研究默认值 |
| `validation_status` | 当前能做的验证方法 |
| `vendor_data_needed` | 器件级结论是否仍需要 vendor 数据 |
| `claim_boundary` | 目前允许写进论文/报告的结论边界 |

十类缺口包括：

- RL/WL 与 RCD。
- RP/RAS/RC/RTP/WR/RRD/FAW。
- BL/CCD/WTR/RTW 的模式确认。
- density/温度分支相关刷新。
- RAA/RFM/PRAC。
- ECC/CRC/retry/syndrome/scrub。
- DFI/WCK/DVFS/training。
- VDD/IDD/每命令能量。
- 封装、材料、TSV、散热边界。
- subarray/mat/cell/microbump 几何。

`validation_status` 表示已验证模型路径或敏感性，不表示已经获得真实器件精度。

## 4. `project_identity.csv`

该表记录即使以后接入 vendor 数据也应保留的项目原创能力：

- MemoryImage 的真实 payload、init mask 和读写正确性。
- sparse、mmap_sparse、chunk_file 三种可替换存储后端。
- 完整 HBM stack 统计口径。
- 带真实 payload 的 DFI trace 和 validator。
- die/layer/tile/subarray/mat/cell/microbump 逻辑坐标。
- 命令能量和 TSV-aware 稀疏 3D RC 热模型。
- JEDEC/vendor/derived/research-default provenance 治理。
- HBM3/HBM4/LPDDR5/LPDDR6 的专注范围。
- streaming/burst 前端和数据正确性检查。

`reference_relation` 用于说明该能力是独立模型、项目扩展，还是比单 channel
参考范围更广；它不是“参考模拟器已验证该能力”的声明。

## 5. 推荐验证流程

先做项目回归：

```bash
make test
```

检查模型来源和内部公式：

```bash
./build/model_validation
```

检查 native timing provenance：

```bash
./build/hbm_sim --config configs/validation/hbm4_native_1ch.cfg \
  --strict-jedec \
  --dump-timing-table outputs/hbm4_native_timing.csv
```

运行 reference 配置时，应使用与 native 相同的 trace、请求数和停止条件，再比较：

```text
命令序列与合法性
row-hit/row-miss
读写完成数
system_cycles
平均/尾延迟
有效 payload 带宽
```

## 6. 能说明到什么程度

| 证据 | 可以说明 | 不能说明 |
|---|---|---|
| 单元/序列/烟雾测试 | 软件路径和已编码约束一致 | 真实芯片 timing 正确 |
| timing provenance 审计 | 每个值的来源可追踪 | research default 自动变成 vendor 值 |
| 公式/容量/带宽检查 | 项目口径内部自洽 | 封装和 PHY 达到标称带宽 |
| Ramulator2.1 差分 | 相同条件下趋势和命令差异 | 两者任一等于实物 |
| JEDEC 表对照 | 标准接口和公开 timing 条件一致 | 厂商实现相关 RL/WL/RAS/功耗 |
| vendor 手册/实测校准 | 可支持目标器件结论 | 自动推广到其他 density/speed-bin |

因此本目录支持“可运行、可审计、内部一致、可与参考模型受控比较”的声明；
在真实 vendor 表和硬件测量缺失时，不支持“已经精确复现某款 HBM4 器件”。
