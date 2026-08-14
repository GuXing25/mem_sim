# data.hpp / data.cpp 逐行代码详解

本文档按符号和职责对真实存储层的两个核心文件进行逐段解释：
- `include/hbm_sim/core/data.hpp`——数据结构与接口声明
- `src/core/data.cpp`——全部实现

章节中的行号只作相对导航；代码持续演进时以符号名和当前源码为准。

这两个文件实现的是**堆叠存储模型的数据面**：真实 payload 读写、行缓冲、物理坐标、ECC shadow、功耗事件、3D RC 热模型、文本/CSV/二进制导出。协议面（命令、timing、状态机）在 dram/ 和 controller/。

```
MemoryImage（本文件）
  ├── backend_（memory_backend.cpp）— 稀疏/文件后端，只管"存哪里"
  ├── row_buffers_ — 每 bank 一个打开行的列数据视图
  ├── thermal_tiles_ — 稀疏 3D RC 热网格
  └── DataBlock — 一行 cache line 的完整状态
```

---

# 第一部分：data.hpp（接口声明）

## 一、头文件与 include（第 1-17 行）

```cpp
#pragma once

#include <cstdint>
#include <memory>        // std::unique_ptr — backend_ 智能指针
#include <optional>      // std::optional — 可能不存在的返回值
#include <string>
#include <unordered_map> // row_buffers_ / thermal_tiles_
#include <utility>       // std::move
#include <vector>

#include "hbm_sim/core/addr_map.hpp"        // AddressMapper / DecodedAddress
#include "hbm_sim/core/common.hpp"          // Address / Cycle / ByteVector / Command
#include "hbm_sim/core/memory_backend.hpp"  // MemoryBackend / BackendLine / MemoryBackendKind
#include "hbm_sim/core/storage_key.hpp"     // StorageKey / StorageKeyHash / FloorplanKey / ThermalGridKey
#include "hbm_sim/dram/spec.hpp"            // DramSpec / StorageModelOptions 依赖的 Organization 等
```

关键 include：`memory_backend.hpp` —— MemoryImage 不再自己持有 `unordered_map<Address, MemoryLine>`，而是委托给 `std::unique_ptr<MemoryBackend>`。这是重构后最大的架构变化：**存储介质从"内嵌数据结构"变成"可替换后端"**。

## 二、DataCheckResult（第 19-24 行）

```cpp
struct DataCheckResult {
  bool matched = true;       // expected == actual ?
  bool initialized = true;   // 数据是否已初始化（未初始化不算 mismatch，单列标记）
  ByteVector actual;         // 实际读到的数据
  std::string message;       // 不匹配时的可读描述
};
```

一次读校验的结果。注意 `matched` 和 `initialized` 分离：读未初始化地址时 `initialized=false` 但 `matched` 可能为 true（预期值恰好是默认值）。CLI 统计 `data_uninitialized_reads` 与 `data_mismatches` 分开计数。

## 三、FloorplanKey / ThermalGridKey（第 26-60 行）

```cpp
struct FloorplanKey {
  int stack = 0;   // 物理堆叠编号
  int layer = 0;   // 物理层号
  int tile_x = 0;  // 片内 x 坐标
  int tile_y = 0;  // 片内 y 坐标
  bool operator==(const FloorplanKey& other) const { ... }
};
struct FloorplanKeyHash { std::size_t operator()(const FloorplanKey& key) const; };
```

4 维空间坐标，用于 `thermal_tiles_` 等按物理位置的索引。`operator==` 全部字段比较；Hash 在 data.cpp 中实现（用 FNV-1a）。

**为什么不用 std::tuple 或单个 uint64_t？** 显式 struct 的字段名让代码自文档化（`key.layer` vs `get<1>(tuple)`）。Hash 函数把所有字段混合进 64 位，避免 tuple 默认 hash 的逐字段拼接冲突。

`ThermalGridKey` 结构与 FloorplanKey 相同，只是第三/四维是 `x`/`y`（热网格坐标，比 tile 更细）。

## 四、PhysicalAddress（第 64-102 行）

```cpp
struct PhysicalAddress {
  // ---- 逻辑定位 ----
  Address logical_line_base = 0;   // 所属 cache line 的基地址
  std::size_t byte_offset = 0;     // 在 line 内的字节偏移
  // ---- 解码坐标 ----
  int channel / pseudo_channel / sid / rank / bank_group / bank / row / column;
  // ---- 堆叠层 ----
  int stack = 0;    // 堆叠编号（当前恒 0，单堆叠）
  int die = 0;      // die 编号（当前 = layer）
  int layer = 0;    // 物理层号（核心派生量）
  // ---- floorplan tile ----
  int tile_x / tile_y / tile_z / tile_id;
  int floorplan_cols = 1;  // 每层 tile 列数
  int floorplan_rows = 1;  // 每层 tile 行数
  // ---- 热网格 ----
  int thermal_x / thermal_y / thermal_z;      // 全局热坐标
  int thermal_grid_x / thermal_grid_y;        // tile 内热子网格坐标
  int thermal_cols = 1;  // 全局热网格列数 = floorplan_cols * grid_cols
  int thermal_rows = 1;
  // ---- 亚 bank 几何 ----
  int subarray / mat_x / mat_y / mat_id / cell_x / cell_y;
  int microbump_x / microbump_y;
  int subarrays_per_bank = 1;
  int mats_per_subarray_x = 1;
  int mats_per_subarray_y = 1;
};
```

### 4.1 字段分组的设计意图

38 个字段分五组，每组回答不同层次的问题：

| 组 | 字段 | 回答的问题 |
|----|------|-----------|
| 逻辑定位 | logical_line_base, byte_offset | 这个物理位置对应哪个逻辑地址？ |
| 解码坐标 | channel~column | DRAM 协议如何解释这个地址？ |
| 堆叠层 | stack, die, layer | 数据在 3D 堆叠的第几层？ |
| floorplan | tile_x/y/z, tile_id | 数据在 die 的哪个平面区域？ |
| 热/几何 | thermal_*, subarray/mat/cell/microbump | 热和物理细节落在哪里？ |

**派生 vs 存储**：`logical_line_base`/`byte_offset` 是输入；其余全部是 `physical_address()` 从 spec 和 options 推导的。所以磁盘上（BackendLine）不保存 PhysicalAddress——重新加载时按同样规则重建，避免 options 变化导致陈旧坐标。

### 4.2 为什么 tile_id 单独存在？

`tile_id = (tile_z * floorplan_rows + tile_y) * floorplan_cols + tile_x` 把 3D 坐标压平成一维序号，方便输出和排序（dump 时按 tile_id 就能得到空间连续顺序）。

## 五、PhysicalStorageStats（第 104-167 行）

统计结构，~64 个字段分四类：

```cpp
// 1. 存储规模（从 backend 读）
std::uint64_t lines_allocated;          // 已分配行数
std::uint64_t unique_written_lines;     // version>0 的行数
std::uint64_t bytes_allocated;          // lines * line_size
// 2. 拓扑扫描控制
std::uint64_t topology_lines_scanned;   // 实际扫描行数
std::uint64_t topology_scan_skipped;    // 1 = 因超限跳过了扫描
// 3. 触达统计（各维度去重集合大小）
stacks/dies/layers/channels/pseudo_channels/sids/ranks/bank_groups/
banks/rows/columns/subarrays/mats/cells/microbumps/floorplan_tiles/
thermal_tiles/thermal_grid_cells _touched
// 4. 行为计数（与私有成员一一对应）
read/write_line_accesses, row_buffer_*（11 项）
power_*（7 项能量 + events）, thermal_*（5 项）, ecc_*（6 项）
// 5. 热结果
thermal_peak_temp_c / avg_temp_c / hotspot_layer / hotspot_x / hotspot_y
```

`topology_scan_skipped` 是重构后新增的重要字段：full-stack 大规模实验时，遍历所有已分配行做去重统计可能非常昂贵（chunk_file 后端要逐行 I/O），所以用 `topology_stats_scan_limit`（默认 100000）限制。超限时跳过扫描，报告里 `topology_scan_skipped : 1` 表明这些 touched 统计不可用。

## 六、StorageModelOptions（第 169-244 行）

配置结构，~75 个字段分七组：

### 6.1 后端与扫描（169-174）

```cpp
MemoryBackendOptions memory_backend;     // 后端选择（kind/capacity/文件路径）
double sparse_density_warning_pct = 30.0; // sparse 覆盖率达 30% 提示换文件后端
std::uint64_t topology_stats_scan_limit = 100000; // 0 = 不限制
```

### 6.2 模型开关（175-181）

```cpp
bool floorplan_enabled = true;   // 物理平面映射
bool power_enabled = true;       // 功耗事件
bool thermal_enabled = true;     // 热事件
std::string power_source = "configured_pj"; // 或 "dramsim3_idd"
double power_scale = 1.0;        // 全局能量缩放
```

### 6.3 热模型参数（182-205）—— 18 个参数

```
thermal_ambient_c = 40.0          环境温度（°C）
thermal_cooling_per_cycle = 0.00025 每 cycle 冷却比例（牛顿冷却）
thermal_rise_c_per_pj = 0.00002   每 pJ 升温（°C）
thermal_grid_cols/rows_per_tile = 1  tile 内热子网格
thermal_coupling_enabled = true   耦合开关
thermal_lateral_coupling = 0.02   横向耦合系数
thermal_vertical_coupling = 0.012 纵向耦合系数
thermal_tsv_coupling_scale = 0.03 TSV 额外耦合
thermal_tsvs_per_grid = 4         每网格 TSV 数
thermal_chip_dim_x/y_m = 0.01     芯片尺寸（m）
thermal_tsv_radius_m = 5e-6       TSV 半径
thermal_k_silicon/copper/insulator/heatsink = 148/401/1.5/4.0  W/(m·K)
thermal_c_silicon/copper/insulator/heatsink  比热容 J/(m³·K)
thermal_layer_height_si/cu/insulator_m       层高（m）
```

**为什么有材料属性但算法里只用了一部分？** `k_silicon`/`k_copper` 用在 `vertical_coupling_alpha()` 计算 TSV 面积加权的有效导热系数；比热容和层高当前是"声明但未深度使用"——为将来升级到更真实的瞬态热模型预留。这是研究模型的典型状态：参数面完整，求解器简化。

### 6.4 物理几何（206-212）

```
subarrays_per_bank = 16, mats_per_subarray_x/y = 4, cells_per_mat_x/y = 512,
microbumps_x/y = 8
```

### 6.5 ECC（213-216）

```
ecc_shadow_enabled = true    SECDED shadow 开关
ecc_check_on_read = true     读时校验
ecc_correct_single_bit = true 允许单 bit 纠正
ecc_inject_period = 0        0 = 不注入；N = 每 N 次写注入 1 bit 错误
```

### 6.6 DRAMsim3 IDD 电流（217-227）—— 10 个参数

```
idd_vdd = 1.2, idd0_ma = 65, idd2n_ma = 40, idd3n_ma = 55,
idd4r_ma = 390, idd4w_ma = 500, idd5ab_ma = 250, idd5pb_ma = 5,
idd6x_ma = 31, idd_devices_per_rank = 1.0, idd_burst_cycles = 0.0
```

### 6.7 per-command 能量（228-243）—— 17 个 pJ 值

```
act = 520, act1 = 280, act2 = 280, pre = 190, preab = 1200, cas = 45,
read = 340, read_per_byte = 2.0, write = 390, write_per_byte = 2.4,
refpb = 950, refdb = 1500, refab = 4200, rfmpb = 1250, rfmab = 5200,
control = 120
```

全部是 `research_default`（在 vendor_parameters.csv 中标明），不是厂商数据。

## 七、DataBlock（第 246-264 行）

```cpp
struct DataBlock {
  ByteVector bytes;             // payload（line_size 字节）
  ByteVector byte_mask;         // 本次写入字节标记（0=未更新）
  ByteVector initialized_mask;  // 历史初始化标记（逐字节）
  bool initialized = false;     // 全部字节已初始化？
  std::uint64_t version = 0;    // 全局写版本号
  std::uint64_t last_writer_request_id = 0;
  Cycle last_write_cycle = 0;
  Cycle last_access_cycle = 0;
  std::uint32_t checksum = 0;   // FNV-1a 32bit
  std::uint64_t ecc_hamming = 0;    // SECDED 校验子
  bool ecc_overall = false;         // 总奇偶位
  int ecc_parity_bits = 0;          // 校验位数量
  bool ecc_valid = false;           // shadow 有效
  bool ecc_uncorrectable = false;   // 检测到不可纠正错误
  std::uint64_t ecc_error_injections = 0; // 注入计数
  PhysicalAddress physical;   // 物理坐标（内存中计算，不持久化）
  StorageKey storage_key;     // 8 维逻辑键
};
```

**byte_mask vs initialized_mask 的区别**（容易混淆的一对）：
- `byte_mask`：**最近一次** write 调用中哪些字节被更新。写完后立刻被清零，下一次 write 重新设置。用于"本次写入的字节范围"。
- `initialized_mask`：**历史上**哪些字节被写过（累计）。只增不减。用于"这个字节有有效数据吗"。

`initialized = all_masked(initialized_mask)` —— 只有全部字节都写过才为 true。

## 八、DataBlockMetadata / DataMismatchRecord（第 266-291 行）

```cpp
struct DataBlockMetadata {   // DataBlock 的只读快照（去掉 bytes/byte_mask）
  bool initialized; version; last_writer_request_id; last_write_cycle;
  checksum; ecc_hamming; ecc_overall; ecc_parity_bits; ecc_valid;
  ecc_uncorrectable; PhysicalAddress physical; StorageKey storage_key;
};

struct DataMismatchRecord {  // 一次校验失败的完整现场
  Cycle cycle; std::uint64_t request_id; Address address;
  bool initialized; bool forwarded;   // 是否来自 write-forwarding
  ByteVector expected; ByteVector actual;
  std::optional<DataBlockMetadata> block;  // 有 block 才有元数据细节
  PhysicalAddress physical;
};
```

`DataBlockMetadata` 是为 `metadata()` 查询准备的轻量返回类型——调用方（如 controller 统计）不需要 64B payload，只要元数据。`DataMismatchRecord` 保存 mismatch 的完整上下文，dump 到 `mismatch_report.txt`。

## 九、DataValidator（第 293-311 行）

```cpp
class DataValidator {
 public:
  DataCheckResult check_read(cycle, request_id, address, physical,
                             expected, actual, initialized, forwarded, block);
  std::size_t mismatch_count() const;
  const std::vector<DataMismatchRecord>& mismatches() const;
  void dump_text(const std::string& path) const;
 private:
  std::vector<DataMismatchRecord> mismatches_;
};
```

只做一件事：比较 expected/actual，不匹配时记录现场。**无依赖**——不读 MemoryImage，由调用方（controller）把 block 元数据作为参数传入。

## 十、MemoryImage 类（第 313-461 行）

### 10.1 公共接口分组

```cpp
// 构造
explicit MemoryImage(std::size_t line_size = 64, std::uint8_t default_value = 0);  // 无 spec（测试用）
explicit MemoryImage(DramSpec spec, std::uint8_t default_value = 0,
                     StorageModelOptions options = {});   // 完整构造
~MemoryImage();   // 析构时 flush backend（try/catch 吞异常）

// 查询
line_size() / allocated_lines() / allocated_bytes() / all_addresses()
options() / backend_kind() / flush_backend()

// 坐标
physical_address(address, decoded)     // 核心：地址 → 38 字段坐标
storage_key(address, decoded)          // 地址 → 8 维键

// 统计与反查
storage_stats()                        // 完整统计结构
metadata(address, decoded)             // 元数据快照
address_for_storage_key(key)           // 反查
bank_storage_blocks()

// 行缓冲操作
activate_row / precharge_bank / precharge_all / flush_all_row_buffers

// 功耗/热
record_command_event(command, decoded, cycle, payload_bytes)

// 数据读写
read(address, size, initialized, decoded)
read_initialized_mask(address, size, decoded)   // 只读初始化掩码
write(address, data, mask, decoded, request_id, cycle)

// 导入导出
load_text / dump_text / dump_csv
load_binary / dump_binary
load_file（自动探测格式）/ dump_file（按扩展名）
dump_thermal_text
```

### 10.2 私有结构（376-392）

```cpp
struct RowBufferEntry {
  bool open = false;          // 该 bank 是否有打开的行
  bool dirty = false;         // 是否有未写回的列
  StorageKey bank_key;        // 所属 bank（row/col = -1）
  StorageKey row_key;         // 打开的行（col = -1）
  Cycle opened_cycle = 0;     // 打开时刻
  Cycle last_access_cycle = 0;
  std::unordered_map<int, DataBlock> columns;  // column → 数据块
};

struct ThermalTileState {
  PhysicalAddress physical;   // 首个触达该 tile 的坐标
  Cycle last_cycle = 0;       // 上次更新时刻（冷却计算）
  double temperature_c = 40.0; // 当前温度
  double energy_pj = 0.0;     // 累计能量
  std::uint64_t events = 0;   // 事件计数（0 = 未激活）
};
```

**RowBufferEntry 的 columns 是 `map<int, DataBlock>` 而非 vector**：一个 bank 的行可能有 32 列，但实际访问的只有少数几列——稀疏 map 只分配访问过的列。`events == 0` 是 ThermalTileState 的"未激活"哨兵（`thermal_tiles_[key]` 默认构造后 events=0，首次事件时才填 physical）。

### 10.3 私有方法与成员（394-461）

```cpp
// 地址拆分
line_base(address) / line_offset(address)
// 数据构造
make_line(base, decoded)                      // 新建空行
refresh_line_metadata(block)                  // 重算 checksum/init/storage_key
load_backend_line(base, block, decoded)       // 后端 → DataBlock
store_backend_line(base, block)               // DataBlock → 后端
// 键
floorplan_key(physical) / thermal_grid_key(physical)
bank_key_from_decoded / row_key_from_decoded
// 行缓冲
row_buffer_block(base, decoded, cycle, create)  // 非 const 版本
row_buffer_block(base, decoded)                 // const 版本
writeback_row_buffer(bank_key, cycle)
// ECC
refresh_ecc_shadow / maybe_inject_ecc_error / check_ecc_shadow
// 热
relax_thermal_tile / vertical_coupling_alpha / couple_thermal_neighbor / apply_thermal_event
// 成员
line_size_ = 64;  default_value_ = 0;
std::optional<DramSpec> spec_;        // 无 spec 时用退化坐标
StorageModelOptions options_;
std::unique_ptr<MemoryBackend> backend_;  // ★ 核心：存储委托后端
std::uint64_t next_version_ = 1;      // 全局写版本计数器
// 计数器（与 PhysicalStorageStats 对应，~30 个 uint64_t/double）
std::unordered_map<StorageKey, RowBufferEntry, StorageKeyHash> row_buffers_;
std::unordered_map<ThermalGridKey, ThermalTileState, ThermalGridKeyHash> thermal_tiles_;
```

**注意：没有 lines_ 成员了！** 重构前 MemoryImage 自带 `unordered_map<Address, MemoryLine> lines_`，现在完全由 `backend_` 接管。所有统计计数器保留在 MemoryImage 内（它们只在内存中变化，不需要持久化）。

### 10.4 全局辅助函数（463-467）

```cpp
ByteVector parse_hex_bytes(text);       // "AABBcc" → 字节数组
std::string bytes_to_hex(bytes);        // 字节数组 → "aabbcc"
std::string format_address(addr);       // 0x 前缀 16 位宽
ByteVector make_request_payload(addr, req_id, size);  // 确定性伪随机 payload
ByteVector normalize_mask(mask, size);  // 掩码归一化（0 → 0xff）
```

---

# 第二部分：data.cpp（实现）

## 十一、匿名命名空间工具函数（第 17-394 行）

### 11.1 十六进制工具（22-38）

```cpp
int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

std::string compact_hex(std::string text) {
  if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
    text.erase(0, 2);   // 去 0x 前缀
  }
  // 去掉 _ : 和所有空白，允许 "AA_BB:CC" 这种可读写法
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
               return c == '_' || c == ':' || std::isspace(c);
             }), text.end());
  return text;
}
```

`compact_hex` 宽容地接受多种写法：`0xAABBCC`、`aa_bb_cc`、`aa:bb:cc`、`aabb cc` 都会被规范化为 `aabbcc`。这对 trace 文件和 memory image 文件的手工编辑很友好。

### 11.2 key_of — FNV-1a 哈希（40-47）

```cpp
std::uint64_t key_of(std::initializer_list<int> values) {
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a 偏移基数
  for (int value : values) {
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
    h *= 1099511628211ull;                   // FNV-1a 素数
  }
  return h;
}
```

FNV-1a 哈希：先异或再乘素数，每轮混合一个 int。`static_cast<uint32_t>` 把有符号 int 转成无符号再混入——否则负数（如 row=-1 的哨兵）会产生符号扩展的混乱位模式。

**为什么自研哈希而不是 std::hash？** `std::hash` 对自定义聚合类型的专门化需要在多个编译单元保持一致；`key_of(initializer_list)` 一次实现，所有 KeyHash 复用，且 64 位输出比 `std::hash` 的 32/64 位平台差异稳定。

### 11.3 checksum_bytes（49-56）

```cpp
std::uint32_t checksum_bytes(const ByteVector& bytes) {
  std::uint32_t h = 2166136261u;   // FNV-1a 32 位偏移基数
  for (std::uint8_t byte : bytes) {
    h ^= byte;
    h *= 16777619u;
  }
  return h;
}
```

同样是 FNV-1a，32 位变体。用于 payload 完整性——不是纠错码，是快速检测"数据是否被意外修改"。

### 11.4 all_masked（58-62）

```cpp
bool all_masked(const ByteVector& mask) {
  return std::all_of(mask.begin(), mask.end(), [](std::uint8_t byte) {
    return byte != 0;
  });
}
```

"所有字节都非零" = 全部已初始化。注意语义：mask 中 0xff 表示"已写"，0 表示"未写"。

### 11.5 ECC 基础算法（64-149）

```cpp
bool is_power_of_two_u64(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}
```

经典 2 的幂判断：`n & (n-1) == 0`。

```cpp
bool byte_bit(const ByteVector& bytes, std::size_t bit) {
  std::size_t byte = bit / 8;
  std::size_t shift = bit % 8;
  return byte < bytes.size() && ((bytes[byte] >> shift) & 0x1u) != 0;
}

void flip_byte_bit(ByteVector& bytes, std::size_t bit) {
  std::size_t byte = bit / 8;
  std::size_t shift = bit % 8;
  if (byte < bytes.size()) {
    bytes[byte] ^= static_cast<std::uint8_t>(1u << shift);
  }
}
```

bit 级读写与翻转。`flip_byte_bit` 用异或实现——翻转就是 `^ (1<<shift)`，无论原值 0/1。

```cpp
int parity_bits_for_data_bits(std::size_t data_bits) {
  int parity_bits = 1;
  while ((std::uint64_t{1} << parity_bits) <
         data_bits + static_cast<std::size_t>(parity_bits) + 1) {
    parity_bits++;
  }
  return parity_bits;
}
```

**SECDED 校验位数计算**：汉明码需要 `2^p >= data_bits + p + 1`。对 512 bit（64B）payload：p=10（2^10=1024 >= 512+10+1=523）。p=9 时 2^9=512 < 523 不满足。这个公式从汉明码的定义直接推导。

```cpp
std::uint64_t code_position_for_data_bit(std::size_t data_bit) {
  // 汉明码：2 的幂位置是校验位，其余位置是数据位
  std::size_t data_seen = 0;
  std::uint64_t code_pos = 1;
  while (true) {
    if (!is_power_of_two_u64(code_pos)) {   // 非 2 幂 = 数据位槽
      if (data_seen == data_bit) return code_pos;
      data_seen++;
    }
    code_pos++;
  }
}
```

**数据位 → 编码位置映射**：汉明码把数据放在非 2 的幂位置（1,2,4,8,... 是校验位）。第 data_bit 个数据位放在第 data_seen 个非 2 幂位置。例如 data_bit=0 → code_pos=3，data_bit=1 → code_pos=5，data_bit=2 → code_pos=6...

```cpp
std::optional<std::size_t> data_bit_for_code_position(std::uint64_t code_position,
                                                      std::size_t data_bits) {
  // 逆映射：编码位置 → 数据位下标
  std::size_t data_seen = 0;
  for (std::uint64_t code_pos = 1; data_seen < data_bits; code_pos++) {
    if (is_power_of_two_u64(code_pos)) continue;   // 跳过校验位
    if (code_pos == code_position) return data_seen;
    data_seen++;
  }
  return std::nullopt;
}
```

纠错时 syndrome 指向一个编码位置，需要逆映射回数据位下标。返回 `std::nullopt` 表示 syndrome 指向校验位（那是奇偶修复场景）。

```cpp
struct SecdedShadow {
  std::uint64_t hamming = 0;  // p 位校验子
  bool overall = false;       // 总奇偶位（SEC 变 DED 的关键）
  int parity_bits = 0;
};

SecdedShadow compute_secded_shadow(const ByteVector& bytes) {
  SecdedShadow shadow;
  const std::size_t data_bits = bytes.size() * 8;
  shadow.parity_bits = parity_bits_for_data_bits(data_bits);
  bool overall = false;
  for (std::size_t bit = 0; bit < data_bits; bit++) {
    if (!byte_bit(bytes, bit)) continue;
    overall = !overall;    // 总奇偶 = 所有数据位的异或
    std::uint64_t code_pos = code_position_for_data_bit(bit);
    for (int parity = 0; parity < shadow.parity_bits; parity++) {
      if ((code_pos & (std::uint64_t{1} << parity)) != 0) {
        shadow.hamming ^= (std::uint64_t{1} << parity);
      }
    }
  }
  for (int parity = 0; parity < shadow.parity_bits; parity++) {
    if ((shadow.hamming & (std::uint64_t{1} << parity)) != 0) {
      overall = !overall;   // 总奇偶也包括校验位自身
    }
  }
  shadow.overall = overall;
  return shadow;
}
```

**SECDED 编码算法**（汉明码 + 总奇偶位）：
1. 对每个为 1 的数据位，取它的编码位置 code_pos
2. code_pos 的二进制表示中，每个为 1 的 bit 对应一个校验位，把该校验位置异或翻转
3. 收集所有数据位的贡献 → `shadow.hamming` 就是 p 位校验子
4. `overall` = 所有数据位奇偶 ⊕ 所有校验位奇偶 = 全部编码位的总奇偶

`overall` 的存在让 SEC 变成 SECDED：单 bit 错误时 syndrome 非零且 overall 翻转（可纠正）；双 bit 错误时 syndrome 非零但 overall 不翻转（检出不可纠正）。**这是"单纠错 + 双检错"的理论依据**。

### 11.6 ceil_sqrt_int（151-158）

```cpp
int ceil_sqrt_int(int value) {
  value = std::max(1, value);
  int root = 1;
  while (root * root < value) root++;
  return root;
}
```

**向上取整平方根**：floorplan 把每层的 bank 排列成近似正方形。例如每层 8 个 bank → ceil_sqrt(8)=3，排列 3×3 网格（9 个槽位，1 个空）。这是"最接近方形的矩形排列"的简单实现。

### 11.7 字符串工具（160-198）

```cpp
std::string lower_token(std::string token) {  // '-' → '_' + 小写
  std::transform(... [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return token;
}

std::string trim(std::string value) {  // 去除首尾空白
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::vector<std::string> split_tokens(const std::string& line) {
  std::istringstream iss(line);   // 按空白分割
  std::vector<std::string> tokens;
  std::string token;
  while (iss >> token) tokens.push_back(token);
  return tokens;
}

Address parse_address_token(const std::string& token) {
  std::size_t pos = 0;
  Address value = std::stoull(token, &pos, 0);  // base 0：自动识别 0x 前缀
  if (pos != token.size()) {
    throw std::invalid_argument("invalid memory image address: " + token);
  }
  return value;
}
```

`std::stoull(token, &pos, 0)` 的 base=0 自动识别十进制/十六进制（`0x` 前缀）——memory image 文件两种写法都接受。`pos != token.size()` 检查整串都被消费（`0x12abc` 有尾随垃圾时报错）。

### 11.8 类型转换（200-255）

```cpp
StorageKey key_from_physical(const PhysicalAddress& physical) {
  return StorageKey{physical.channel, physical.pseudo_channel, physical.sid,
                    physical.rank, physical.bank_group, physical.bank,
                    physical.row, physical.column};
}

DecodedAddress decoded_from_storage_key(const StorageKey& key) {
  DecodedAddress decoded;
  decoded.channel = key.channel; ...   // 反向填充
  return decoded;
}

BackendLine backend_line_from_block(const DataBlock& block) {
  BackendLine line;
  line.bytes = block.bytes;              // 拷贝 payload（进入后端）
  line.initialized_mask = block.initialized_mask;
  line.initialized = block.initialized;
  line.version = block.version; ...      // 全部元数据
  line.storage_key = block.storage_key;
  return line;
}

StorageKey bank_key_from_storage_key(StorageKey key) {
  key.row = -1;   // 哨兵：整行聚合键
  key.column = -1;
  return key;
}

StorageKey row_key_from_storage_key(StorageKey key) {
  key.column = -1;   // 哨兵：整列聚合键
  return key;
}
```

**-1 哨兵键的语义**：`bank_key`（row=col=-1）唯一标识一个 bank；`row_key`（col=-1）唯一标识某 bank 中打开的一行。行缓冲查找用这两个聚合键，避免为每列单独记录 bank/row 信息。

### 11.9 command_energy_pj（257-296）

```cpp
double command_energy_pj(const StorageModelOptions& options,
                         Command command, std::size_t payload_bytes) {
  switch (command) {
    case Command::ACT: return options.act_energy_pj;
    ...
    case Command::RD:
    case Command::RDA: return options.read_energy_pj +
        options.read_energy_per_byte_pj * static_cast<double>(payload_bytes);
    // 读写能量 = 固定部分 + per-byte 部分（数据量越大越耗电）
    ...
    case Command::NOP: return 0.0;
  }
  return 0.0;
}
```

命令 → 能量查表。**RD/WR 是唯一有 payload 相关性的**——`fixed + per_byte × payload_bytes`。其余命令（ACT/PRE/REF/RFM/control）固定值。

### 11.10 IDD 功耗校准（298-354）

```cpp
bool uses_dramsim3_idd_power(const StorageModelOptions& options) {
  std::string source = lower_token(options.power_source);
  return source == "idd" || source == "dramsim3_idd" || source == "dramsim3";
}

double nck_to_ns(int nck, double tck_ps) {
  return static_cast<double>(std::max(0, nck)) * tck_ps / 1000.0;  // ps→ns
}

double positive_current_delta(double active_ma, double background_ma) {
  return std::max(0.0, active_ma - background_ma);  // 防止负电流差
}

StorageModelOptions calibrate_power_from_idd(const DramSpec& spec,
                                             StorageModelOptions options) {
  if (!uses_dramsim3_idd_power(options)) return options;
  // DRAMsim3: E = VDD × ΔI(mA) × t(ns) × devices；1V×1mA×1ns = 1pJ
  const double tck_ps = spec.timing.tCK_ps > 0 ? spec.timing.tCK_ps : 1000.0;
  ...
  const double burst_ns = burst_cycles * tck_ps / 1000.0;
  const double tras_ns = nck_to_ns(spec.timing.nRAS, tck_ps);
  const double trp_ns  = nck_to_ns(spec.timing.nRP, tck_ps);
  const double trc_ns  = nck_to_ns(spec.timing.nRC, tck_ps);
  const double trfc_ns = nck_to_ns(spec.timing.nRFC, tck_ps);

  // ACT = VDD × (IDD0×tRC − (IDD3N×tRAS + IDD2N×tRP)) × devices
  options.act_energy_pj = std::max(0.0, options.idd_vdd *
      (options.idd0_ma * trc_ns -
       (options.idd3n_ma * tras_ns + options.idd2n_ma * trp_ns)) * devices);
  options.act1_energy_pj = options.act_energy_pj * 0.5;   // split activate 半价
  options.act2_energy_pj = options.act_energy_pj * 0.5;
  // RD = VDD × (IDD4R − IDD3N) × tBURST × devices
  options.read_energy_pj = options.idd_vdd *
      positive_current_delta(options.idd4r_ma, options.idd3n_ma) * burst_ns * devices;
  ...
  // REFab = VDD × (IDD5AB − IDD3N) × tRFC
  options.refab_energy_pj = options.idd_vdd *
      positive_current_delta(options.idd5ab_ma, options.idd3n_ma) * trfc_ns * devices;
  // HBM4 无公开 RFM 电流表 → 保守复用 REF 恢复能量
  options.rfmab_energy_pj = options.refab_energy_pj;
  options.rfmpb_energy_pj = options.refpb_energy_pj;
  return options;
}
```

**这是与 DRAMsim3 完全一致的公式**：
- ACT 能量 = 激活期间总电流(IDD0)减去 active-standby(IDD3N)和 precharge-standby(IDD2N)后的净电流 × tRC
- RD/WR 能量 = 读/写电流减去 standby 电流的增量 × 突发时间
- REF 能量 = 刷新电流增量 × tRFC

`1 V × 1 mA × 1 ns = 1 pJ`——单位自洽，无需换算系数。

### 11.11 metadata_from_block（356-371）

```cpp
DataBlockMetadata metadata_from_block(const DataBlock& block) {
  return DataBlockMetadata{block.initialized, block.version,
      block.last_writer_request_id, block.last_write_cycle, block.checksum,
      block.ecc_hamming, block.ecc_overall, block.ecc_parity_bits,
      block.ecc_valid, block.ecc_uncorrectable, block.physical,
      block.storage_key};
}
```

聚合初始化，按字段顺序一一对应。

### 11.12 load_text 的解析辅助（373-392）

```cpp
void set_decoded_key(DecodedAddress& decoded, const std::string& key,
                     const std::string& value) {
  int parsed = std::stoi(value);
  if (key == "ch" || key == "channel") decoded.channel = parsed;
  else if (key == "pc" || key == "pseudo_channel" || key == "pseudo") decoded.pseudo_channel = parsed;
  else if (key == "sid") decoded.sid = parsed;
  ...
}

bool is_decoded_key(const std::string& key) {
  return key == "ch" || ... || key == "column";
}
```

memory image 文件中 `ch=0 pc=1 sid=2` 等键 → DecodedAddress 字段的白名单映射。

## 十二、Hash 实现（第 396-413 行）

```cpp
std::size_t StorageKeyHash::operator()(const StorageKey& key) const {
  return static_cast<std::size_t>(key_of({key.channel, key.pseudo_channel,
      key.sid, key.rank, key.bank_group, key.bank, key.row, key.column}));
}
std::size_t FloorplanKeyHash::operator()(const FloorplanKey& key) const {
  return static_cast<std::size_t>(key_of({key.stack, key.layer, key.tile_x, key.tile_y}));
}
std::size_t ThermalGridKeyHash::operator()(const ThermalGridKey& key) const {
  return static_cast<std::size_t>(key_of({key.stack, key.layer, key.x, key.y}));
}
```

三个 Hash 全部委托 `key_of`，只是字段列表不同。**注意字段顺序**：StorageKey 按 StorageKey 结构体声明顺序（channel→column），确保 `==` 相等的键 hash 也相等（这是 unordered_map 的正确性要求）。

## 十三、DataValidator 实现（第 415-501 行）

### 13.1 check_read

```cpp
DataCheckResult DataValidator::check_read(Cycle cycle, std::uint64_t request_id,
    Address address, const PhysicalAddress& physical,
    const ByteVector& expected, const ByteVector& actual,
    bool initialized, bool forwarded, std::optional<DataBlockMetadata> block) {
  DataCheckResult result;
  result.initialized = initialized;
  result.actual = actual;
  result.matched = expected == actual;    // 向量比较
  if (result.matched) return result;      // 一致 → 提前返回

  // 找第一个不同字节的位置（用于诊断）
  std::size_t mismatch_offset = 0;
  while (mismatch_offset < expected.size() && mismatch_offset < actual.size() &&
         expected[mismatch_offset] == actual[mismatch_offset]) {
    mismatch_offset++;
  }
  std::ostringstream msg;
  msg << "data mismatch at " << format_address(address)
      << " request=" << request_id << " offset=" << mismatch_offset
      << " expected=" << bytes_to_hex(expected)
      << " actual=" << bytes_to_hex(actual);
  result.message = msg.str();

  mismatches_.push_back(DataMismatchRecord{cycle, request_id, address,
      initialized, forwarded, expected, actual, std::move(block), physical});
  return result;
}
```

**设计要点**：
- `expected == actual` 是向量整体比较（`operator==`），O(n)
- mismatch 记录用 `std::move(block)` 转移所有权，避免拷贝
- 记录里保存**完整 expected/actual 副本**——后续 dump 时不需要再访问数据源

### 13.2 dump_text

```cpp
void DataValidator::dump_text(const std::string& path) const {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("failed to open mismatch report output: " + path);
  out << "# cycle request_id address initialized forwarded ch pc sid rank bg bank row col layer tile_x tile_y "
      << "version last_writer last_write_cycle checksum ecc_valid ecc_hamming ecc_overall ecc_uncorrectable "
      << "expected actual\n";
  for (const auto& mismatch : mismatches_) {
    ...
    out << (mismatch.block ? mismatch.block->storage_key.rank : p.rank) << ' '
    ...
    if (mismatch.block) { ... } else { out << "NA NA NA NA NA NA NA NA"; }
    out << ' ' << bytes_to_hex(mismatch.expected)
        << ' ' << bytes_to_hex(mismatch.actual) << '\n';
  }
}
```

输出固定 27 列，列名在首行注释。**有 block 时输出元数据细节，无 block 时输出 8 个 NA**——保证列数恒定，方便脚本解析。

## 十四、构造函数（第 503-532 行）

### 14.1 无 spec 构造（测试用）

```cpp
MemoryImage::MemoryImage(std::size_t line_size, std::uint8_t default_value)
    : line_size_(std::max<std::size_t>(1, line_size)),
      default_value_(default_value),
      backend_(make_memory_backend(line_size_, options_.memory_backend)) {}
```

无 spec → 无法解码地址 → 使用退化坐标（行号来自地址位）。用于 sequence_tests 等不需要 DRAM 语义的场景。`std::max(1, line_size)` 防止 line_size=0 导致除零。

### 14.2 完整构造

```cpp
MemoryImage::MemoryImage(DramSpec spec, std::uint8_t default_value,
                         StorageModelOptions options)
    : line_size_(static_cast<std::size_t>(std::max(1, spec.transaction_bytes()))),
      default_value_(default_value),
      spec_(std::move(spec)),
      options_(calibrate_power_from_idd(*spec_, std::move(options))) {
  if (options_.memory_backend.capacity_bytes == 0) {
    // 未显式指定容量 → 使用 spec 的可寻址总容量
    options_.memory_backend.capacity_bytes = spec_->addressable_capacity_bytes();
  }
  backend_ = make_memory_backend(line_size_, options_.memory_backend);
}
```

**line_size 来自 `spec.transaction_bytes()`**（不是硬编码 64）：控制器一次事务的字节数（HBM4 验证配置是 32B）。这保证数据块粒度与协议粒度一致。

**容量自动推导**：用户不指定 `memory_capacity_bytes` 时，用 `addressable_capacity_bytes()`（ch × pc × sid × rank × bg × bank × rows × columns × line_size）。对 HBM4 全栈这是 2048GB 级的大数——这就是为什么 mmap/chunk 后端有价值：逻辑容量巨大但磁盘稀疏。

### 14.3 析构

```cpp
MemoryImage::~MemoryImage() {
  try { flush_backend(); } catch (...) {}
}
void MemoryImage::flush_backend() {
  if (backend_) backend_->flush();
}
```

析构时自动 flush（mmap 数据回写磁盘）。`try/catch(...)` 吞掉异常——析构函数抛异常会导致 `std::terminate`。

## 十五、地址基础（第 534-540 行）

```cpp
Address MemoryImage::line_base(Address address) const {
  return address - (address % static_cast<Address>(line_size_));
}
std::size_t MemoryImage::line_offset(Address address) const {
  return static_cast<std::size_t>(address % static_cast<Address>(line_size_));
}
```

**cache line 对齐**：`line_base` 向下取整到 line 边界，`line_offset` 取余。任何 address 都能拆成 (base, offset) 对。**注意用取模而非位运算**——line_size 不保证是 2 的幂（如 32 是，但 24 不是），取模通用。

## 十六、physical_address — 核心坐标推导（第 542-646 行）

这是整个文件最核心的函数：把 (address, decoded) 映射成 38 字段的 PhysicalAddress。

### 16.1 第一步：获取解码坐标（543-566）

```cpp
Address base = line_base(address);
PhysicalAddress physical;
physical.logical_line_base = base;
physical.byte_offset = line_offset(address);

DecodedAddress d;
if (decoded != nullptr) {
  d = *decoded;                              // 调用方已解码（快路径）
} else if (spec_.has_value()) {
  d = AddressMapper(*spec_).decode(base);    // 现场解码（慢路径）
} else {
  std::uint64_t line = base / line_size_;
  d.row = static_cast<int>(line & 0x7fffffffull);  // 退化：行号 = 地址位
  d.column = 0;
}
```

三级策略：调用方给解码（controller 热路径）→ 无解码但有 spec（现场解码）→ 无 spec（退化地址位）。`0x7fffffff` 限制行号在 31 bit 内。

### 16.2 第二步：层号推导（568-579）

```cpp
const int stack_height = spec_.has_value() ? std::max(1, spec_->stack_height) : 1;
const int sid_count = spec_.has_value() ? std::max(1, spec_->org.sids) : 1;
const int banks_per_group = spec_.has_value() ? std::max(1, spec_->org.banks_per_group) : 1;
const int bank_index = std::max(0, d.bank_group) * banks_per_group + std::max(0, d.bank);
const int layers_per_sid = std::max(1, (stack_height + sid_count - 1) / sid_count);
int layer = std::max(0, d.sid) * layers_per_sid + (bank_index % layers_per_sid);
if (layer >= stack_height) {
  layer %= stack_height;    // 防止越界（例如 16hi 但 sids=2）
}
physical.layer = layer;
physical.die = layer;       // 当前模型 die == layer
physical.stack = 0;         // 单堆叠模型
```

**层分配算法**：`bank_index` 是全局 bank 序号（bg×banks_per_group + bank）。层 = `sid × layers_per_sid + (bank_index % layers_per_sid)`。

举例（HBM4 8hi, sids=2）：layers_per_sid = 8/2 = 4。sid=0 的 bank 落在 layer 0-3，sid=1 的 bank 落在 layer 4-7。**同一个 SID 内的不同 bank 交错分布在 4 层上**——这模拟了真实 HBM 中 bank 在多个 die 上的分布。

`layer >= stack_height` 的取模保护是防御性代码：如果配置了 16hi 但 sids=4，layers_per_sid=4，sid=3×4+3=15 仍然 < 16，但如果 stack_height 与 sids 不匹配（如 8hi + sids=4 → layers_per_sid=2，sid=3×2+1=7 < 8 OK；但 bank_index 可能 ≥2 导致 6+1=7...）——取模兜底。

### 16.3 第三步：floorplan tile（580-603）

```cpp
if (options_.floorplan_enabled) {
  const int pseudo_channels = ...;
  const int ranks = ...;
  const int bank_groups = ...;
  const int banks_per_layer = std::max(1, bank_groups * banks_per_group);
  const int floorplan_cols = ceil_sqrt_int(banks_per_layer);   // 近似方形
  const int floorplan_rows_per_slice = (banks_per_layer + floorplan_cols - 1) / floorplan_cols;
  // slice = (rank × sid_count + sid) × pseudo_channels + pseudo_channel
  const int slice = ((std::max(0, d.rank) * sid_count + std::max(0, d.sid)) *
                     pseudo_channels) + std::max(0, d.pseudo_channel);
  physical.floorplan_cols = floorplan_cols;
  physical.floorplan_rows =
      std::max(1, floorplan_rows_per_slice * pseudo_channels * sid_count * ranks);
  physical.tile_x = bank_index % floorplan_cols;
  physical.tile_y = slice * floorplan_rows_per_slice + (bank_index / floorplan_cols);
  physical.tile_z = layer;
  physical.tile_id = (physical.tile_z * physical.floorplan_rows + physical.tile_y) *
                     physical.floorplan_cols + physical.tile_x;
}
```

**floorplan 布局算法**：
1. 每层 bank 数 `banks_per_layer = bg × banks_per_group`（HBM4：2×8=16）
2. `floorplan_cols = ceil_sqrt(16) = 4`，每 slice 行数 = ceil(16/4) = 4 → 4×4 方形
3. `slice` 是 (rank, sid, pc) 的三维扁平化——每个 slice 占 floorplan_rows_per_slice 行
4. tile_x = bank % cols（水平位置），tile_y = slice×rows_per_slice + bank/cols（垂直位置：先按 slice 分块再按 bank 排列）

效果：bank 在 die 上排成方形块，不同 sid/pc 的块纵向堆叠。`tile_id` 把 (z,y,x) 压平成一维。

### 16.4 第四步：热网格（605-620）

```cpp
const int grid_cols = std::max(1, options_.thermal_grid_cols_per_tile);
const int grid_rows = std::max(1, options_.thermal_grid_rows_per_tile);
const int row_count = spec_.has_value() ? std::max(1, spec_->org.rows) : ...;
const int column_count = spec_.has_value() ? std::max(1, spec_->org.columns) : ...;
auto scaled_index = [](int value, int span, int buckets) {
  int safe_value = std::max(0, value);
  int idx = static_cast<int>((static_cast<long long>(safe_value) * buckets) / std::max(1, span));
  return std::clamp(idx, 0, std::max(1, buckets) - 1);
};
physical.thermal_grid_x = scaled_index(d.row, row_count, grid_cols);
physical.thermal_grid_y = scaled_index(d.column, column_count, grid_rows);
physical.thermal_x = physical.tile_x * grid_cols + physical.thermal_grid_x;
physical.thermal_y = physical.tile_y * grid_rows + physical.thermal_grid_y;
physical.thermal_z = physical.tile_z;
physical.thermal_cols = std::max(1, physical.floorplan_cols * grid_cols);
physical.thermal_rows = std::max(1, physical.floorplan_rows * grid_rows);
```

**scaled_index 是比例缩放**：行号/列号在全范围的位置 × 桶数 → 映射到网格坐标。例如 16384 行映射到 4 个网格列，第 8192 行 → 8192×4/16384 = 2。**用 `long long` 做乘法防溢出**。

热网格 = floorplan tile 内再细分（`thermal_grid_cols_per_tile × thermal_grid_rows_per_tile`）。`thermal_x/y` 是全局热坐标 = tile 起点 + tile 内偏移。

### 16.5 第五步：亚 bank 几何（622-644）

```cpp
const int row_per_subarray = std::max(1, (row_count + subarrays - 1) / subarrays);
const int row_in_subarray = std::max(0, d.row) % row_per_subarray;
physical.subarray = scaled_index(d.row, row_count, subarrays);
physical.mat_x = scaled_index(row_in_subarray, row_per_subarray, mats_x);
physical.mat_y = scaled_index(col, column_count, mats_y);
physical.mat_id = physical.mat_y * mats_x + physical.mat_x;
physical.cell_x = scaled_index(row_in_subarray, row_per_subarray, cells_x);
physical.cell_y = scaled_index(col, column_count, cells_y);
physical.microbump_x = std::clamp(physical.tile_x % microbumps_x, 0, microbumps_x - 1);
physical.microbump_y = std::clamp((physical.tile_y + physical.layer) % microbumps_y, 0, microbumps_y - 1);
```

- `subarray`：行号按比例落入哪个子阵列（16384 行 / 16 子阵列 = 每子阵列 1024 行）
- `mat_x`/`mat_y`：子阵列内按行/列位置分块 → 矩阵
- `microbump`：tile 坐标对 microbump 网格取模——**不同的 layer 会偏移 microbump_y**（`(tile_y + layer) % microbumps_y`），模拟 TSV 阵列在不同层间的交错排列。

## 十七、storage_stats — 统计聚合（第 652-791 行）

### 17.1 直接拷贝段（654-694）

从私有计数器逐字段拷贝到 `PhysicalStorageStats`。这些计数器在 write/read/record_command_event 等路径中累加。

### 17.2 拓扑扫描段（696-745）

```cpp
const bool scan_topology =
    options_.topology_stats_scan_limit == 0 ||
    backend_->address_scan_lines() <= options_.topology_stats_scan_limit;
stats.topology_scan_skipped = scan_topology ? 0 : 1;
...
if (scan_topology) {
  for (Address base : backend_->all_addresses()) {
    DataBlock block;
    if (!load_backend_line(base, block)) continue;
    stats.topology_lines_scanned++;
    const PhysicalAddress& p = block.physical;
    stacks.insert(key_of({p.stack}));
    dies.insert(key_of({p.stack, p.die}));
    ...
    floorplan_tiles.insert(key_of({p.stack, p.layer, p.tile_x, p.tile_y}));
  }
}
```

**去重统计**：对每行，把各维度坐标用 `key_of` 哈希后插入 `unordered_set`。每个集合的大小就是"触达了多少个 X"。层级递进：`channels` 集合的键是 `(stack, channel)`——**不同 stack 的同号 channel 计为不同**，保证层级正确。

**scan limit 保护**：`address_scan_lines()` 对 sparse 返回已分配行数，对 mmap/chunk 返回逻辑总行数。逻辑总行数超 100000 时跳过扫描（`topology_scan_skipped=1`），避免 mmap/chunk 后端遍历全部逻辑空间的高昂代价。

### 17.3 热统计段（765-789）

```cpp
for (const auto& [key, tile] : thermal_tiles_) {
  temp_sum += tile.temperature_c;
  if (!saw_tile || tile.temperature_c > stats.thermal_peak_temp_c) {
    saw_tile = true;
    stats.thermal_peak_temp_c = tile.temperature_c;
    stats.thermal_hotspot_layer = tile.physical.layer;  // 热点定位
    stats.thermal_hotspot_x = tile.physical.thermal_x;
    stats.thermal_hotspot_y = tile.physical.thermal_y;
  }
}
if (!thermal_tiles_.empty()) {
  stats.thermal_avg_temp_c = temp_sum / thermal_tiles_.size();
}
for (const auto& [key, row_buffer] : row_buffers_) {
  if (row_buffer.open) {
    stats.row_buffer_open_rows++;
    if (row_buffer.dirty) stats.row_buffer_dirty_rows++;
  }
}
```

峰值温度遍历所有热 tile，同时记录**热点位置**（layer/x/y）。开放行缓冲计数也在这一遍收集。

## 十八、查询方法（第 793-812 行）

```cpp
std::optional<DataBlockMetadata> MemoryImage::metadata(Address address,
                                                       const DecodedAddress* decoded) const {
  Address base = line_base(address);
  if (const DataBlock* row_block = row_buffer_block(base, decoded)) {
    return metadata_from_block(*row_block);   // 行缓冲优先
  }
  DataBlock block;
  if (!load_backend_line(base, block, decoded)) {
    return std::nullopt;                      // 不存在
  }
  DataBlockMetadata out = metadata_from_block(block);
  if (decoded != nullptr) {
    out.physical = physical_address(base, decoded);   // 用调用方解码刷新坐标
    out.storage_key = key_from_physical(out.physical);
  }
  return out;
}

std::optional<Address> MemoryImage::address_for_storage_key(const StorageKey& key) const {
  return backend_->address_for_storage_key(key);   // 委托后端
}
```

`metadata()` 先查行缓冲（数据可能在 buffer 中尚未写回），再查后端。`address_for_storage_key` 完全委托后端（sparse 的 reverse_ 索引 O(1)，文件后端遍历位图）。

## 十九、make_line / refresh_line_metadata / 后端桥接（第 814-861 行）

```cpp
DataBlock MemoryImage::make_line(Address base, const DecodedAddress* decoded) const {
  DataBlock block;
  block.bytes = ByteVector(line_size_, default_value_);       // 全默认值
  block.byte_mask = ByteVector(line_size_, 0);                // 无写入
  block.initialized_mask = ByteVector(line_size_, 0);         // 未初始化
  block.physical = physical_address(base, decoded);
  block.storage_key = key_from_physical(block.physical);
  block.checksum = checksum_bytes(block.bytes);               // 初始校验和
  return block;
}

void MemoryImage::refresh_line_metadata(DataBlock& block) {
  block.initialized = all_masked(block.initialized_mask);
  block.checksum = checksum_bytes(block.bytes);
  block.storage_key = key_from_physical(block.physical);
}
```

**新建空行**：数据全默认值、初始化掩码全 0（未初始化）、checksum 立即计算。**元数据刷新**：数据变化后重算三项派生值——initialized、checksum、storage_key。

```cpp
bool MemoryImage::load_backend_line(Address base, DataBlock& block,
                                    const DecodedAddress* decoded) const {
  BackendLine line;
  if (!backend_->load(base, line)) return false;
  block.bytes = std::move(line.bytes);        // 移动而非拷贝
  block.byte_mask.assign(line_size_, 0);      // 后端不存 byte_mask → 清零
  block.initialized_mask = std::move(line.initialized_mask);
  ... // 元数据逐字段
  block.storage_key = decoded == nullptr ? line.storage_key
                                         : storage_key(base, decoded);
  DecodedAddress stored_decoded = decoded_from_storage_key(block.storage_key);
  block.physical = physical_address(base, decoded == nullptr ? &stored_decoded : decoded);
  return true;
}

bool MemoryImage::store_backend_line(Address base, const DataBlock& block) {
  return backend_->store(base, backend_line_from_block(block));
}
```

**后端 ↔ DataBlock 桥接**：`load_backend_line` 是唯一从 BackendLine 构造 DataBlock 的地方；`store_backend_line` 是唯一反向的地方。**byte_mask 不持久化**（assign 0），因为它只描述最近一次写。

**坐标重建策略**：有 decoded 参数时用它（调用方知道当前地址映射）；无 decoded 时用后端存的 storage_key 重建 DecodedAddress 再推导坐标。这保证了"文件重开 + 配置变化"时坐标总是按当前 spec 计算。

## 二十、ECC 三函数（第 871-937 行）

### 20.1 refresh_ecc_shadow

```cpp
void MemoryImage::refresh_ecc_shadow(DataBlock& block) {
  if (!options_.ecc_shadow_enabled) {
    block.ecc_valid = false;
    block.ecc_uncorrectable = false;
    return;
  }
  SecdedShadow shadow = compute_secded_shadow(block.bytes);
  block.ecc_hamming = shadow.hamming;
  block.ecc_overall = shadow.overall;
  block.ecc_parity_bits = shadow.parity_bits;
  block.ecc_valid = true;
  block.ecc_uncorrectable = false;
  ecc_shadow_updates_++;
}
```

每次数据写入后调用：重算校验子，标记 valid。**ecc_valid 是"shadow 与当前数据同步"的标记**。

### 20.2 maybe_inject_ecc_error

```cpp
void MemoryImage::maybe_inject_ecc_error(DataBlock& block) {
  if (!options_.ecc_shadow_enabled || options_.ecc_inject_period <= 0) return;
  if (block.version == 0 ||
      (block.version % static_cast<std::uint64_t>(options_.ecc_inject_period)) != 0) {
    return;
  }
  flip_byte_bit(block.bytes, 0);           // 翻转 bit 0
  block.checksum = checksum_bytes(block.bytes);
  block.ecc_error_injections++;
  ecc_injected_errors_++;
}
```

**故障注入**：每 N 次写入（按 version 取模）翻转第一个 bit——模拟存储单元位翻转。**注意**：翻转后 checksum 重算（checksum 反映实际数据），但 **ecc shadow 不重算**——故意制造"数据与 shadow 不一致"的状态，供下次读时检测。这是测试 ECC 检测/纠正路径的标准方法。

### 20.3 check_ecc_shadow — SECDED 校验

```cpp
void MemoryImage::check_ecc_shadow(DataBlock& block) {
  if (!options_.ecc_shadow_enabled || !options_.ecc_check_on_read || !block.ecc_valid) return;
  ecc_checked_reads_++;
  SecdedShadow now = compute_secded_shadow(block.bytes);   // 当前数据重算
  std::uint64_t syndrome = block.ecc_hamming ^ now.hamming; // 存储 vs 当前
  bool overall_mismatch = block.ecc_overall != now.overall;

  if (syndrome == 0 && !overall_mismatch) return;          // 无错误

  if (syndrome == 0 && overall_mismatch) {                 // 奇偶位损坏
    block.ecc_overall = now.overall;                       // 修复 shadow
    ecc_parity_repairs_++;
    return;
  }
  if (!overall_mismatch) {                                 // 偶数个 bit 错
    block.ecc_uncorrectable = true;                        // 双 bit 不可纠正
    ecc_uncorrectable_errors_++;
    return;
  }
  if (is_power_of_two_u64(syndrome)) {                     // syndrome 是 2 幂
    block.ecc_hamming = now.hamming;                       // 校验位错误，仅修 shadow
    block.ecc_overall = now.overall;
    ecc_parity_repairs_++;
    return;
  }
  std::optional<std::size_t> data_bit =
      data_bit_for_code_position(syndrome, block.bytes.size() * 8);
  if (!data_bit.has_value() || !options_.ecc_correct_single_bit) {
    block.ecc_uncorrectable = true;                        // 不可纠正
    ecc_uncorrectable_errors_++;
    return;
  }
  flip_byte_bit(block.bytes, *data_bit);                   // 纠正数据位
  block.checksum = checksum_bytes(block.bytes);
  refresh_ecc_shadow(block);                               // 重算 shadow
  ecc_corrected_errors_++;
}
```

**SECDED 判定逻辑表**（syndrome = 存储校验子 ⊕ 当前校验子，overall_mismatch = 总奇偶不一致）：

| syndrome | overall 翻转 | 含义 | 处理 |
|----------|-------------|------|------|
| 0 | 否 | 无错误 | 返回 |
| 0 | 是 | 奇偶位自身错误 | 修复 shadow |
| ≠0 | 否 | **偶数个位错（≥2，双 bit 错）** | 标记不可纠正 |
| 2 的幂 | 是 | 校验位错误 | 修复 shadow |
| 其他 | 是 | **单 bit 数据错** | 纠正数据位 |

理论依据（汉明码）：单 bit 错 → syndrome 指向出错位（非 2 幂位置 = 数据位，2 幂位置 = 校验位）；双 bit 错 → syndrome 非零但总奇偶不变（两个翻转互相抵消奇偶）。**这是 SECDED 能"单纠双检"的根本**。

## 二十一、热模型（第 939-1048 行）

### 21.1 relax_thermal_tile — 牛顿冷却

```cpp
void MemoryImage::relax_thermal_tile(ThermalTileState& tile, Cycle cycle) {
  Cycle delta = cycle > tile.last_cycle ? cycle - tile.last_cycle : 0;
  if (delta > 0 && tile.temperature_c > options_.thermal_ambient_c) {
    double cooling = std::min(1.0, static_cast<double>(delta) * options_.thermal_cooling_per_cycle);
    tile.temperature_c -= (tile.temperature_c - options_.thermal_ambient_c) * cooling;
  }
  tile.last_cycle = cycle;
}
```

**离散化牛顿冷却**：温度高于环境时，每 cycle 按 `cooling_per_cycle` 比例向环境温度衰减。`cooling = delta × rate` 允许一次跨多个 cycle 松弛（delta 是距上次更新的周期数）。`min(1.0, ...)` 防止长时间跨度导致过冲。

### 21.2 vertical_coupling_alpha — TSV 面积加权导热

```cpp
double MemoryImage::vertical_coupling_alpha(const PhysicalAddress& physical) const {
  double base = std::max(0.0, options_.thermal_vertical_coupling);
  const double cell_area = std::max(1e-18,
      (options_.thermal_chip_dim_x_m / std::max(1, physical.thermal_cols)) *
      (options_.thermal_chip_dim_y_m / std::max(1, physical.thermal_rows)));
  const double tsv_area = kPi * options_.thermal_tsv_radius_m * options_.thermal_tsv_radius_m *
                          static_cast<double>(std::max(0, options_.thermal_tsvs_per_grid));
  const double tsv_fraction = std::clamp(tsv_area / cell_area, 0.0, 0.90);
  const double silicon_k = std::max(1e-9, options_.thermal_k_silicon);
  const double effective_k = (1.0 - tsv_fraction) * options_.thermal_k_silicon +
                             tsv_fraction * options_.thermal_k_copper;
  const double tsv_boost = options_.thermal_tsv_coupling_scale *
                           std::max(0.0, (effective_k / silicon_k) - 1.0);
  return std::clamp(base + tsv_boost, 0.0, 0.25);
}
```

**物理意义**：TSV（硅通孔）是铜柱，铜的导热系数（401 W/m·K）远高于硅（148）。TSV 占比越高，层的垂直导热越好。

计算：
1. 网格单元面积 = 芯片尺寸 / 网格数
2. TSV 总面积 = πr² × 数量
3. TSV 面积占比（clamp 到 0.90 防物理上不可能的 100%）
4. 有效导热 = 硅占比×k_si + TSV 占比×k_cu（混合法则）
5. 耦合增强 = scale × (有效k / 硅k − 1)——TSV 带来的额外耦合
6. 最终系数 clamp 到 [0, 0.25]（防止数值不稳定）

### 21.3 couple_thermal_neighbor — 6 向耦合

```cpp
void MemoryImage::couple_thermal_neighbor(const ThermalGridKey& source_key, int dx, int dy,
                                          int dz, double alpha, Cycle cycle, bool tsv_path) {
  if (alpha <= 0.0) return;
  auto src_it = thermal_tiles_.find(source_key);
  if (src_it == thermal_tiles_.end()) return;
  const PhysicalAddress src_physical = src_it->second.physical;
  ThermalGridKey neighbor_key{source_key.stack, source_key.layer + dz,
                              source_key.x + dx, source_key.y + dy};
  // 边界检查：层号非负、坐标在网格范围内
  if (neighbor_key.layer < 0 ||
      neighbor_key.x < 0 || neighbor_key.x >= std::max(1, src_physical.thermal_cols) ||
      neighbor_key.y < 0 || neighbor_key.y >= std::max(1, src_physical.thermal_rows)) {
    return;
  }
  if (spec_.has_value() && neighbor_key.layer >= std::max(1, spec_->stack_height)) {
    return;   // 超出堆叠层数
  }

  auto& src = src_it->second;
  auto& dst = thermal_tiles_[neighbor_key];      // 不存在则创建（稀疏）
  if (dst.events == 0) {                          // 首次激活邻居
    dst.physical = src_physical;
    dst.physical.layer = neighbor_key.layer;     // 修正为邻居坐标
    dst.physical.thermal_z = neighbor_key.layer;
    dst.physical.thermal_x = neighbor_key.x;
    dst.physical.thermal_y = neighbor_key.y;
    dst.temperature_c = options_.thermal_ambient_c;
    dst.last_cycle = cycle;
  }
  relax_thermal_tile(src, cycle);
  relax_thermal_tile(dst, cycle);

  double delta = (src.temperature_c - dst.temperature_c) * std::clamp(alpha, 0.0, 0.25);
  if (std::abs(delta) < 1e-12) return;           // 温度差极小 → 跳过
  src.temperature_c -= delta;                     // 热从 src 流向 dst
  dst.temperature_c += delta;
  thermal_coupled_delta_c_ += std::abs(delta);
  if (dz == 0) thermal_lateral_transfers_++;     // 横向
  else { thermal_vertical_transfers_++; if (tsv_path) thermal_tsv_transfers_++; }
}
```

**热传导的简化模型**：相邻 tile 间的热流 ∝ 温度差 × 耦合系数。`delta = ΔT × alpha`，源降温目标升温，总能量守恒（一个减一个加）。**稀疏激活**：邻居首次被触及才分配 ThermalTileState，且继承源的物理坐标并修正为邻居位置。

### 21.4 apply_thermal_event

```cpp
void MemoryImage::apply_thermal_event(const PhysicalAddress& physical,
                                      Cycle cycle, double energy_pj) {
  if (!options_.thermal_enabled) return;
  ThermalGridKey key = thermal_grid_key(physical);
  auto& tile = thermal_tiles_[key];               // 首次分配（events==0）
  if (tile.events == 0) {
    tile.physical = physical;
    tile.last_cycle = cycle;
    tile.temperature_c = options_.thermal_ambient_c;
  }
  relax_thermal_tile(tile, cycle);
  tile.temperature_c += energy_pj * options_.thermal_rise_c_per_pj;  // 升温
  tile.energy_pj += energy_pj;
  tile.events++;
  tile.last_cycle = cycle;
  thermal_updates_++;
  if (options_.thermal_coupling_enabled) {
    const double lateral = std::clamp(options_.thermal_lateral_coupling, 0.0, 0.25);
    const double vertical = vertical_coupling_alpha(physical);
    // 4 个横向邻居 + 2 个纵向邻居（TSV 路径）
    couple_thermal_neighbor(key, -1, 0, 0, lateral, cycle, false);
    couple_thermal_neighbor(key,  1, 0, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 0, -1, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 0,  1, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 0, 0, -1, vertical, cycle, true);
    couple_thermal_neighbor(key, 0, 0,  1, vertical, cycle, true);
  }
}
```

**能量→温度**：`ΔT = energy_pj × rise_c_per_pj`。每个功耗事件：先冷却（relax）再升温（加能量），然后向 6 个邻居耦合。纵向邻居用 `tsv_path=true` 标记——它们的耦合系数来自 TSV 面积加权。

## 二十二、record_command_event — 功耗入口（第 1050-1082 行）

```cpp
void MemoryImage::record_command_event(Command command, const DecodedAddress& decoded,
                                       Cycle cycle, std::size_t payload_bytes) {
  if (!options_.power_enabled) return;
  double energy = command_energy_pj(options_, command, payload_bytes) * options_.power_scale;
  if (energy <= 0.0) return;

  PhysicalAddress physical = physical_address(0, &decoded);   // 命令级坐标
  power_events_++;
  power_energy_pj_ += energy;
  if (command == Command::ACT || command == Command::ACT1 || command == Command::ACT2) {
    power_act_energy_pj_ += energy;
  } else if (command_meta(command).precharge) {
    power_pre_energy_pj_ += energy;
  } else if (command == Command::RD || command == Command::RDA || command == Command::CASRD) {
    power_read_energy_pj_ += energy;
  } else if (command == Command::WR || command == Command::WRA || command == Command::CASWR) {
    power_write_energy_pj_ += energy;
  } else if (command_meta(command).refresh) {
    power_refresh_energy_pj_ += energy;
  } else if (command_meta(command).rfm) {
    power_rfm_energy_pj_ += energy;
  } else {
    power_control_energy_pj_ += energy;
  }
  apply_thermal_event(physical, cycle, energy);   // 功耗事件 → 热事件
}
```

由 Controller 在每条命令发出时调用。**7 类能量分桶**用 `command_meta()`（semantics.cpp）判断类别。`physical_address(0, &decoded)` 的 0 地址——命令没有字节地址，只有解码坐标，所以传地址 0 并显式给 decoded。

## 二十三、行缓冲操作（第 1084-1239 行）

### 23.1 键构造（1084-1102）

```cpp
StorageKey MemoryImage::bank_key_from_decoded(const DecodedAddress& decoded) const {
  StorageKey key;
  key.channel = decoded.channel; ... key.bank = decoded.bank;
  key.row = -1; key.column = -1;   // 哨兵
  return key;
}
StorageKey MemoryImage::row_key_from_decoded(const DecodedAddress& decoded) const {
  StorageKey key = bank_key_from_decoded(decoded);
  key.row = decoded.row;
  key.column = -1;   // 哨兵
  return key;
}
```

与 `bank_key_from_storage_key`/`row_key_from_storage_key` 等价，但从 DecodedAddress 构造。

### 23.2 writeback_row_buffer

```cpp
void MemoryImage::writeback_row_buffer(StorageKey bank_key, Cycle cycle) {
  auto rb_it = row_buffers_.find(bank_key);
  if (rb_it == row_buffers_.end() || !rb_it->second.open) return;   // 无打开行

  RowBufferEntry& row_buffer = rb_it->second;
  row_buffer_precharges_++;
  if (!row_buffer.dirty) {
    row_buffer_clean_precharges_++;   // 干净行：直接丢弃
    row_buffer.open = false;
    row_buffer.columns.clear();
    return;
  }
  for (auto& [column, block] : row_buffer.columns) {
    Address base = block.physical.logical_line_base;
    block.last_access_cycle = cycle;
    refresh_line_metadata(block);       // 重算 checksum/init/key
    store_backend_line(base, block);    // ★ 写回后端
    row_buffer_dirty_writebacks_++;
  }
  row_buffer.dirty = false;
  row_buffer.open = false;
  row_buffer.columns.clear();
}
```

**PRE 的核心语义**：
- 干净行（无写）→ 直接关闭，无 I/O
- 脏行 → 逐列 `store_backend_line` 写回后端，然后关闭

**注意"最后写者赢"**：同一行的多列各自写回，如果同列写多次，最后一次 store 覆盖。`refresh_line_metadata` 在写回前重算校验和等派生值。

### 23.3 activate_row

```cpp
void MemoryImage::activate_row(const DecodedAddress& decoded, Cycle cycle) {
  StorageKey bank_key = bank_key_from_decoded(decoded);
  StorageKey row_key = row_key_from_decoded(decoded);
  auto& row_buffer = row_buffers_[bank_key];
  if (row_buffer.open) {
    if (row_buffer.row_key == row_key) {
      row_buffer.last_access_cycle = cycle;
      return;                            // 同行使能重复 ACT → 无操作
    }
    row_buffer_forced_closes_++;         // 换行 → 强制关闭旧行
    writeback_row_buffer(bank_key, cycle);
  }
  row_buffer.open = true;
  row_buffer.dirty = false;
  row_buffer.bank_key = bank_key;
  row_buffer.row_key = row_key;
  row_buffer.opened_cycle = cycle;
  row_buffer.last_access_cycle = cycle;
  row_buffer.columns.clear();            // ★ ACT 不加载数据（lazy）
  row_buffer_activations_++;
}
```

**ACT 打开行但不加载数据**——这是 lazy-load 设计：数据在首次 RD/WR 时按需加载。重复 ACT 同一行是无操作（真实 DRAM 中同行使能也基本免费）。ACT 不同行 → 强制关闭旧行（写回脏数据）。

### 23.4 precharge_bank / precharge_all / flush_all_row_buffers

```cpp
void MemoryImage::precharge_bank(const DecodedAddress& decoded, Cycle cycle) {
  writeback_row_buffer(bank_key_from_decoded(decoded), cycle);
}
void MemoryImage::precharge_all(const DecodedAddress& decoded, Cycle cycle) {
  std::vector<StorageKey> targets;
  for (const auto& [bank_key, row_buffer] : row_buffers_) {
    if (!row_buffer.open) continue;
    if (bank_key.channel == decoded.channel) targets.push_back(bank_key);
  }
  for (const auto& bank_key : targets) writeback_row_buffer(bank_key, cycle);
}
void MemoryImage::flush_all_row_buffers(Cycle cycle) {
  // 收集所有打开行后统一写回（避免遍历中修改 map）
  std::vector<StorageKey> targets;
  targets.reserve(row_buffers_.size());
  for (const auto& [bank_key, row_buffer] : row_buffers_) {
    if (row_buffer.open) targets.push_back(bank_key);
  }
  for (const auto& bank_key : targets) writeback_row_buffer(bank_key, cycle);
}
```

**为什么先收集再写回？** `writeback_row_buffer` 修改 `row_buffers_` 中的条目（open=false, columns.clear()），如果在遍历 map 的同时修改条目，迭代器可能失效（unordered_map 在修改 value 时迭代器安全，但 erase/emplace 会失效）。先收集 targets 再遍历 targets 写回，安全且清晰。`precharge_all` 只关指定 channel 的行（PREAB 语义），`flush_all` 关全部。

### 23.5 row_buffer_block（非 const 版本）

```cpp
DataBlock* MemoryImage::row_buffer_block(Address base, const DecodedAddress* decoded,
                                         Cycle cycle, bool create) {
  if (decoded == nullptr) { row_buffer_misses_++; return nullptr; }
  StorageKey line_key = storage_key(base, decoded);
  StorageKey bank_key = bank_key_from_storage_key(line_key);
  StorageKey row_key = row_key_from_storage_key(line_key);
  auto rb_it = row_buffers_.find(bank_key);
  if (rb_it == row_buffers_.end() || !rb_it->second.open ||
      !(rb_it->second.row_key == row_key)) {
    row_buffer_misses_++;        // bank 未打开或打开的是别的行
    return nullptr;
  }
  row_buffer_hits_++;
  RowBufferEntry& row_buffer = rb_it->second;
  row_buffer.last_access_cycle = cycle;
  auto block_it = row_buffer.columns.find(line_key.column);
  if (block_it == row_buffer.columns.end()) {
    DataBlock backing;
    row_buffer_lazy_loads_++;
    if (load_backend_line(base, backing, decoded)) {
      block_it = row_buffer.columns.emplace(line_key.column, std::move(backing)).first;
    } else {
      (void)create;
      block_it = row_buffer.columns.emplace(line_key.column, make_line(base, decoded)).first;
    }
  }
  block_it->second.physical = physical_address(base, decoded);
  block_it->second.storage_key = line_key;
  return &block_it->second;
}
```

**命中判定链**：decoded 非空 → 行缓冲存在 → 打开 → 打开的就是目标行。三级全过 = 行命中。列不存在 → lazy load：后端有数据则加载，无则建空行。

**注意 create 参数实际未使用**（`(void)create`）——旧版本区分"读不创建/写创建"，重构后统一为"无论读写都创建空行"。这是有意的简化：列数据本来就该惰性存在。

### 23.6 row_buffer_block（const 版本）

```cpp
const DataBlock* MemoryImage::row_buffer_block(Address base,
                                               const DecodedAddress* decoded) const {
  if (decoded == nullptr) return nullptr;
  StorageKey line_key = storage_key(base, decoded);
  StorageKey bank_key = bank_key_from_storage_key(line_key);
  StorageKey row_key = row_key_from_storage_key(line_key);
  auto rb_it = row_buffers_.find(bank_key);
  if (rb_it == row_buffers_.end() || !rb_it->second.open ||
      !(rb_it->second.row_key == row_key)) {
    return nullptr;
  }
  auto block_it = rb_it->second.columns.find(line_key.column);
  if (block_it == rb_it->second.columns.end()) return nullptr;
  return &block_it->second;
}
```

**只读版本**：不计数、不 lazy load、不创建。用于 `metadata()` 和 `read_initialized_mask` 等只读查询。

## 二十四、read（第 1241-1302 行）

```cpp
ByteVector MemoryImage::read(Address address, std::size_t size, bool* initialized,
                             const DecodedAddress* decoded) {
  ByteVector out(size, default_value_);
  bool all_initialized = true;
  std::size_t copied = 0;
  while (copied < size) {              // 跨行循环
    Address current = address + copied;
    Address base = line_base(current);
    std::size_t offset = line_offset(current);
    std::size_t chunk = std::min(size - copied, line_size_ - offset);

    read_line_accesses_++;
    DataBlock* rb_block = row_buffer_block(base, decoded, 0, false);
    if (rb_block != nullptr) {        // 路径 A：行缓冲命中
      check_ecc_shadow(*rb_block);
      row_buffer_reads_++;
      // 检查初始化掩码
      for (...) if (rb_block->initialized_mask[offset + i] == 0) all_initialized = false;
      std::copy_n(rb_block->bytes.begin() + offset, chunk, out.begin() + copied);
    } else {                          // 路径 B：后端
      DataBlock block;
      if (!load_backend_line(base, block, decoded)) {
        all_initialized = false;      // 未分配 → 全默认值
      } else {
        // ECC 检查：保存前后状态，若 ECC 修复了数据 → 写回后端
        const ByteVector bytes_before_ecc = block.bytes;
        const std::uint64_t hamming_before_ecc = block.ecc_hamming;
        const bool overall_before_ecc = block.ecc_overall;
        const bool valid_before_ecc = block.ecc_valid;
        const bool uncorrectable_before_ecc = block.ecc_uncorrectable;
        check_ecc_shadow(block);
        // 复制数据
        std::copy_n(block.bytes.begin() + offset, chunk, out.begin() + copied);
        if (block.bytes != bytes_before_ecc ||
            block.ecc_hamming != hamming_before_ecc ||
            block.ecc_overall != overall_before_ecc ||
            block.ecc_valid != valid_before_ecc ||
            block.ecc_uncorrectable != uncorrectable_before_ecc) {
          refresh_line_metadata(block);
          store_backend_line(base, block);   // ★ ECC 修复后持久化
        }
      }
    }
    copied += chunk;
  }
  if (initialized != nullptr) *initialized = all_initialized;
  return out;
}
```

**跨行循环**：请求可能跨多个 cache line（如 100 字节读跨越 2 个 64B 行），每次处理当前行内的 chunk。

**路径 A（行缓冲命中）**：ECC 检查在缓冲块上执行（如果修复了数据，缓冲块已更新，PRE 写回时会持久化）。

**路径 B（后端）**：读后端块 → ECC 检查。**修复检测**：比较 ECC 检查前后 5 个字段（bytes + 4 个 ECC 状态），任何变化都说明 ECC 修复了数据 → 重算元数据并写回后端。这保证"单 bit 错误被纠正后，持久化的是正确数据"。这是重构后新增的健壮性设计。

**未初始化语义**：后端无此地址 → 返回默认值 + `all_initialized=false`。调用方（controller）用 `initialized` 标志决定是否算 uninitialized read。

## 二十五、read_initialized_mask（第 1304-1330 行）

```cpp
ByteVector MemoryImage::read_initialized_mask(Address address, std::size_t size,
                                              const DecodedAddress* decoded) const {
  ByteVector out(size, 0);
  std::size_t copied = 0;
  while (copied < size) {
    ...
    if (const DataBlock* rb_block = row_buffer_block(base, decoded)) {
      std::copy_n(rb_block->initialized_mask.begin() + offset, chunk, out.begin() + copied);
    } else {
      DataBlock block;
      if (load_backend_line(base, block, decoded)) {
        std::copy_n(block.initialized_mask.begin() + offset, chunk, out.begin() + copied);
      }
    }
    copied += chunk;
  }
  return out;
}
```

只读初始化掩码——DFI validator 用它检查 write mask 一致性，不需要读 payload。未分配的行返回全 0（未初始化）。

## 二十六、write（第 1332-1404 行）

```cpp
void MemoryImage::write(Address address, const ByteVector& data, const ByteVector* mask,
                        const DecodedAddress* decoded, std::uint64_t request_id, Cycle cycle) {
  if (mask != nullptr && mask->size() != data.size()) {
    throw std::invalid_argument("memory image write mask size does not match data size");
  }

  std::size_t copied = 0;
  Address first_base = line_base(address);
  while (copied < data.size()) {              // 跨行循环
    Address current = address + copied;
    Address base = line_base(current);
    std::size_t offset = line_offset(current);
    std::size_t chunk = std::min(data.size() - copied, line_size_ - offset);

    write_line_accesses_++;
    // 只对首行使用调用方解码；后续行地址解码可能不同
    const DecodedAddress* line_decoded = (base == first_base) ? decoded : nullptr;
    DataBlock backing;
    const bool backing_exists = load_backend_line(base, backing, line_decoded);
    if (!backing_exists) {
      backing = make_line(base, line_decoded);
    } else if (line_decoded != nullptr) {
      backing.physical = physical_address(base, line_decoded);   // 刷新坐标
    }

    DataBlock* target = row_buffer_block(base, line_decoded, cycle, true);
    bool row_buffer_target = target != nullptr;
    if (target == nullptr) {
      target = &backing;                    // 无行缓冲 → 直接写后端
    } else if (!backing_exists) {
      // 保持既有分配边界：WR 立即分配后端数据块，新字节先留在打开的行缓冲中，
      // 直到 PRE、REF 或最终刷新写回。
      store_backend_line(base, backing);
    }

    std::fill(target->byte_mask.begin(), target->byte_mask.end(), 0);   // 清零本次掩码
    bool wrote_any = false;
    for (std::size_t i = 0; i < chunk; i++) {
      std::size_t src = copied + i;
      if (mask == nullptr || (*mask)[src] != 0) {   // 掩码过滤
        target->bytes[offset + i] = data[src];
        target->initialized_mask[offset + i] = 0xff;
        target->byte_mask[offset + i] = 0xff;
        wrote_any = true;
      }
    }
    if (wrote_any) {
      target->version = next_version_++;            // 全局递增版本
      target->last_writer_request_id = request_id;
      target->last_write_cycle = cycle;
      target->last_access_cycle = cycle;
      if (row_buffer_target) {
        row_buffer_writes_++;
        row_buffers_[bank_key_from_storage_key(target->storage_key)].dirty = true;  // ★ 标记脏
      }
    }
    if (row_buffer_target) {
      target->initialized = all_masked(target->initialized_mask);
      target->checksum = checksum_bytes(target->bytes);
      refresh_ecc_shadow(*target);
      maybe_inject_ecc_error(*target);
    } else {
      refresh_line_metadata(backing);
      refresh_ecc_shadow(backing);
      maybe_inject_ecc_error(backing);
      store_backend_line(base, backing);            // 直接写后端
    }
    copied += chunk;
  }
}
```

### 26.1 三条写入路径

| 场景 | 路径 | 写目标 | 持久化时机 |
|------|------|--------|-----------|
| 行缓冲命中 | A | 行缓冲列 | PRE 写回 |
| 无行缓冲（backing 存在） | B | 后端块（backing） | 立即 |
| 无行缓冲（backing 不存在） | C | 新 backing，先占位存后端 | 立即占位，数据在缓冲 |

### 26.2 关键设计点

**`line_decoded` 只对首行使用**：跨行的写，后续行的解码由 `physical_address(base)` 自动完成（因为 base 地址解码结果与调用方 decoded 可能不同——调用方 decoded 是针对首地址的）。

**"WR 立即分配后端数据块"**（注释明确）：
```cpp
} else if (!backing_exists) {
  store_backend_line(base, backing);   // 空行先入后端，保持分配边界
}
```
新地址的写：后端立即有该地址的行（未初始化、version=0），而实际字节先留在行缓冲中。这样 `unique_written_lines`/`allocated_lines` 统计在写时立即反映，不等到 PRE。

**byte_mask 语义**：每次写先清零再按掩码设置。注意 `target->byte_mask` 清零作用于**整个行**，而写入只覆盖 chunk 范围——所以 byte_mask 最终只标记"本次写调用的 chunk 内被更新的字节"（其他字节 0）。跨行写时每行独立。

**version 是全局计数器** `next_version_`：每次真实写入（wrote_any）取一个全局递增号。ECC 注入按 `version % inject_period` 触发。多行写时每行获得不同 version。

**dirty 标记**：行缓冲目标写后 `row_buffers_[bank_key].dirty = true`——通过 `target->storage_key` 反查 bank。

## 二十七、load_text（第 1406-1518 行）

### 27.1 逐行解析

```cpp
while (std::getline(in, line)) {
  lineno++;
  std::size_t comment = line.find('#');   // 去注释
  if (comment != std::string::npos) line.resize(comment);
  line = trim(line);
  if (line.empty()) continue;             // 跳过空行
  std::vector<std::string> tokens = split_tokens(line);
  std::optional<Address> address;
  ByteVector data;
  ByteVector init_mask;
  DecodedAddress decoded;
  bool has_decoded = false;
  for (std::size_t i = 0; i < tokens.size(); i++) {
    const std::string& token = tokens[i];
    std::size_t eq = token.find('=');
    if (eq == std::string::npos) {
      // 无 = 的 token：第一个是地址，第二个是裸 hex 数据
      if (!address.has_value()) address = parse_address_token(token);
      else if (data.empty()) data = parse_hex_bytes(token);
      else throw std::runtime_error("unexpected token: " + token);
      continue;
    }
    std::string key = lower_token(token.substr(0, eq));
    std::string value = token.substr(eq + 1);
    if (key == "addr" || key == "address") address = parse_address_token(value);
    else if (key == "data" || key == "payload") data = parse_hex_bytes(value);
    else if (key == "init" || key == "initialized" || key == "initialized_mask") init_mask = ...;
    else if (is_decoded_key(key)) { set_decoded_key(decoded, key, value); has_decoded = true; }
    else if (key == "version" || key == "last_writer" || ... ) {
      // 重新加载时接受导出元数据，但根据数据和 spec 重新计算
    } else throw std::runtime_error("unknown key: " + key);
  }
  if (!address.has_value()) throw ... "missing address";
  if (data.empty()) throw ... "missing data";
  ...
}
```

**解析策略**：
- `#` 注释、空白行、`key=value` 与裸 token 混合
- **裸 token 顺序**：第一个是地址，第二个是数据（旧格式兼容）
- **白名单拒绝未知键**：拼错的键立即报错（与配置系统同样的哲学）
- **dump 元数据被接受但忽略**：重新加载时由 spec 和数据重算——防止陈旧/错误的元数据污染

### 27.2 数据应用

```cpp
std::size_t copied = 0;
Address first_base = line_base(*address);
while (copied < data.size()) {
  Address current = *address + copied;
  Address base = line_base(current);
  ...
  DataBlock block;
  if (!load_backend_line(base, block, line_decoded)) {
    block = make_line(base, line_decoded);       // 新行
  } else if (line_decoded != nullptr) {
    block.physical = physical_address(base, line_decoded);
  }
  std::fill(block.byte_mask.begin(), block.byte_mask.end(), 0);
  for (std::size_t i = 0; i < chunk; i++) {
    block.bytes[offset + i] = data[src];
    bool initialized = init_mask.empty() || (src < init_mask.size() && init_mask[src] != 0);
    block.initialized_mask[offset + i] = initialized ? 0xff : 0x00;
    block.byte_mask[offset + i] = initialized ? 0xff : 0x00;
  }
  block.version = 0;               // 初始加载无写入版本
  block.last_writer_request_id = 0;
  block.last_write_cycle = 0;
  refresh_line_metadata(block);
  refresh_ecc_shadow(block);
  store_backend_line(base, block);
  copied += chunk;
}
```

**load 语义**：
- 无 init_mask → 全部标记为已初始化（`init_mask.empty()` → initialized=true）
- 有 init_mask → 按字节标记
- version=0（未写入，只是加载）
- ECC shadow 立即计算（加载的数据被当作可信）
- 直接写后端（不经过行缓冲）

## 二十八、dump_text / dump_csv（第 1520-1641 行）

### 28.1 dump_text

```cpp
void MemoryImage::dump_text(const std::string& path) const {
  ...
  out << "# address ch pc sid rank bg bank row col layer tile_x tile_y tile_z tile_id "
      << "subarray mat_x mat_y mat_id cell_x cell_y microbump_x microbump_y "
      << "version last_writer last_write_cycle checksum ecc_valid ecc_hamming ecc_overall ecc_uncorrectable "
      << "thermal_x thermal_y thermal_z thermal_grid_x thermal_grid_y data init\n";
  std::vector<Address> bases = backend_->all_addresses();   // 已排序
  for (Address base : bases) {
    DataBlock block;
    if (!load_backend_line(base, block)) continue;
    const PhysicalAddress& p = block.physical;
    out << format_address(base)
        << " ch=" << p.channel << " pc=" << p.pseudo_channel << ...
        << " data=" << bytes_to_hex(block.bytes)
        << " init=" << bytes_to_hex(block.initialized_mask) << '\n';
  }
}
```

**36 个 key=value 字段 + 首行注释列名**。与 load_text 完全对称（load 接受这些键）。输出行按地址排序（`backend_->all_addresses()` 已排序）。

### 28.2 dump_csv

```cpp
out << "address,data,init,initialized,version,last_writer,last_write_cycle,checksum,"
    << "ecc_valid,ecc_hamming,ecc_overall,ecc_uncorrectable,"
    << "channel,pseudo_channel,sid,rank,bank_group,bank,row,column,"
    << "stack,die,layer,tile_x,tile_y,tile_z,tile_id,floorplan_cols,floorplan_rows,"
    << "thermal_x,thermal_y,thermal_z,thermal_grid_x,thermal_grid_y,thermal_cols,thermal_rows,"
    << "subarray,mat_x,mat_y,mat_id,cell_x,cell_y,microbump_x,microbump_y\n";
```

**44 列 CSV**：比 text 多了 `initialized` 布尔列、`stack`/`die`、`floorplan_cols/rows`、`thermal_cols/rows`（text 格式通过层级推断）。CSV 适合 pandas 等工具直接加载分析。

## 二十九、二进制格式（第 1643-1794 行）

### 29.1 格式常量与读写辅助

```cpp
constexpr std::uint32_t kBinaryMagic = 0x534D4248;   // "HBMS" little-endian
constexpr std::uint32_t kBinaryVersion = 1;

template <typename T>
void write_le(std::ostream& os, T value) {
  // 所有多字节字段使用 little-endian，保证 x86/ARM 互操作。
  os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
template <typename T>
void read_le(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!is) throw std::runtime_error("binary memory image: unexpected end of file");
}
```

**`0x534D4248` 小端存储时磁盘上是 `48 42 4D 53` = "HBMS"**——与 memory_backend 的 "HBM BACK" 魔数风格一致（可读魔数）。小端假设：x86/ARM 主流平台都是小端，`reinterpret_cast` 直接写内存表示即可。**注意：这假设宿主机是小端**（x86/ARM 都是），文档注释明确此约定。

### 29.2 dump_binary

```cpp
void MemoryImage::dump_binary(const std::string& path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ...
  // 文件头：magic + version + line_size + entry_count + flags（保留）
  write_le(out, kBinaryMagic);
  write_le(out, kBinaryVersion);
  write_le(out, static_cast<std::uint32_t>(line_size_));
  if (backend_->allocated_lines() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("binary memory image v1 cannot store more than 2^32-1 lines");
  }
  write_le(out, static_cast<std::uint32_t>(backend_->allocated_lines()));
  write_le(out, std::uint32_t{0});   // flags reserved

  std::vector<Address> bases = backend_->all_addresses();   // 排序保证确定性
  for (Address base : bases) {
    DataBlock block;
    if (!load_backend_line(base, block)) continue;
    std::uint32_t data_len = static_cast<std::uint32_t>(block.bytes.size());
    write_le(out, base);
    write_le(out, data_len);
    out.write(reinterpret_cast<const char*>(block.bytes.data()), data_len);
    out.write(reinterpret_cast<const char*>(block.initialized_mask.data()), data_len);
  }
  if (!out) throw std::runtime_error("binary memory image write failed: " + path);
}
```

**文件布局**：
```
[magic 4B][version 4B][line_size 4B][entry_count 4B][flags 4B]
[entry_0: base 8B][data_len 4B][payload data_len B][init_mask data_len B]
[entry_1: ...]
```

只存 payload + init_mask——**元数据（version/checksum/ECC）不存，加载时重建**。这是"镜像"而非"完整转储"的设计：镜像用于 checkpoint/golden 比较，重建成本可接受。

### 29.3 load_binary

```cpp
void MemoryImage::load_binary(const std::string& path) {
  ...
  // 逐字段读文件头并校验
  if (magic != kBinaryMagic) throw ... "invalid magic number";
  if (version != kBinaryVersion) throw ... "version mismatch";
  (void)flags;
  if (file_line_size != static_cast<std::uint32_t>(line_size_))
    throw ... "line_size mismatch";

  backend_->clear();          // load 语义 = 替换整个存储区
  row_buffers_.clear();

  for (std::uint32_t i = 0; i < entry_count; i++) {
    Address base = 0; std::uint32_t data_len = 0;
    read_le(in, base); read_le(in, data_len);
    if (data_len != line_size_) throw ... "data_len mismatch";
    DataBlock block;
    block.bytes.resize(data_len);
    block.initialized_mask.resize(data_len);
    block.byte_mask.resize(data_len, 0);
    in.read(...); in.read(...);
    if (!in) throw ...;
    std::fill(block.byte_mask.begin(), block.byte_mask.end(), 0xff);  // 全标记
    block.initialized = all_masked(block.initialized_mask);
    block.checksum = checksum_bytes(block.bytes);
    if (spec_.has_value()) block.physical = physical_address(base);
    else { block.physical.logical_line_base = base; block.physical.byte_offset = 0; }
    block.storage_key = key_from_physical(block.physical);
    refresh_ecc_shadow(block);      // 重建 ECC shadow
    store_backend_line(base, block);
  }
}
```

**三重校验**（magic/version/line_size）+ **逐条目校验**（data_len）+ **替换语义**（clear 后加载）。`byte_mask` 全 0xff 表示"整行都被写过"——这是镜像加载的合理假设。**ECC shadow 由加载数据重算**——镜像本身被当作可信数据。

## 三十、load_file / dump_file（第 1796-1843 行）

```cpp
bool has_extension(const std::string& path, const std::string& ext) {
  if (path.size() < ext.size()) return false;
  return path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

void MemoryImage::load_file(const std::string& path) {
  // 探测文件头是否以 "HBMS" magic 开头 → binary，否则 → text
  std::ifstream probe(path, std::ios::binary);
  if (!probe) throw std::runtime_error("failed to open memory image: " + path);
  char magic[4] = {};
  probe.read(magic, 4);
  probe.close();
  if (magic[0] == 'H' && magic[1] == 'B' && magic[2] == 'M' && magic[3] == 'S') {
    load_binary(path);
  } else {
    load_text(path);
  }
}

void MemoryImage::dump_file(const std::string& path) const {
  std::string lowered = lower_path(path);
  if (has_extension(lowered, ".csv")) dump_csv(path);
  else if (has_extension(lowered, ".bin")) dump_binary(path);
  else dump_text(path);
}
```

**加载自动探测**（读前 4 字节判 magic）、**导出按扩展名**（.bin→binary、.csv→csv、其他→text）。这让 CLI 的 `--memory-image` / `--dump-memory-image` 参数可以透明处理三种格式。

## 三十一、dump_thermal_text（第 1845-1917 行）

```cpp
void MemoryImage::dump_thermal_text(const std::string& path) const {
  ...
  // 首行：模型参数（自描述）
  out << "# model floorplan_enabled=... power_enabled=... thermal_enabled=... "
      << "power_scale=... thermal_ambient_c=... ... power_source=...\n";
  // 次行：列名
  out << "# stack layer thermal_x thermal_y thermal_z tile_x tile_y grid_x grid_y tile_id "
      << "temperature_c energy_pj events ch pc sid rank bg bank row col subarray mat_x mat_y "
      << "mat_id cell_x cell_y microbump_x microbump_y\n";
  // 排序键：stack → layer → y → x（空间连续）
  std::vector<ThermalGridKey> keys;
  for (const auto& [key, tile] : thermal_tiles_) keys.push_back(key);
  std::sort(keys.begin(), keys.end(), [](const ThermalGridKey& a, const ThermalGridKey& b) {
    if (a.stack != b.stack) return a.stack < b.stack;
    if (a.layer != b.layer) return a.layer < b.layer;
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
  });
  out << std::fixed << std::setprecision(4);
  for (const auto& key : keys) {
    const ThermalTileState& tile = thermal_tiles_.at(key);
    const PhysicalAddress& p = tile.physical;
    out << key.stack << ' ' << key.layer << ' ' << key.x << ' ' << key.y << ' '
        << p.thermal_z << ' ' << p.tile_x << ' ' << p.tile_y << ' '
        << p.thermal_grid_x << ' ' << p.thermal_grid_y << ' ' << p.tile_id << ' '
        << tile.temperature_c << ' ' << tile.energy_pj << ' ' << tile.events << ' '
        << p.channel << ' ' << ... << '\n';
  }
}
```

**热力图导出**：每行一个热 tile，含温度/能量/事件数和该 tile 首次触达的物理坐标。排序 (stack, layer, y, x) 使输出按空间顺序——y 优先于 x 是因为视觉上"先按行再按列"更接近 grid 布局。温度用 `fixed` + 4 位小数保证列对齐。

## 三十二、全局工具函数（第 1919-1977 行）

### 32.1 parse_hex_bytes

```cpp
ByteVector parse_hex_bytes(const std::string& text) {
  std::string hex = compact_hex(text);
  if (hex.empty()) return {};
  if (hex.size() % 2 != 0) hex.insert(hex.begin(), '0');   // 奇数 → 前补 0
  ByteVector out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int hi = hex_value(hex[i]);
    int lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) throw std::invalid_argument("invalid hex byte string: " + text);
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}
```

宽容 + 严格结合：接受 `0x`/`_`/`:`/空白，但**非法字符立即报错**。奇数位前补 0（`"abc"` → `0abc` = 2 字节）——对用户输入友好。

### 32.2 bytes_to_hex / format_address

```cpp
std::string bytes_to_hex(const ByteVector& bytes) {
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (std::uint8_t byte : bytes) os << std::setw(2) << static_cast<unsigned>(byte);
  return os.str();
}
std::string format_address(Address address) {
  std::ostringstream os;
  os << "0x" << std::hex << std::setfill('0') << std::setw(16) << address;
  return os.str();
}
```

`static_cast<unsigned>` 避免 `uint8_t` 被当作 char 输出。`format_address` 16 位宽 + `0x` 前缀统一地址格式。

### 32.3 make_request_payload — 确定性伪随机 payload

```cpp
ByteVector make_request_payload(Address address, std::uint64_t request_id, std::size_t size) {
  ByteVector out(size, 0);
  std::uint64_t state = address ^ (request_id * 0x9e3779b97f4a7c15ull) ^ 0xd1b54a32d192ed03ull;
  for (std::size_t i = 0; i < size; i++) {
    state ^= state >> 12;    // xorshift 三个移位
    state ^= state << 25;
    state ^= state >> 27;
    out[i] = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dull) >> 56);  // 取高位字节
  }
  return out;
}
```

**目的**：合成流量（stream/random）的写数据需要真实 payload（否则数据校验无意义）。这个函数生成**确定性**的伪随机字节——同一 (address, request_id) 永远生成同一 payload，使实验可复现。

**实现**：xorshift64* 变体（三个移位 + 乘法散列）。种子 = 地址 ⊕ request_id×黄金比例常数 ⊕ 固定盐。输出取最高字节（`* 0x2545f... >> 56`）——取高位字节比低位字节的随机性更好。

### 32.4 normalize_mask

```cpp
ByteVector normalize_mask(const ByteVector& mask, std::size_t size) {
  if (mask.empty()) return ByteVector(size, 0xff);   // 无掩码 = 全部写入
  ByteVector out(size, 0);
  for (std::size_t i = 0; i < size; i++) {
    out[i] = i < mask.size() && mask[i] != 0 ? 0xff : 0x00;
  }
  return out;
}
```

把任意非零掩码值归一化为 0xff/0x00（两值制），空掩码 = 全写。这统一了 trace 中 `mask=00ff0000` 这类写法与程序内部两值掩码的语义。

---

# 第三部分：设计原则总结

## 33. 分层与职责

```
MemoryImage（data.cpp）          ← 语义层：知道"写意味着什么、行缓冲是什么"
    │ 委托
MemoryBackend（memory_backend.cpp）← 介质层：只管"存哪里、怎么持久化"
```

MemoryImage 不关心 sparse/mmap/chunk 的区别；MemoryBackend 不关心 DRAM 语义。中间通过 `BackendLine`（无物理坐标、无 byte_mask 的纯持久状态）解耦。

## 34. 关键设计决策清单

| 决策 | 理由 |
|------|------|
| 存储委托后端（`unique_ptr<MemoryBackend>`） | 三种介质可替换，不影响协议语义 |
| PhysicalAddress 全部派生不持久化 | options 变化时坐标自动刷新，避免陈旧数据 |
| byte_mask vs initialized_mask 分离 | 最近写入 vs 历史初始化，两种语义分开 |
| lazy-load 行缓冲（ACT 不加载数据） | 稀疏访问下避免无谓 I/O |
| 全局 version 计数器 | 写顺序追踪 + ECC 注入触发 |
| ECC 修复后写回后端（read 路径） | 保证持久化数据与 shadow 一致 |
| 拓扑扫描 limit | 大规模后端避免昂贵遍历 |
| 二进制镜像不存元数据 | 镜像=checkpoint，加载时重建更安全 |
| 确定性 payload（xorshift） | 实验可复现 |
| SECDED 用 overall 位区分单/双 bit 错 | 汉明码理论的标准扩展 |

## 35. 数据生命周期全景

```
write()  ──→ 行缓冲列（dirty）──PRE──→ 后端 ──dump──→ txt/csv/bin 文件
    │                                    │
    └──无缓冲──→ 后端（立即）             └──load──→ 重新构造 DataBlock
                                                （坐标/ECC/checksum 重建）

read()   ──→ 行缓冲命中？──是──→ 缓冲数据 + ECC 检查
              └──否──→ 后端加载 → ECC 检查 → 修复则写回

record_command_event() ──→ 能量分桶 ──→ apply_thermal_event() ──→ 热网格升温 + 耦合
```
