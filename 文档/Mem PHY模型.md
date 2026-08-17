# Mem PHY 模型

## 1. 项目中的位置

```text
配置 / Frontend
       |
MemorySystem（stack 路由、channel 分发、QoS/反压）
       |
Controller / MC（JEDEC 命令推导、调度、timing、refresh/RFM）
       |
MemPhy（DFI 生命周期、FIFO、训练、协议编码、数据流水线）
       |
MemoryImage / Mem Stack（payload、行缓冲、物理坐标、功耗、热、ECC）
```

因此本项目所称“堆叠存储模型”由 **Mem PHY + Mem Stack** 构成；完整项目是
**配置 + MC + Mem PHY + Mem Stack + 输出/验证**。`MemorySystem` 为每个
`[stack][channel]` Controller 建立独立 PHY；同一 stack 的各通道仍连接该 stack
自己的 `MemoryImage`，不同 stack 不共享数据或 PHY 状态。

## 2. 两种模式

`mem_phy_mode=direct` 是兼容路径。它保留历史行为：Controller 直接在原有完成点
访问 `MemoryImage`，适合复现增加 PHY 前的结果。它不是一个 PHY 实现。

`mem_phy_mode=behavioral` 是在线行为级 PHY。命令必须通过 PHY 状态和 command
FIFO，写数据在 CWL/PHY 流水线后提交，读数据在 CL+BL/PHY 返回流水线后返回 MC。
Controller 只有取得 `MemPhyCompletion` 后才统计完成。读写重叠顺序也会保护，避免
尚在 PHY 中的写被后到读越过。

四个主要运行配置 `hbm3.cfg`、`hbm4.cfg`、`lpddr5.cfg`、`lpddr6.cfg` 已默认选择
Behavioral；命令行可用 `--mem-phy direct` 覆盖。

## 3. 公共核心与协议适配器

公共 `MemPhy` 实现：

- Reset → Initialization → Training → Ready 生命周期；
- command/read/write FIFO 及反压；
- command、read-return、write-data 流水线；
- 异步读写完成返回；
- DVFS 后 LPDDR WCK retraining debt；
- 实际数据完成拍回填到 DFI trace；
- PHY 命令、CA edge、FIFO 峰值、训练和服务周期统计。

`HbmPhyAdapter` 保留 HBM 独立 row/column path、DFI phase 和 edge-pairing 摘要，
用于 HBM3/HBM4。`LpddrPhyAdapter` 保留统一 CA、ACT1/ACT2、CAS/WCK、WCKTRAIN
和 DVFS 事件，用于 LPDDR5/LPDDR6。JEDEC bank/timing legality 仍由 Controller
负责，适配器不会重复计算 tRCD、tFAW、refresh 或 RFM。

## 4. DFI 版本口径

DFI Group 官方公开页面在 2026-05-26 宣布的是 **DFI 6.0**，并说明 6.0 首次加入
HBM、继续支持最新 LPDDR，同时定义 MC 与 PHY 之间的 signals、timing 和
functionality：<https://ddr-phy.org/?overrideMobileRedirect=1>。

配置允许写 `dfi_version=6.0.1`，用于记录项目要求的目标修订；但截至本次实现，官方
公开下载列表只明确列出 6.0，项目没有获得可逐条审计的 6.0.1 全文。因此准确表述是：

> DFI 6.0/6.0.1-oriented 的周期级行为模型，不是 DFI pin-level 合规实现或认证。

当前模型覆盖公开可确认的接口职责、PHY-independent training、频率变化、低功耗、
HBM/LPDDR 协议适配和 command/data 时序类别。未覆盖厂商 lane mapping、deskew、
Vref、模拟 I/O、电气训练收敛、lane repair 和封装信号完整性。

## 5. 配置项

| 配置 | 含义 |
|---|---|
| `mem_phy_mode` | `direct` 或 `behavioral` |
| `dfi_version` | 输出中的目标版本标签 |
| `phy_command_fifo_depth` | command FIFO 深度 |
| `phy_read_fifo_depth` / `phy_write_fifo_depth` | 读/写数据 FIFO 深度 |
| `phy_command_pipeline_cycles` | MC→PHY 命令流水线 nCK |
| `phy_read_return_pipeline_cycles` | 器件读数据→MC 的附加 nCK |
| `phy_write_data_pipeline_cycles` | MC 写数据→器件的附加 nCK |
| `phy_reset_cycles` | Reset 阶段周期 |
| `phy_initialization_cycles` | Initialization 阶段周期 |
| `phy_training_cycles` | 自动训练周期 |
| `phy_auto_train` | 是否执行 PHY-independent 自动训练 |

## 6. 输出与验证

标准输出增加 `mem_phy_mode`、`phy_protocol`、`dfi_version`、FIFO/流水线配置，以及
`phy_*` 运行统计。Behavioral DFI 数据事件的 `cycle` 来自真实 PHY completion，
不再只按离线固定 CL/CWL 猜测。

`tests/phy_tests.cpp` 覆盖：

- HBM 与 LPDDR 适配器编码；
- reset/init/training 的禁止与允许边界；
- depth=1 FIFO 反压与零深度负例；
- LPDDR DVFS → WCKTRAIN 状态恢复；
- HBM3/HBM4/LPDDR5/LPDDR6 的写入、读回、payload 校验；
- 在线 PHY completion 生成的 DFI trace 自洽验证。

原有 sequence、timing boundary、smoke、性能曲线、敏感性和项目模型验证继续使用
Direct 默认路径回归，以保证历史功能；四个运行配置另行验证 Behavioral 完整路径。
