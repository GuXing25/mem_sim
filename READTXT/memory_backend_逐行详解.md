# memory_backend.cpp 逐行代码详解

本文档按符号和职责解释 `src/core/memory_backend.cpp`，说明每段代码“做什么”以及
“为什么这样设计”。章节中的行号只作相对导航，以符号名和当前源码为准。

配套头文件：`include/hbm_sim/core/memory_backend.hpp`（80 行，定义接口）。

---

## 一、头文件与常量定义（第 1-28 行）

### 1.1 包含的头文件

```cpp
#include "hbm_sim/core/memory_backend.hpp"   // 自身接口
#include <algorithm>    // std::min, std::sort, std::copy, std::fill, std::min_element
#include <array>        // std::array
#include <bit>          // std::countr_zero — C++20 的位操作，用于稀疏位图遍历
#include <cctype>       // std::tolower — 用于 parse_memory_backend_kind
#include <cerrno>       // errno — 系统调用失败时取错误码
#include <cstring>      // std::strerror, std::memcpy
#include <fcntl.h>      // O_RDWR, O_CREAT — POSIX 文件打开标志
#include <limits>       // std::numeric_limits
#include <stdexcept>    // std::runtime_error, std::invalid_argument, std::overflow_error
#include <string_view>  // std::string_view — 零拷贝字符串引用
#include <sys/mman.h>   // mmap, munmap, msync — POSIX 内存映射
#include <sys/stat.h>   // fstat — 获取文件元数据
#include <type_traits>  // std::is_trivially_copyable_v — 编译期类型检查
#include <unistd.h>     // pread, pwrite, ftruncate, fsync, close
#include <unordered_map>
```

**关键设计决策**：`<sys/mman.h>` 和 `<unistd.h>` 是 POSIX 特有头文件。这意味着 memory_backend 刻意绑定 POSIX 平台（Linux/macOS/BSD），不追求 Windows 兼容。原因是 `mmap_sparse` 和 `chunk_file` 的性能优势（零拷贝 mmap 读取、OS page cache）完全依赖 POSIX API。在 Windows 上实现等效功能需要完全不同的代码路径（`CreateFileMapping` 等），项目选择不维护那类复杂性。

### 1.2 匿名命名空间

```cpp
namespace hbm_sim {
namespace {
```

整个实现文件的内容（除了最后两个工厂函数）都放在匿名命名空间 `namespace {` 中。匿名命名空间中的符号具有**内部链接**（internal linkage），等价于每个符号前加 `static`，但适合类/结构体。效果是：三个 Backend 实现类、DiskHeader、所有工具函数都是翻译单元私有的，外部代码完全看不到。唯一能访问这些实现的入口是通过头文件暴露的 `make_memory_backend()` 工厂函数。

### 1.3 文件格式常量

```cpp
constexpr std::array<char, 8> kMetaMagic{'H', 'B', 'M', 'B', 'A', 'C', 'K', '\0'};
```
用 `std::array<char, 8>` 而非 `uint64_t`，因为标准布局 struct 中 `char[8]` 没有特殊对齐要求——如果 magic 放在 struct 头部且类型为 `uint64_t`，会强制 struct 8 字节对齐并在后面产生 padding。`char[8]` 放首位不受对齐影响，后面紧接 `uint32_t version`（4 字节对齐）无空隙。

值为 `"HBM BACK\0"`，这样用 `hexdump -C` 查看 `.meta` 文件时前 8 字节清晰可读，一眼能识别文件类型。

```cpp
constexpr std::uint32_t kBackendVersion = 3;
```

文件格式版本。version 2 曾统一 mmap_sparse 和 chunk_file 的元数据布局；当前
version 3 又加入完整物理坐标/访问元数据。布局改变会递增版本，旧文件由
`validate_header()` 明确拒绝，而不是被错误解析。

```cpp
constexpr std::uint8_t kInitializedFlag      = 1u << 0;  // 0x01
constexpr std::uint8_t kEccOverallFlag       = 1u << 1;  // 0x02
constexpr std::uint8_t kEccValidFlag         = 1u << 2;  // 0x04
constexpr std::uint8_t kEccUncorrectableFlag  = 1u << 3;  // 0x08
```

四个位掩码，对应 `BackendLine` 中的四个 bool 字段。`DiskLineMeta` 用一个 `uint8_t flags` 字节打包这四个 bool，节省磁盘空间。如果用 4 个 `bool`（每个 1 字节）加 padding，会多占 7 字节。

---

## 二、DiskHeader — 文件头部元数据（第 29-37 行）

```cpp
struct DiskHeader {
  std::array<char, 8> magic{};              // 偏移 0,  8 字节
  std::uint32_t version = 0;                // 偏移 8,  4 字节
  std::uint32_t record_size = 0;            // 偏移 12, 4 字节
  std::uint64_t line_size = 0;              // 偏移 16, 8 字节
  std::uint64_t capacity_bytes = 0;         // 偏移 24, 8 字节
  std::array<std::uint64_t, 4> reserved{};  // 偏移 32, 32 字节
};
static_assert(sizeof(DiskHeader) == 64);
```

这是 `.meta` 文件的第一个结构体，也是 `chunk_file` 后端的 `meta_` 文件描述符的第一个结构体。它让后端文件做到**自描述**：只凭文件内容就能确定其格式、大小和状态。

### 2.1 为什么要有 DiskHeader？

考虑以下场景：你两周前用 `line_size=32, capacity=4GB` 运行了一次实验，现在用 `line_size=64, capacity=64GB` 重新打开了同一个数据文件。如果没有 header，程序会静默地用错误的参数解析文件，产生垃圾数据且不知道。DiskHeader 的 `validate_header()` 检查就是防止这种情况——参数不匹配立即报错。

### 2.2 逐字段解释

**`magic`（8 字节）**：文件类型签名。值为 `{'H','B','M','B','A','C','K','\0'}`。`hexdump -C data.bin.meta | head -1` 输出 `00000000  48 42 4d 42 41 43 4b 00 ...`（"HBM BACK\0"），人类和设备都能识别。

**`version`（4 字节）**：当前为 3。`validate_header()` 拒绝版本不匹配的文件，
避免新代码按错误布局解释旧文件。

**`record_size`（4 字节）**：`sizeof(DiskLineMeta)` 的实际值。它与 version 一起
检查当前 ABI 下的记录布局，大小不一致会立即报错。

**`line_size`（8 字节）**：创建文件时使用的 `line_size`。如果当前 `line_size=64` 但文件是用 `line_size=32` 创建的，字节偏移计算会全部错误。此字段在 `validate_header()` 中拦截这类配置变更。

**`capacity_bytes`（8 字节）**：创建文件时使用的 `capacity_bytes`。作用同上——数据文件的大小必须与 capacity 一致。

**`reserved`（32 字节 = 4 × uint64_t）**：预留扩展空间。当前用途：
- `reserved[0]` = `allocated_lines_`（已分配行数）。在 `flush()` 时从内存写回，重新打开时从 `validate_header_counters()` 恢复。
- `reserved[1]` = `unique_written_lines_`（version>0 的唯一写入行数）。同上逻辑。
- `reserved[2]` 和 `reserved[3]`：未使用，未来可存更多计数器/标志。

为什么用 4 个而非 2 个？因为目标是 DiskHeader = 精确 64 字节。设计计算：
- 不加 reserved：8+4+4+8+8 = 32 字节
- 加 2 个 uint64_t：32+16 = 48 字节（不齐）
- 加 4 个 uint64_t：32+32 = 64 字节 ✓

64 字节对齐同时满足：(1) x86 cache line 大小（对齐访问更快）；(2) 常见磁盘扇区 512/4096 的整除；(3) 未来如果前 4 个字段不够，还有两个 reserved 槽位可用，不用改变结构体大小。

### 2.3 static_assert 的意义

```cpp
static_assert(sizeof(DiskHeader) == 64);
```

三重保护：(1) 确保字段布局与磁盘文件完全一致（不同平台/编译器可能有不同 padding 规则）；(2) 如果有人在 struct 中添加/删除字段，编译立即失败，强制更新版本号和所有相关逻辑；(3) 文档作用——任何人读代码就知道 "DiskHeader 精确 64 字节"。

---

## 三、DiskLineMeta — 每行元数据（第 40-54 行）

```cpp
struct DiskLineMeta {
  std::uint64_t version = 0;                       // 8B, offset 0
  std::uint64_t last_writer_request_id = 0;        // 8B, offset 8
  std::uint64_t last_write_cycle = 0;              // 8B, offset 16
  std::uint64_t last_access_cycle = 0;             // 8B, offset 24
  std::uint64_t ecc_hamming = 0;                   // 8B, offset 32
  std::uint64_t ecc_error_injections = 0;          // 8B, offset 40
  std::uint32_t checksum = 0;                      // 4B, offset 48
  std::int32_t ecc_parity_bits = 0;                // 4B, offset 52
  std::array<std::int32_t, 9> storage{};           // 36B, offset 56
  std::uint8_t flags = 0;                          // 1B, offset 92
  std::array<std::uint8_t, 7> reserved{};          // 7B, offset 93
};                                                   // 常见 64-bit ABI 下总计 104 字节
static_assert(std::is_trivially_copyable_v<DiskLineMeta>);
```

### 3.1 设计原则

`DiskLineMeta` 是 `BackendLine` 的磁盘序列化形式。它包含 `BackendLine` 中除了两个可变长度字段（`bytes` 和 `initialized_mask`，存在数据文件中按 `index * line_size` 定位）和 `storage_key`（编码到 `storage[]` 数组）外的所有字段。

**为什么用固定大小结构体而非序列化格式（Protobuf/JSON/手工序列化）？**
- 固定大小 → 每行元数据可通过 `offset = header_size + index * sizeof(DiskLineMeta)` 直接定位。O(1) 随机访问。Protobuf 需要线性扫描。
- trivially copyable → 可以用 `std::memcpy` 直接写到文件/从文件读。零序列化开销。
- 整数字段显式位宽可固定字段宽度，但本后端直接保存宿主结构体，仍受 ABI、padding
  和字节序影响。因此 backend 四文件是同平台持久状态，不承诺跨架构交换；需要跨
  平台搬运时应使用 `MemoryImage` 的显式 little-endian checkpoint。

### 3.2 各字段含义

**`version`（8B）**：写入版本号。与 `BackendLine::version` 对应。0 = 从未写入。`unique_written_lines_` 计数器只统计 `version > 0` 的行。

**`last_writer_request_id`（8B）**：最后一次写这个地址的请求 ID。debug 时如果 payload 不对，可以反查是哪个请求写入的。

**`last_write_cycle` / `last_access_cycle`（各 8B）**：两个时间戳。`last_write_cycle` 是最后一次写入的 cycle，`last_access_cycle` 是最后一次读写（含 load）的 cycle。用于数据新鲜度审计。

**`ecc_hamming`（8B）**：SECDED（单纠错双检错）的汉明码校验子。虽然用了 64-bit 存储，但只有 `ecc_parity_bits` 个最低位有效（对 512-bit payload 约 10 位）。用 uint64_t 而非更小的类型是为了 8 字节对齐。

**`ecc_error_injections`（8B）**：ECC 错误注入计数器。用于测试 ECC 纠正/检测路径——可以通过 `ecc_inject_period` 配置每 N 次写入注入一个单 bit 翻转。

**`checksum`（4B）**：payload bytes 的 FNV-1a 32-bit 哈希。不是 ECC（不能纠正），但提供快速完整性检查（比 ECC 更快，单次哈希 vs 纠错算法）。

**`ecc_parity_bits`（4B）**：ECC 校验位的数量（SECDED 算法确定）。存到磁盘以便重新打开时准确知道 ecc_hamming 中哪些位是有效的。

**`storage[8]`（32B = 8 × int32_t）**：`StorageKey` 的磁盘编码。8 个字段按固定顺序展开到定长数组：
```cpp
meta.storage = {
    line.storage_key.channel,        // storage[0]
    line.storage_key.pseudo_channel, // storage[1]
    line.storage_key.sid,            // storage[2]
    line.storage_key.rank,           // storage[3]
    line.storage_key.bank_group,     // storage[4]
    line.storage_key.bank,           // storage[5]
    line.storage_key.row,            // storage[6]
    line.storage_key.column,         // storage[7]
};
```
用 `int32_t` 而非 `int` 固定单字段宽度；这减少 ABI 差异，但不等于整个结构体文件
具备跨平台兼容性。

**`flags`（1B）**：4 个 bool 值用位掩码打包到 1 字节：
```
bit 0 = kInitializedFlag       (0x01) — initialized ?
bit 1 = kEccOverallFlag        (0x02) — ecc_overall ?
bit 2 = kEccValidFlag          (0x04) — ecc_valid ?
bit 3 = kEccUncorrectableFlag  (0x08) — ecc_uncorrectable ?
bit 4-7 = 保留
```
用 1 字节 vs 4 个 bool（4 字节）+ 3 字节 padding = 7 字节节省。

**`reserved[7]`（7B）**：为未来预留。这 7 字节让 `DiskLineMeta` 结构体末尾对齐到 8 字节边界。

### 3.3 static_assert(is_trivially_copyable_v)

确保 `DiskLineMeta` 可以用 `std::memcpy` 直接复制到/从文件缓冲区。如果结构体包含非 trivially copyable 的成员（如 std::string, std::vector），此断言会在编译期报错。

---

## 四、工具函数（第 56-103 行）

### 4.1 checked_mul — 防溢出乘法

```cpp
std::uint64_t checked_mul(std::uint64_t lhs, std::uint64_t rhs,
                          std::string_view what) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(what) + " size overflow");
  }
  return lhs * rhs;
}
```

**为什么需要？** 后端需要计算 `line_count * sizeof(DiskLineMeta)` 来确定元数据文件
大小。对于 `capacity = 2^38 bytes (256GB)`、`line_size = 64`，约 4G 行；按常见
104B 记录约需 416GiB 逻辑元数据空间。恶意或错误的超大参数还可能让乘法溢出；
`checked_mul` 会在溢出前拒绝。

**为什么用 `lhs != 0 && rhs > max / lhs` 而非 `a * b > max`？** `a * b` 已经溢出了，结果不可信。检测必须在乘法前做。

### 4.2 checked_size — 防 size_t 截断

```cpp
std::size_t checked_size(std::uint64_t value, std::string_view what) {
  if (value > static_cast<std::uint64_t>(
      std::numeric_limits<std::size_t>::max())) {
    throw std::overflow_error(std::string(what)
        + " does not fit host size_t");
  }
  return static_cast<std::size_t>(value);
}
```

POSIX 的 `mmap` 和 `munmap` 的 length 参数是 `size_t`。在 64 位系统上 `size_t` = `uint64_t`，无事。在 32 位系统上 `size_t` = `uint32_t`，如果值为 4GB+ 就截断了。此函数明确检查。

### 4.3 ceil_div — 整数上取整

```cpp
std::uint64_t ceil_div(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}
```

`ceil_div(line_count, 8)` = 位图需要多少字节。`line_count = 10` → `10/8 + 1 = 2` 字节（16 bits，覆盖 10 个 bit）。

### 4.4 bit_is_set / set_bit — 位图读写

```cpp
bool bit_is_set(const std::uint8_t* bits, std::uint64_t index) {
  return (bits[index / 8] &
          static_cast<std::uint8_t>(1u << (index % 8))) != 0;
}
```

`index / 8`：找到所在字节。`1u << (index % 8)`：该字节内的掩码。一条 `&` 后判断是否非零。

```cpp
void set_bit(std::uint8_t* bits, std::uint64_t index, bool value) {
  const std::uint8_t mask =
      static_cast<std::uint8_t>(1u << (index % 8));
  if (value) bits[index / 8] |= mask;
  else       bits[index / 8] &=
              static_cast<std::uint8_t>(~mask);
}
```

**为什么 `&= ~mask` 前要 cast？** `mask` 是 `uint8_t` 类型（值如 0x04）。按 C++ 的整型提升规则，`~mask` 首先将 mask 提升为 `int`（值 0x00000004），`~` 后变为 0xFFFFFFFB（32 位全 1 除了 bit 2）。赋值给 `bits[index/8]`（uint8_t）时截断低 8 位，结果是 0xFB——比特位被正确清除。但先 cast 到 uint8_t 再做 `~`：`~(uint8_t(0x04))` → 对 0x04 取反得 0xFB（8 位）→ 提升到 int 时值仍为 0x000000FB。两种写法结果相同，但显式 cast 避免了对默认整型提升规则的依赖，阅读意图更清晰。

### 4.5 visit_set_bits — 稀疏位图遍历

```cpp
template <typename Visitor>
void visit_set_bits(const std::uint8_t* bits,
                    std::uint64_t bit_count,
                    Visitor&& visitor) {
  const std::uint64_t byte_count = ceil_div(bit_count, 8);
  for (std::uint64_t byte_index = 0; byte_index < byte_count;
       byte_index++) {
    unsigned value = bits[byte_index];
    while (value != 0) {
      const unsigned bit = std::countr_zero(value);
      const std::uint64_t index = byte_index * 8 + bit;
      if (index >= bit_count || !visitor(index)) {
        return;
      }
      value &= value - 1;
    }
  }
}
```

**算法原理**（这是整个文件中算法上最精妙的部分）：

普通位图遍历是：
```cpp
for (uint64_t i = 0; i < bit_count; i++) {
  if (bit_is_set(bits, i)) visitor(i);
}
```
复杂度 O(总位数)。对于 `capacity=64GB, line_size=64`：`1G` 个 bit → 128MB 位图 → 遍历 1G 次，哪怕只有 10 行已分配。这是不可接受的。

`visit_set_bits` 的优化：
1. **跳过全零字节**：`while (value != 0)` — 如果整个字节都是 0（8 个连续 bit 全未设置），直接 `continue` 到下一个字节。稀疏场景下大多数字节都是 0，一跳 8 位。
2. **`std::countr_zero(value)`**：C++20 引入的函数，编译为 CPU 指令 `tzcnt`（x86）或 `clz + 反转`（ARM）。直接找到最低位的 1 的位置，1 个时钟周期完成。
3. **`value &= value - 1`**：Brian Kernighan 的经典技法 —— 清除最低位的 1。例如 `value = 0b101100`，`value - 1 = 0b101011`，`value & (value-1) = 0b101000`。每次循环恰好处理一个设置的 bit，零开销跳过中间的 0。

复杂度：**O(设置的 bit 数 + 全零字节数)**，对稀疏位图接近 O(已分配的行数)。

**模板参数 Visitor**：编译期多态。lambda 传入时完全内联，无虚函数调用开销。`visitor(index)` 返回 `false` 时立即退出（用于 `address_for_storage_key` 的"找到即停"语义）。

### 4.6 require_data_path 和 sidecar_path

```cpp
std::string require_data_path(const MemoryBackendOptions& options) {
  if (options.data_file.empty()) {
    throw std::invalid_argument(
        "file-backed memory backend requires memory_data_file");
  }
  return options.data_file;
}
```

Sparse 后端不需要文件（纯内存），但 mmap_sparse 和 chunk_file **必须有**文件。此检查在构造时立即报错，而非在第一次 store 时。

```cpp
std::string sidecar_path(const std::string& configured,
                         const std::string& data_path,
                         std::string_view suffix) {
  return configured.empty()
             ? data_path + std::string(suffix)
             : configured;
}
```

"sidecar 文件"是指与主数据文件配套的辅助文件（`data.bin` + `data.bin.init` + `data.bin.meta` + `data.bin.present`）。默认行为：用户只需指定 `--memory-data-file outputs/data.bin`，其余三个文件自动生成为 `outputs/data.bin.init`、`outputs/data.bin.meta`、`outputs/data.bin.present`。如果用户想放在不同位置（例如 init 文件放在 SSD 但数据文件放在 HDD），可以用 `--memory-init-file` 等显式指定。

---

## 五、FileDescriptor — RAII 文件描述符封装（第 118-203 行）

```cpp
class FileDescriptor {
  int fd_ = -1;
  std::string path_;
  std::uint64_t original_size_ = 0;
};
```

### 5.1 设计意图

C 风格的 `int fd` 有几个问题：
- 如果忘记 `close()`，文件描述符泄漏。
- 如果函数中间抛异常，`close()` 被跳过。
- 复制 fd 会导致双重关闭；移动时源对象仍持有旧 fd。
- 没有与之关联的路径名和期望大小信息。

FileDescriptor 用 RAII 解决以上全部问题。

### 5.2 构造函数

```cpp
FileDescriptor(const std::string& path, std::uint64_t expected_size)
    : path_(path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw std::runtime_error("failed to open memory backend file "
        + path + ": " + std::strerror(errno));
  }
  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    throw std::runtime_error("failed to stat memory backend file "
        + path + ": " + std::strerror(errno));
  }
  original_size_ = static_cast<std::uint64_t>(st.st_size);
  if (original_size_ == 0) {
    resize(expected_size);            // 新文件：扩展到指定大小
  } else if (original_size_ != expected_size) {
    throw std::runtime_error(         // 旧文件大小不匹配：报错
        "memory backend file size mismatch for " + path
        + ": expected " + std::to_string(expected_size)
        + ", got " + std::to_string(original_size_));
  }
}
```

**`O_RDWR | O_CREAT`**：读写模式 + 不存在则创建。不设 `O_TRUNC`（截断），因为已有数据的旧文件应该保留。

**`fstat` → `st.st_size`**：获取当前文件大小。Linus 说过 "never trust stat"，但这里用 `fstat`（基于 fd 而非路径名）是可靠且原子的。

**三种分支**：
1. `size == 0`：新文件 → `ftruncate` 扩展到期望大小
2. `size == expected`：旧文件存在且匹配 → 保留
3. `size != expected`：不匹配 → 报错。防止用 64GB 配置打开 4GB 文件导致越界。

### 5.3 移动语义

```cpp
FileDescriptor(FileDescriptor&& other) noexcept {
  *this = std::move(other);
}
FileDescriptor& operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    close();           // 先释放自己持有的 fd
    fd_ = other.fd_;   // 接管源对象的 fd
    path_ = std::move(other.path_);
    original_size_ = other.original_size_;
    other.fd_ = -1;    // 源对象不再持有 fd
  }
  return *this;
}
```

**为什么 `other.fd_ = -1` 至关重要？** 没有这行，两个 FileDescriptor 对象同时持有同一个 fd。析构时 `~FileDescriptor()` 会对同一 fd 调两次 `close()`——第二次 close 可能关闭了已被其他线程重用的 fd，后果是随机的数据损坏。这是 C++ 移动语义的经典陷阱。

复制构造/赋值被显式删除：
```cpp
FileDescriptor(const FileDescriptor&) = delete;
FileDescriptor& operator=(const FileDescriptor&) = delete;
```
文件描述符是唯一资源，复制没有意义（两个独立对象不能同时拥有同一个 fd）。

### 5.4 析构函数

```cpp
~FileDescriptor() { close(); }
```

RAII 核心：对象离开作用域时自动 `close(fd_)`，无论是以正常 return、异常抛出还是函数末尾。

### 5.5 close() — 私有方法

```cpp
void close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;   // 关键：置为 sentinel 值，防止双重关闭
  }
}
```

**为什么需要 `fd_ = -1`？** 如果 `close()` 后不置 -1，析构函数可能再次调用 `close()`（某些异常路径）。第二次 `close(-1)` → `EBADF`（坏的文件描述符）→ 无害但有噪音。而双重 `close(some_fd)` 可能关闭已被重用的 fd（比如另一个线程刚打开的日志文件），产生难以调试的 bug。`fd_ = -1` 是经典的 sentinel 模式。

### 5.6 resize / reset_sparse / sync

```cpp
void resize(std::uint64_t size) {
  if (size > static_cast<std::uint64_t>(
      std::numeric_limits<off_t>::max())) {
    throw std::overflow_error(
        "memory backend file exceeds host off_t range: " + path_);
  }
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
    throw std::runtime_error(
        "failed to resize memory backend file " + path_
        + ": " + std::strerror(errno));
  }
}
```

`ftruncate` 将文件扩/缩到指定大小。对于 mmap_sparse 数据文件，`ftruncate(64GB)` 在 ext4/xfs 上产生一个**稀疏文件**——磁盘上只分配文件系统元数据（inode + extent map），不实际分配数据块。当第一次 `mmap` 写入时才分配物理块。这意味着 mmap_sparse 后端的 "capacity" 设置很大但不占磁盘，只有真正写入的行才占空间。

```cpp
void reset_sparse(std::uint64_t size) {
  resize(0);     // 释放所有数据块
  resize(size);  // 重新扩展到原大小，产生新稀疏文件
}
```

`resize(0) → resize(size)` 序列**保留文件 inode**。与 `unlink + create` 不同，文件路径不变，任何已持有此路径的代码不受影响。`resize(0)` 释放全部数据块，`resize(size)` 产生新的空洞。两步操作在 ext4/xfs 上是 O(1) 的元数据操作。

```cpp
void sync() const {
  if (::fsync(fd_) != 0) {
    throw std::runtime_error(
        "failed to sync memory backend file " + path_
        + ": " + std::strerror(errno));
  }
}
```

`fsync` 将操作系统 page cache 中的脏页刷到磁盘。对于仿真实验的数据文件，`fsync` 在 `flush()` 中被调用，保证仿真完成的 checkpoint 在系统崩溃后仍然可恢复。

---

## 六、pread/pwrite 循环（第 205-241 行）

```cpp
void pread_fill(int fd, void* data, std::size_t size,
                std::uint64_t offset) {
  auto* out = static_cast<std::uint8_t*>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t count = ::pread(fd, out + done, size - done,
                             static_cast<off_t>(offset + done));
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("memory backend pread failed: "
          + std::string(std::strerror(errno)));
    }
    if (count == 0) {
      std::fill(out + done, out + size, 0);
      return;
    }
    done += static_cast<std::size_t>(count);
  }
}
```

### 6.1 为什么不能用一次 pread？

POSIX **不保证** `pread(fd, buf, n, off)` 一次读完 n 字节。返回的 `count` 可以小于请求量。原因包括：
- **信号中断**（EINTR）：`SIGALRM`, `SIGCHLD`, `SIGPROF` 等信号可能在任何时刻到达。`pread` 被中断后返回已读取的字节数（可能 >0 但 < n）。
- **短读取**：某些文件系统（NFS、FUSE）可能在读到空洞（sparse 区域）时只返回已填充的部分。
- **内核内部缓冲限制**：管道/FIFO 有 `PIPE_BUF` 限制，虽然普通文件不受此限。

`while (done < size)` 循环保证无论多少部分读取，最终 buffer 被完全填满。

### 6.2 EINTR 重试

```cpp
if (count < 0) {
  if (errno == EINTR) continue;
  throw ...
}
```

`EINTR`（被信号中断）不是错误——是正常的系统行为。重试即可。其他 errno（如 EIO = I/O 错误）是真正的硬件/文件系统故障，无法恢复，直接抛异常。

### 6.3 count == 0 的特殊处理

```cpp
if (count == 0) {
  std::fill(out + done, out + size, 0);
  return;
}
```

`pread` 返回 0 表示**读到了文件末尾**（offset 超出文件已有数据范围）。在稀疏文件场景中，未写入的区域物理上不存在于磁盘。但 `chunk_file` 后端需要把这些区域视为全零数据返回给上层。`std::fill(0)` 将剩余空间填充为零，与稀疏区域的语义一致。

### 6.4 pwrite_all

```cpp
void pwrite_all(int fd, const void* data, std::size_t size,
                std::uint64_t offset) {
  const auto* input = static_cast<const std::uint8_t*>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t count = ::pwrite(fd, input + done, size - done,
                              static_cast<off_t>(offset + done));
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("memory backend pwrite failed: "
          + std::string(std::strerror(errno)));
    }
    done += static_cast<std::size_t>(count);
  }
}
```

与 `pread_fill` 相同的 `while` + `EINTR` 逻辑。但没有 `count == 0` 分支——`pwrite` 在正常磁盘文件上不会返回 0（返回 0 意味着写入 0 字节，这不会发生），因此不需要专门处理。

### 6.5 为什么用 pread/pwrite 而非 lseek+read/write？

`pread/pwrite` 是**原子**操作——它们不改变文件偏移量。在多线程场景中，两个线程可以同时读写同一文件的不同偏移而不互相干扰（无需锁）。如果用 `lseek + read`，线程 A 的 `lseek` 和 `read` 之间线程 B 可能改变偏移量，导致读到错误位置。chunk_file 后端用 `pread/pwrite`，天然线程安全。

---

## 七、encode_meta / decode_meta — 序列化/反序列化（第 276-326 行）

### 7.1 encode_meta（BackendLine → DiskLineMeta）

```cpp
DiskLineMeta encode_meta(const BackendLine& line) {
  DiskLineMeta meta;
  meta.version = line.version;
  meta.last_writer_request_id = line.last_writer_request_id;
  meta.last_write_cycle = line.last_write_cycle;
  meta.last_access_cycle = line.last_access_cycle;
  meta.ecc_hamming = line.ecc_hamming;
  meta.ecc_error_injections = line.ecc_error_injections;
  meta.checksum = line.checksum;
  meta.ecc_parity_bits = line.ecc_parity_bits;
  // StorageKey 8 维压缩到定长数组
  meta.storage = {
      line.storage_key.channel,
      line.storage_key.pseudo_channel,
      line.storage_key.sid,
      line.storage_key.rank,
      line.storage_key.bank_group,
      line.storage_key.bank,
      line.storage_key.row,
      line.storage_key.column,
  };
  // 4 个 bool 打包到 1 字节 flags
  if (line.initialized)       meta.flags |= kInitializedFlag;
  if (line.ecc_overall)       meta.flags |= kEccOverallFlag;
  if (line.ecc_valid)         meta.flags |= kEccValidFlag;
  if (line.ecc_uncorrectable) meta.flags |= kEccUncorrectableFlag;
  return meta;
}
```

**为什么物理坐标不序列化？** 头文件的注释解释了："BackendLine 只保存持久状态。物理坐标可由地址和 storage_key 确定，由 MemoryImage 重建，不在每个数据块中重复保存。"PhysicalAddress 包含 ~40 个字段，其中大部分（tile_x, thermal_x, cell_x 等）是在 `MemoryImage::physical_address()` 中从 base address + spec + options 推导出来的。冗余存储不仅浪费空间（每行 ~200B），而且如果改变了 `StorageModelOptions`（如 mats_per_subarray_x），磁盘上的旧坐标就过时了，必须重建。

### 7.2 decode_meta（DiskLineMeta → BackendLine）

```cpp
void decode_meta(const DiskLineMeta& meta, BackendLine& line) {
  line.initialized = (meta.flags & kInitializedFlag) != 0;
  line.version = meta.version;
  line.last_writer_request_id = meta.last_writer_request_id;
  line.last_write_cycle = meta.last_write_cycle;
  line.last_access_cycle = meta.last_access_cycle;
  line.checksum = meta.checksum;
  line.ecc_hamming = meta.ecc_hamming;
  line.ecc_overall = (meta.flags & kEccOverallFlag) != 0;
  line.ecc_parity_bits = meta.ecc_parity_bits;
  line.ecc_valid = (meta.flags & kEccValidFlag) != 0;
  line.ecc_uncorrectable =
      (meta.flags & kEccUncorrectableFlag) != 0;
  line.ecc_error_injections = meta.ecc_error_injections;
  // 从定长数组重建 StorageKey
  line.storage_key = StorageKey{
      meta.storage[0], meta.storage[1],
      meta.storage[2], meta.storage[3],
      meta.storage[4], meta.storage[5],
      meta.storage[6], meta.storage[7],
  };
}
```

`encode_meta` 的精确逆操作。注意 `bytes` 和 `initialized_mask` 不在这里设置——它们由后端的 `load()` 方法从数据文件和 init 文件分别读取。

### 7.3 validate_line

```cpp
void validate_line(Address base, const BackendLine& line,
                   std::size_t line_size,
                   std::uint64_t capacity_bytes) {
  if (base % static_cast<Address>(line_size) != 0) {
    throw std::invalid_argument(
        "memory backend address is not line aligned");
  }
  if (line.bytes.size() != line_size ||
      line.initialized_mask.size() != line_size) {
    throw std::invalid_argument(
        "memory backend line has incorrect payload "
        "or init-mask size");
  }
  if (capacity_bytes != 0 &&
      (base >= capacity_bytes ||
       capacity_bytes - base < line_size)) {
    throw std::out_of_range(
        "memory backend address exceeds configured capacity");
  }
}
```

三层校验：
1. **地址对齐**：`base % line_size == 0`。如果 `line_size=64`，地址必须是 64 的倍数。
2. **payload 大小**：`bytes` 和 `initialized_mask` 必须恰好 `line_size` 字节。
3. **容量边界**：整个 cache line 必须在 `[0, capacity_bytes)` 范围内。

这是防御性编程——在 `store()` 时立即捕获错误，而非让错误数据写入文件后才发现。

---

## 八、SparseMemoryBackend — 纯内存后端（第 344-412 行）

```cpp
class SparseMemoryBackend final : public MemoryBackend {
  std::size_t line_size_;
  std::uint64_t capacity_bytes_;
  std::unordered_map<Address, BackendLine> lines_;
  std::unordered_map<StorageKey, Address, StorageKeyHash> reverse_;
  std::uint64_t unique_written_lines_ = 0;
};
```

### 8.1 数据结构

**`lines_`**：主存储。`unordered_map<Address, BackendLine>`，key 为 line-aligned base address，value 为完整的 BackendLine（含 payload bytes + 元数据）。

**`reverse_`**：反查索引。`unordered_map<StorageKey, Address>`。`StorageKey` 的 hash（`StorageKeyHash`）将 8 个 int 字段混合为一个 hash 值。这个索引让 `address_for_storage_key()` 实现 O(1) 反查。

**为什么需要两个索引？** `lines_` 回答"地址 X 上有数据吗？"，适合 load/store 的 O(1) 路径。`reverse_` 回答"哪个地址对应通道 3 的 bank 7 行 1024 列 5？"，适合 dump_text 和 golden 验证的 storage_key → address 映射。

### 8.2 load

```cpp
bool load(Address base, BackendLine& line) const override {
  auto it = lines_.find(base);
  if (it == lines_.end()) return false;
  line = it->second;
  return true;
}
```

O(1) 查找。未找到返回 false（上层 MemoryImage 会 `make_line(base, decoded)` 创建新行）。

### 8.3 store

```cpp
bool store(Address base, const BackendLine& line) override {
  validate_line(base, line, line_size_, capacity_bytes_);
  auto it = lines_.find(base);
  const bool inserted = it == lines_.end();
  const bool was_written = !inserted && it->second.version > 0;
  const bool is_written = line.version > 0;
  if (!inserted) {
    reverse_.erase(it->second.storage_key);  // 删除旧 key 映射
    it->second = line;                        // 覆盖
  } else {
    lines_.emplace(base, line);              // 插入新行
  }
  reverse_[line.storage_key] = base;         // 建立新 key 映射
  if (!was_written && is_written)
      unique_written_lines_++;               // 首次写入
  if (was_written && !is_written)
      unique_written_lines_--;               // version 归零（文件重新打开）
  return inserted;                           // true = 新分配
}
```

**`version > 0` 作为"已写入"标志**：version 为 0 表示此行被分配（load/store 创建了条目）但从未被真实数据写入。`unique_written_lines_` 只统计 version > 0 的行。这区分了：
- "内存空间已分配"（lines_ 中存在）
- "实际数据已写入"（version > 0）
- 两层独立追踪。`storage_density_pct` 统计指标使用的是后者。

**为什么先 `reverse_.erase` 再赋值？** `storage_key` 可能在写入时改变（尽管罕见——同一地址被重映射到不同 bank）。如果不先删除旧映射，`reverse_` 中会残留一条过时的反向索引，导致 `address_for_storage_key` 返回错误地址。

### 8.4 address_for_storage_key

```cpp
std::optional<Address> address_for_storage_key(
    const StorageKey& key) const override {
  auto it = reverse_.find(key);
  if (it == reverse_.end()) return std::nullopt;
  return it->second;
}
```

O(1) 直接反查。`reverse_` 由 store 维护。

### 8.5 all_addresses

```cpp
std::vector<Address> all_addresses() const override {
  std::vector<Address> addresses;
  addresses.reserve(lines_.size());
  for (const auto& [base, line] : lines_) {
    (void)line;
    addresses.push_back(base);
  }
  std::sort(addresses.begin(), addresses.end());
  return addresses;
}
```

遍历所有已分配地址，排序后返回。排序是为了 `dump_text` 和 `dump_csv` 的输出顺序稳定，方便 diff。

### 8.6 clear / flush

```cpp
void clear() override {
  lines_.clear();
  reverse_.clear();
  unique_written_lines_ = 0;
}
void flush() override {}  // 纯内存，无需持久化
```

---

## 九、MappedFile — mmap RAII 封装（第 414-472 行）

```cpp
class MappedFile {
  FileDescriptor file_;
  std::uint64_t size_;
  void* mapping_ = nullptr;
};
```

### 9.1 设计

MappedFile 封装了 `mmap` → `munmap` 的完整生命周期。与 FileDescriptor 不同，它持有（而非可选地使用）一个 mmap 区域。代码结构比 FileDescriptor 简单，因为 mmap 区域没有"移动"的复杂性——直接禁止复制和移动。

### 9.2 map / unmap

```cpp
void map() {
  mapping_ = ::mmap(nullptr,
                    checked_size(size_, "mmap region"),
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    file_.get(),
                    0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr;
    throw std::runtime_error(
        "failed to mmap memory backend file "
        + file_.path() + ": " + std::strerror(errno));
  }
}

void unmap() {
  if (mapping_ != nullptr) {
    ::munmap(mapping_, checked_size(size_, "mmap region"));
    mapping_ = nullptr;
  }
}
```

**`MAP_SHARED` 而非 `MAP_PRIVATE`**：这是关键选择。`MAP_SHARED` 意味着内存写入会被操作系统异步刷回磁盘文件。`MAP_PRIVATE` 做 copy-on-write——写入只影响进程内存，不会持久化。用 `MAP_SHARED`，store() 直接 `memcpy` 到 mmap 区域后，OS 负责最终写回磁盘。

`mapping_ = nullptr` 是 sentinel：`munmap(nullptr)` 是 UB（未定义行为），`nullptr` 哨兵确保只在成功 mmap 后才 unmap。

### 9.3 data()

```cpp
std::uint8_t* data() {
  return static_cast<std::uint8_t*>(mapping_);
}
```

返回映射区域的首地址。mmap_sparse 后端的 `load()` 直接从 `data() + index * line_size` 读取 payload，从 `init_.data() + index` 读取初始化位——操作系统 page cache 管理缓存，热数据自动保持在 RAM 中。

### 9.4 sync

```cpp
void sync() const {
  if (::msync(mapping_, checked_size(size_, "mmap region"),
              MS_SYNC) != 0) {
    throw ...
  }
  file_.sync();  // msync 后还需 fsync（元数据）
}
```

`msync(MS_SYNC)`：阻塞直到数据从页缓存刷到磁盘。`MS_ASYNC` 只发请求不等待；这里用同步版本保证 flush() 完成后数据确实在磁盘上。

**为什么 msync 后还要 fsync**：`msync` 刷的是文件数据页。但文件的 inode 元数据（文件大小、修改时间、extent map）可能仍在日志中。`fsync` 确保 inode 也持久化。两步缺一不可。

---

## 十、MmapSparseMemoryBackend（第 474-641 行）

### 10.1 构造函数

```cpp
MmapSparseMemoryBackend(
    std::size_t line_size,
    const MemoryBackendOptions& options)
    : line_size_(line_size),
      capacity_bytes_(options.capacity_bytes),
      line_count_(capacity_bytes_ / line_size_),
      init_bytes_(ceil_div(capacity_bytes_, 8)),
      presence_bytes_(ceil_div(line_count_, 8)),
      meta_bytes_(sizeof(DiskHeader) +
                  checked_mul(line_count_,
                              sizeof(DiskLineMeta), "metadata")),
      data_(require_data_path(options), capacity_bytes_),
      init_(sidecar_path(options.init_file,
                         options.data_file, ".init"),
            init_bytes_),
      meta_(sidecar_path(options.meta_file,
                         options.data_file, ".meta"),
            meta_bytes_),
      presence_(sidecar_path(options.presence_file,
                             options.data_file, ".present"),
                checked_mul(presence_bytes_, 2,
                            "presence maps")) {
  if (capacity_bytes_ == 0 ||
      capacity_bytes_ % line_size_ != 0) {
    throw std::invalid_argument(
        "mmap_sparse capacity must be a non-zero "
        "multiple of line_size");
  }
  initialize_or_validate_header();
}
```

**四个 MappedFile 对象**：
- `data_`：主数据文件 = `capacity_bytes` 字节。mmap 到内存，直接读写。
- `init_`：初始化位图 = `capacity_bytes / 8` 字节。每 bit 对应数据文件中 1 字节是否初始化。
- `meta_`：元数据文件 = `sizeof(DiskHeader) + line_count * sizeof(DiskLineMeta)` 字节。
- `presence_`：存在性+写入性位图 = `2 * line_count / 8` 字节。前半是 presence bitmap（行是否分配），后半是 written bitmap（行是否 version>0）。

**为什么 init_ 和 presence_ 是两个独立文件？** init_ 追踪**每字节**级别的初始化状态，presence_ 追踪**每行**级别的分配状态。粒度不同：一行 64 字节可能部分初始化（通过 masked write 或后续写入）。在 store 中两者同时更新。设计上可以合并，但分离的好处是每个文件独立管理（各自 mmap/msync），损坏一个不影响其他。

### 10.2 load

```cpp
bool load(Address base, BackendLine& line) const override {
  const std::uint64_t index = checked_index(base);
  if (!bit_is_set(presence_.data(), index)) return false;
  const std::uint64_t byte_offset = index * line_size_;
  // 从 mmap 区域直接读 payload（零拷贝！）
  line.bytes.assign(data_.data() + byte_offset,
                    data_.data() + byte_offset + line_size_);
  // 从 init bit map 重建 initialized_mask
  line.initialized_mask.assign(line_size_, 0);
  for (std::size_t i = 0; i < line_size_; i++) {
    if (bit_is_set(init_.data(), byte_offset + i)) {
      line.initialized_mask[i] = 0xff;
    }
  }
  // 从 meta 区域读元数据
  decode_meta(meta_records()[index], line);
  return true;
}
```

**零拷贝路径**：`data_.data()` 返回 mmap 映射的虚拟地址。`line.bytes.assign(data_ + offset, data_ + offset + 64)` 是从内核 page cache 到用户空间的 `std::vector` 构造——只有一次 `memcpy`（如果页已在缓存中，无磁盘 I/O）。没有 `pread` 系统调用，没有内核-用户态切换。

**`meta_records()`** 辅助函数：
```cpp
const DiskLineMeta* meta_records() const {
  return reinterpret_cast<const DiskLineMeta*>(
      meta_.data() + sizeof(DiskHeader));
}
```
跳过 DiskHeader，返回第一个 DiskLineMeta 的位置。`meta_records()[index]` 就是第 `index` 行的元数据——O(1) 直接索引，无需遍历。

### 10.3 store（类似 load 的逆操作）

```cpp
bool store(Address base, const BackendLine& line) override {
  validate_line(...);
  const std::uint64_t index = checked_index(base);
  const bool inserted =
      !bit_is_set(presence_.data(), index);
  const std::uint64_t byte_offset = index * line_size_;
  // 写入 mmap 区域（OS 异步回写磁盘）
  std::copy(line.bytes.begin(), line.bytes.end(),
            data_.data() + byte_offset);
  for (std::size_t i = 0; i < line_size_; i++) {
    set_bit(init_.data(), byte_offset + i,
            line.initialized_mask[i] != 0);
  }
  meta_records()[index] = encode_meta(line);
  if (inserted) {
    set_bit(presence_.data(), index, true);
    allocated_lines_++;
  }
  // 追踪 unique_written_lines_
  const bool was_written =
      bit_is_set(written_bits(), index);
  if (line.version > 0 && !was_written) {
    set_bit(written_bits(), index, true);
    unique_written_lines_++;
  } else if (line.version == 0 && was_written) {
    set_bit(written_bits(), index, false);
    unique_written_lines_--;
  }
  return inserted;
}
```

**written_bits() 辅助函数**：
```cpp
std::uint8_t* written_bits() {
  return presence_.data() + presence_bytes_;
}
```
`presence_` 文件 = `[presence_bitmap][written_bitmap]`。前半 `presence_bytes_` 是存在位图，后半 `presence_bytes_` 是写入位图。两块连续存储在同一文件中。

### 10.4 clear

```cpp
void clear() override {
  data_.reset_sparse();      // truncate(0) + truncate(capacity)
  init_.reset_sparse();      // 同上，清空位图
  meta_.reset_sparse();      // 同上
  presence_.reset_sparse();  // 同上
  const DiskHeader header =
      make_header(line_size_, capacity_bytes_);
  std::memcpy(meta_.data(), &header, sizeof(header));
  allocated_lines_ = 0;
  unique_written_lines_ = 0;
}
```

`reset_sparse()` 通过 truncate(0) → truncate(size) 重置每个文件。重置后重写 DiskHeader 保持文件格式完整性。

### 10.5 flush

```cpp
void flush() override {
  DiskHeader* header =
      reinterpret_cast<DiskHeader*>(meta_.data());
  header->reserved[0] = allocated_lines_;
  header->reserved[1] = unique_written_lines_;
  data_.sync();
  init_.sync();
  meta_.sync();
  presence_.sync();
}
```

将计数器写回 DiskHeader 的 `reserved[]` 字段，然后 `msync + fsync` 全部四个文件。`flush()` 通常在仿真结束、dump 内存镜像之前调用，确保磁盘文件与内存状态一致。

### 10.6 address_for_storage_key

```cpp
std::optional<Address> address_for_storage_key(
    const StorageKey& key) const override {
  const auto* records = meta_records();
  std::optional<Address> result;
  visit_set_bits(presence_.data(), line_count_,
      [&](std::uint64_t index) {
    BackendLine line;
    decode_meta(records[index], line);
    if (line.storage_key == key) {
      result = index * line_size_;
      return false;  // 找到即停
    }
    return true;  // 未找到继续
  });
  return result;
}
```

用 `visit_set_bits` 稀疏遍历已有行，逐行检查 `storage_key` 匹配。找到后通过返回 `false` 提前退出。这是 O(已分配行数) 的操作——在稀疏数据集中高效，但在密集数据集中可能很慢。对于后者的优化方案是维护一个类似于 `SparseMemoryBackend::reverse_` 的 hash 表，但 mmap_sparse 的目标是**内存效率**——`reverse_` 每行额外占用 ~40 字节（hash node + key + value），对于 1G 行的数据集不可接受。

---

## 十一、ChunkFileMemoryBackend（第 643-887 行）

### 11.1 设计动机

MmapSparse 的优势是零拷贝，但缺点也明显：
- `mmap` 64GB 的地址空间——虽然虚拟地址够用（x86-64 有 48-bit = 256TB 用户空间），但 mmap 区域的页表条目占用内核内存。
- 操作系统 page cache 管理所有已访问页——对 TB 级数据集，cache 驱逐策略不够精细。

ChunkFile 将地址空间切分成固定大小的 chunk（默认 2MB），只在内存中保留有限数量的 chunk（默认 16 个 = 32MB 数据 + 元数据）。用普通 `pread/pwrite` 做磁盘 I/O，LRU 策略选择要缓存的 chunk。

### 11.2 构造函数与成员

```cpp
ChunkFileMemoryBackend(
    std::size_t line_size,
    const MemoryBackendOptions& options)
    : line_size_(line_size),
      capacity_bytes_(options.capacity_bytes),
      line_count_(capacity_bytes_ / line_size_),
      chunk_size_(options.chunk_size_bytes),       // 默认 2MB
      lines_per_chunk_(chunk_size_ / line_size_),  // 2MB/64B = 32768 行
      cache_limit_(std::max<std::size_t>(1,
          options.chunk_cache_entries)),           // 默认 16
      presence_bytes_(ceil_div(line_count_, 8)),
      data_(require_data_path(options),
            capacity_bytes_),          // FileDescriptor (非 mmap)
      init_(sidecar_path(...), ...),   // FileDescriptor
      meta_(sidecar_path(...), ...),   // FileDescriptor
      presence_(sidecar_path(...), ...) // MappedFile (位图保留 mmap)
{
  if (capacity_bytes_ % line_size_ != 0) { throw... }
  if (chunk_size_ % line_size_ != 0 ||
      chunk_size_ % 8 != 0) { throw... }
  initialize_or_validate_header();
}
```

**关键差异**：
- `data_`、`init_`、`meta_` 是 `FileDescriptor`（用 pread/pwrite），不是 `MappedFile`。
- `presence_` 仍然是 `MappedFile`——位图文件很小（1G 行/8 = 128MB，两块共 256MB），且随机访问模式密集（每个 load/store 都查），mmap 是更合理的选择。
- `cache_`：`unordered_map<uint64_t, CachedChunk>` — chunk 索引 → 缓存块。

### 11.3 CachedChunk

```cpp
struct CachedChunk {
  ByteVector data;                    // chunk_size_ 的 payload
  ByteVector init;                    // 对应的 init bitmap
  std::vector<DiskLineMeta> meta;     // lines_per_chunk_ 条元数据
  std::uint64_t last_use = 0;        // LRU 时钟值
  bool dirty = false;                // 有未回写的数据？
};
```

**`dirty` 标记**：chunk 从磁盘读入缓存后为 clean。store() 写入后标记 dirty=true。flush() 或 LRU 驱逐时只回写 dirty chunk，避免无谓的 pwrite 调用。

### 11.4 load

```cpp
bool load(Address base, BackendLine& line) const override {
  const std::uint64_t index = checked_index(base);
  if (!bit_is_set(presence_.data(), index)) return false;
  CachedChunk& chunk =
      ensure_chunk(index / lines_per_chunk_);
  const std::size_t local_line =
      static_cast<std::size_t>(index % lines_per_chunk_);
  const std::size_t byte_offset = local_line * line_size_;
  line.bytes.assign(
      chunk.data.begin() + byte_offset,
      chunk.data.begin() + byte_offset + line_size_);
  line.initialized_mask.assign(line_size_, 0);
  for (std::size_t i = 0; i < line_size_; i++) {
    if (bit_is_set(chunk.init.data(), byte_offset + i)) {
      line.initialized_mask[i] = 0xff;
    }
  }
  decode_meta(chunk.meta[local_line], line);
  return true;
}
```

与 MmapSparse 结构相同，但数据来源从 `mmap 区域` 替换为 `chunk 内存缓存`。

### 11.5 ensure_chunk — LRU 缓存核心

```cpp
CachedChunk& ensure_chunk(std::uint64_t chunk_index) const {
  auto it = cache_.find(chunk_index);
  if (it != cache_.end()) {
    it->second.last_use = ++use_clock_;  // 更新 LRU 时钟
    return it->second;
  }
  // 缓存未命中
  if (cache_.size() >= cache_limit_) {
    // 找最后使用的 victim
    auto victim = std::min_element(
        cache_.begin(), cache_.end(),
        [](const auto& lhs, const auto& rhs) {
          return lhs.second.last_use < rhs.second.last_use;
        });
    write_chunk(victim->first, victim->second);  // dirty → 回写
    cache_.erase(victim);
  }
  // 从磁盘加载新 chunk
  const std::size_t chunk_lines =
      lines_in_chunk(chunk_index);
  const std::size_t chunk_bytes = chunk_lines * line_size_;
  CachedChunk chunk;
  chunk.data.resize(chunk_bytes, 0);
  chunk.init.resize(
      checked_size(ceil_div(chunk_bytes, 8), "..."), 0);
  chunk.meta.resize(chunk_lines);
  chunk.last_use = ++use_clock_;
  // 三条 pread：data + init + meta
  const std::uint64_t first_line =
      chunk_index * lines_per_chunk_;
  const std::uint64_t byte_offset = first_line * line_size_;
  pread_fill(data_.get(), chunk.data.data(),
             chunk.data.size(), byte_offset);
  pread_fill(init_.get(), chunk.init.data(),
             chunk.init.size(), byte_offset / 8);
  pread_fill(meta_.get(), chunk.meta.data(),
             chunk.meta.size() * sizeof(DiskLineMeta),
             sizeof(DiskHeader)
                 + first_line * sizeof(DiskLineMeta));
  return cache_.emplace(chunk_index, std::move(chunk))
      .first->second;
}
```

**LRU 算法细节**：
- `use_clock_`：全局递增计数器（每次 `ensure_chunk` 调用时 `++use_clock_`）。不是真实时钟——只是单调递增序列号。
- `last_use`：该 chunk 最后一次被访问时的 `use_clock_` 值。
- 驱逐选择：`std::min_element` 找 `last_use` 最小的 chunk = 最久未访问的。这不叫"严格 LRU"因为 `last_use` 只在 `ensure_chunk` 时更新，不区分读和写。但对仿真场景（访问模式通常有时间局部性）足够有效。
- 时间复杂度：驱逐 O(cache_limit) = O(16) = 常数。

**三条 pread 的 offset 计算**：
- `data`：`first_line * line_size`（第 chunk 起始位置在数据文件中的字节偏移）
- `init`：`(first_line * line_size) / 8`（bitmap 文件中该区域的起始字节——每 8 个数据字节 1 个 bitmap 字节）
- `meta`：`sizeof(DiskHeader) + first_line * sizeof(DiskLineMeta)`（跳过 header 后，第 first_line 条元数据的起始偏移）

### 11.6 write_chunk

```cpp
void write_chunk(std::uint64_t chunk_index,
                 CachedChunk& chunk) const {
  if (!chunk.dirty) return;  // 洁净 chunk 无需回写
  const std::uint64_t first_line =
      chunk_index * lines_per_chunk_;
  const std::uint64_t byte_offset = first_line * line_size_;
  pwrite_all(data_.get(), chunk.data.data(),
             chunk.data.size(), byte_offset);
  pwrite_all(init_.get(), chunk.init.data(),
             chunk.init.size(), byte_offset / 8);
  pwrite_all(meta_.get(), chunk.meta.data(),
             chunk.meta.size() * sizeof(DiskLineMeta),
             sizeof(DiskHeader)
                 + first_line * sizeof(DiskLineMeta));
  chunk.dirty = false;
}
```

`write_chunk` 将单个 chunk 的 data/init/meta 三条数据写回磁盘。三条 `pwrite` 的 offset 计算与 `ensure_chunk` 中三条 `pread` 完全对称。

**为什么 dirty 检查放在开头？** 大多数 chunk 只被读取（仿真中读取通常多于写入）。无 dirty 检测时，flush 会把 16 个 chunk 全部回写——其中 14 个是干净的，浪费 42 次 pwrite 系统调用。

### 11.7 flush

```cpp
void flush() override {
  for (auto& [index, chunk] : cache_) {
    write_chunk(index, chunk);       // 回写全部 dirty chunk
  }
  DiskHeader header;
  pread_fill(meta_.get(), &header,
             sizeof(header), 0);     // 读取当前 header
  header.reserved[0] = allocated_lines_;
  header.reserved[1] = unique_written_lines_;
  pwrite_all(meta_.get(), &header,
             sizeof(header), 0);     // 写回更新后的 header
  data_.sync();                      // fsync 所有文件
  init_.sync();
  meta_.sync();
  presence_.sync();
}
```

先回写所有 dirty chunk，再更新 header 中的计数器，最后 fsync 全部文件。**先数据后元数据**的顺序很重要：如果中间崩溃，元数据更新的 allocated_lines 可能略旧于实际数据（下次打开时计数器偏小），但数据本身是完整的且 header 的 reserved 可以修正。

### 11.8 析构函数

```cpp
~ChunkFileMemoryBackend() override {
  try { flush(); } catch (...) {}
}
```

如果用户忘记调用 `flush()`，析构函数自动回写数据。`try/catch(...)` 吃掉了异常——析构函数不能抛出异常（会导致 `std::terminate` 如果已经在异常展开中）。丢失最后的数据是最坏情况，但好过崩溃。

---

## 十二、工厂函数（第 891-943 行）

### 12.1 to_string / parse

```cpp
std::string to_string(MemoryBackendKind kind) {
  switch (kind) {
    case MemoryBackendKind::Sparse: return "sparse";
    case MemoryBackendKind::MmapSparse: return "mmap_sparse";
    case MemoryBackendKind::ChunkFile: return "chunk_file";
  }
  return "sparse";
}

MemoryBackendKind parse_memory_backend_kind(
    const std::string& value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(),
                 normalized.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  if (normalized == "sparse") return MemoryBackendKind::Sparse;
  if (normalized == "mmap_sparse" || normalized == "mmap")
      return MemoryBackendKind::MmapSparse;
  if (normalized == "chunk_file" || normalized == "chunk")
      return MemoryBackendKind::ChunkFile;
  throw std::invalid_argument(
      "unsupported memory backend: " + value);
}
```

`parse` 支持缩写别名（`"mmap"` → `MmapSparse`，`"chunk"` → `ChunkFile`），`tolower` 使大小写不敏感。CLI 和配置文件中可用 `memory_backend = mmap` 启动 mmap_sparse 后端。

### 12.2 make_memory_backend

```cpp
std::unique_ptr<MemoryBackend> make_memory_backend(
    std::size_t line_size,
    const MemoryBackendOptions& options) {
  if (line_size == 0) {
    throw std::invalid_argument(
        "memory backend line_size must be > 0");
  }
  if (options.kind != MemoryBackendKind::Sparse) {
    if (options.capacity_bytes == 0 ||
        options.capacity_bytes % line_size != 0) {
      throw std::invalid_argument(
          "file-backed memory capacity must be a "
          "non-zero multiple of line_size");
    }
    if (options.data_file.empty()) {
      throw std::invalid_argument(
          "file-backed memory backend requires "
          "memory_data_file");
    }
  }
  if (options.kind == MemoryBackendKind::ChunkFile &&
      (options.chunk_size_bytes < line_size ||
       options.chunk_size_bytes % line_size != 0 ||
       options.chunk_size_bytes % 8 != 0)) {
    throw std::invalid_argument(
        "memory_chunk_size must be >= line_size and "
        "a multiple of line_size and 8");
  }
  switch (options.kind) {
    case MemoryBackendKind::Sparse:
      return std::make_unique<SparseMemoryBackend>(
          line_size, options.capacity_bytes);
    case MemoryBackendKind::MmapSparse:
      return std::make_unique<MmapSparseMemoryBackend>(
          line_size, options);
    case MemoryBackendKind::ChunkFile:
      return std::make_unique<ChunkFileMemoryBackend>(
          line_size, options);
  }
  throw std::invalid_argument("unsupported memory backend");
}
```

**入参校验层次**：
1. 通用校验：`line_size > 0`
2. File-backed 校验：`capacity` 非零且整除 `line_size`，`data_file` 非空
3. Chunk-specific 校验：chunk_size >= line_size，且整除 line_size 和 8

校验在工厂函数中集中进行，而非分散到各 Backend 构造函数。这样所有后端的构造都安全——进入 Backend 构造函数时参数已合法。

返回 `unique_ptr<MemoryBackend>`——调用方只知道接口，不依赖具体实现。替换后端只需要改一行配置，不影响 `MemoryImage` 的逻辑。

---

## 总结

`memory_backend.cpp` 实现了三个存储后端的完整功能：

| 后端 | 行数 | 数据结构 | 数据访问方式 | 适用场景 |
|------|------|---------|-------------|---------|
| Sparse | ~70 | `unordered_map` × 2 | map.find O(1) | 小规模调试/验证 |
| MmapSparse | ~230 | mmap × 4 文件 | 零拷贝直接内存访问 | 中大工作集/场景恢复 |
| ChunkFile | ~240 | FileDescriptor × 3 + MappedFile × 1 + LRU cache | pread/pwrite + 16-chunk LRU 缓存 | 大规模/满容量写入 |

+ 文件格式层（DiskHeader + DiskLineMeta）、RAII 工具层（FileDescriptor + MappedFile）、位图操作层和工厂函数，总计 ~400 行支撑代码。

设计原则：
- **自描述文件格式**：magic + version + record_size + line_size + capacity，打开时校验兼容性
- **稀疏文件**：mmap_sparse 和 chunk_file 的数据文件是稀疏的——逻辑容量大但不占磁盘空间
- **RAII 管理 OS 资源**：FileDescriptor + MappedFile 封装 POSIX fd 和 mmap 生命周期
- **位图追踪两层状态**：presence（分配）和 written（写入）分离
- **工厂模式**：`make_memory_backend` 唯一创建点，三种后端对调用方透明
