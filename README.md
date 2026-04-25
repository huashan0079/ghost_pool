# GhostPool-v1.0 - 高性能幽灵指针池

## 概述

GhostPool 是一个基于代际管理的环形对象池，实现了类似 C++ `weak_ptr` 的安全访问机制，但避免了传统 `shared_ptr`/`weak_ptr` 的引用计数开销。它适用于需要频繁创建/销毁对象、对性能敏感且对内存布局有控制要求的场景。

## 核心特性

- **环形数组存储**：对象在连续内存中分配，缓存友好
- **代际管理**：每个槽位使用代际计数器处理 ABA 问题
- **Pin 机制**：访问时临时固定对象阻止销毁，类似 `weak_ptr::lock()`
- **无额外堆分配**：对象直接在池中构造，删除器仅标记释放
- **线程安全**：使用原子操作，无锁设计

## 工作原理

```
┌─────────────────────────────────────────┐
│  Pool (环形数组)        元数据数组        │
├─────────────────────────────────────────┤
│ [0] T object    → generation[0], pin[0] │
│ [1] T object    → generation[1], pin[1] │
│ [2] (空闲)       → generation[2], pin[2] │
│ ...                                      │
└─────────────────────────────────────────┘

shared_ghost_ptr → 持有 shared_ptr<T> + 索引 + 代际
weak_ghost_ptr   → 仅持索引 + 期望代际
```

## 使用方法

### 基本示例

```cpp
#include "GhostPool.h"

struct MyObject {
    int id;
    std::string name;
};

int main() {
    // 创建容量为 100 的对象池
    GhostPool<MyObject, 100> pool;

    // 分配对象
    auto ghost = pool.allocate(42, "example");
    
    // 像普通 shared_ptr 一样使用
    std::cout << ghost->id << ", " << ghost->name << std::endl;
    
    // 隐式转换为 shared_ptr（可用于函数参数）
    std::shared_ptr<MyObject> sp = ghost;
    
    // 创建弱引用
    auto weak = pool.observe(ghost);
    
    // 安全访问弱引用
    weak.access([](MyObject* ptr) {
        if (ptr) {
            std::cout << "Access: " << ptr->id << std::endl;
        }
    });
    
    return 0;  // ghost 析构，对象归还池
}
```

### 安全访问弱引用

```cpp
auto ghost = pool.allocate();
auto weak = pool.observe(ghost);

// ghost 可能已被销毁，使用 access 安全访问
bool accessed = weak.access([&](MyObject* ptr) {
    // 此 lambda 仅在对象存活时执行
    ptr->do_something();
});

if (!accessed) {
    std::cout << "对象已销毁" << std::endl;
}

// 检查是否过期
if (weak.expired()) {
    std::cout << "弱引用已失效" << std::endl;
}
```

### 批量分配

```cpp
std::vector<shared_ghost_ptr<MyObject, 100>> objects;

for (int i = 0; i < 50; ++i) {
    objects.push_back(pool.allocate(i, "batch"));
}
```

## API 参考

### `class GhostPool<T, N>`

| 方法 | 描述 |
|------|------|
| `allocate(Args&&...)` | 分配新对象，返回 `shared_ghost_ptr` |
| `observe(sp)` | 从强引用创建弱引用 |
| `try_pin(idx, gen)` | 尝试固定对象，返回是否成功 |
| `unpin(idx)` | 解除对象固定 |
| `is_alive(idx, gen)` | 检查对象是否存活 |
| `get_ptr(idx)` | 获取原始指针（需确保存活） |

### `class shared_ghost_ptr<T, N>`

| 方法 | 描述 |
|------|------|
| `operator->()` | 透明访问成员 |
| `operator*()` | 解引用 |
| `operator bool()` | 检查是否有效 |
| `operator std::shared_ptr<T>()` | 隐式转换为 `shared_ptr` |

### `class weak_ghost_ptr<T, N>`

| 方法 | 描述 |
|------|------|
| `access(Func&&)` | 安全访问，若对象存活则调用回调 |
| `expired()` | 检查弱引用是否失效 |

## 性能特性

| 操作 | 复杂度 | 原子操作 |
|------|--------|----------|
| 分配 | O(1) 平均，最坏 O(N) | 1-2 次 |
| 析构 | O(1) 自旋等待 | 1 次 + 自旋 |
| 弱引用访问 | O(1) | 3-4 次 |
| 检查过期 | O(1) | 1 次 |

## ⚠️ 重要：弱引用访问的性能注意事项

### 回调函数应快速完成

`weak_ghost_ptr::access()` 在调用回调前会通过 `try_pin()` **固定（pin）** 对象，防止其在访问期间被销毁。固定期间，若其他线程尝试销毁该对象，将会**自旋等待** `pin_count` 归零。

```cpp
// ❌ 错误示例：在 access 中执行耗时操作
weak.access([&](MyObject* ptr) {
    ptr->process_large_data();      // 可能耗时数秒
    network_request(ptr->get_id()); // 可能阻塞
    heavy_computation(ptr);         // 长时间持有 pin
});
// 在此期间，析构该对象的线程将一直自旋等待

// ✅ 正确示例：快速提取数据，延迟处理
weak.access([&](MyObject* ptr) {
    // 只做快速操作：复制数据、提取 ID 等
    int id = ptr->id;
    std::string data = ptr->data;  // 假设 data 是小字符串
    // lambda 退出后自动 unpin
});
// 耗时的处理在这里进行
async_process(id, std::move(data));
```

### 核心原则

| 原则 | 说明 |
|------|------|
| **快速执行** | 回调内只做 O(1) 或 O(log N) 的快速操作 |
| **避免阻塞** | 不要在回调内进行 I/O、锁等待、网络请求 |
| **避免长时间循环** | 大数据处理应复制必要数据后移出回调 |
| **注意嵌套访问** | 避免在回调内再次调用 `access()` 或其他 pin 操作 |

### 自旋等待的影响

当对象正在被 `access()` 回调持有时，若 `shared_ghost_ptr` 析构（即要释放对象），析构线程会：

```cpp
// 析构时的自旋等待逻辑
while (pin_count_[idx].load() != 0) {
    _mm_pause();  // 空转等待
}
```

这意味着：
- **回调执行越久，析构线程自旋越久**
- **长时间自旋浪费 CPU 资源**
- **极端情况下可能导致线程饥饿**

### 推荐模式

```cpp
// 模式 1：提取小数据
weak.access([&](MyObject* ptr) {
    cached_id = ptr->id;
    cached_flag = ptr->flag;
});
// 根据提取的数据做后续处理

// 模式 2：使用移动语义提取大对象
struct BigData { /* 大量数据 */ };
std::optional<BigData> extracted;

weak.access([&](MyObject* ptr) {
    extracted.emplace(std::move(ptr->big_data));
    ptr->big_data.clear();  // 清空原数据
});

if (extracted) {
    process_big_data(std::move(*extracted));
}

// 模式 3：获取 shared_ptr 后直接使用（推荐替代长时间 access）
if (auto sp = weak_ghost.lock()) {  // 如果提供 lock 方法
    // 这里可以安全地长时间操作
    sp->do_heavy_work();
}
```

### 性能对比

| 回调时长 | Pin 持有时间 | 对析构线程影响 | 建议 |
|---------|-------------|---------------|------|
| < 1μs | 极短 | 几乎无影响 | ✅ 推荐 |
| 1-100μs | 短 | 轻微自旋 | ⚠️ 可接受 |
| > 100μs | 较长 | 明显 CPU 浪费 | ❌ 应避免 |
| > 1ms | 长 | 严重自旋 | ❌ 强烈不建议 |

## 线程安全说明

- **分配**：多线程安全，使用 `fetch_add` 环形分配
- **强引用访问**：`shared_ghost_ptr` 的使用遵循 `shared_ptr` 规则
- **弱引用访问**：`access()` 线程安全，多线程可同时访问不同对象
- **Pin 机制**：防止对象在访问期间被销毁
- **⚠️ 回调时长**：回调执行期间持有 pin，会阻塞其他线程的析构操作

## 注意事项

⚠️ **池容量限制**
- 固定容量，满时 `allocate()` 抛出 `runtime_error`
- 建议根据实际负载配置容量

⚠️ **自旋等待**
- 析构时会自旋等待 `pin_count` 归零
- **回调函数应快速完成**，避免长时间持有 pin

⚠️ **不支持移动语义**
- 对象在池中位置固定，移动不会改变索引

⚠️ **禁止递归访问**
- 在 `access()` 回调内再次对同一对象调用 `access()` 会导致死锁（自增 pin 计数后递归等待）

## 编译要求

- C++17 或更高版本
- 头文件独立，无外部依赖
- 需包含 `<immintrin.h>` 或 `<x86intrin.h>` 以支持 `_mm_pause()`（MSVC 下可省略）

## 常见问题

**Q: 与传统 `weak_ptr` 的区别？**

A: 传统 `weak_ptr` 使用共享引用计数（控制块），每次 `lock()` 需要原子操作复制 `shared_ptr`。GhostPool 通过 pin 计数和代际检查实现类似功能，无额外堆分配。

**Q: 何时使用？**

A: 适合对象生命周期管理频繁、对象大小相似、要求内存布局可控的场景（如游戏实体、网络会话、缓存对象）。

**Q: 为什么回调要快速执行？**

A: `access()` 在回调期间持有对象的 pin 计数，阻止析构。其他线程若要销毁该对象（`shared_ghost_ptr` 析构）会自旋等待 pin 归零。长时间回导会导致 CPU 空转。

**Q: 如何执行耗时操作？**

A: 在回调内快速提取必要数据（值语义），然后退出回调，在主线程或任务队列中处理提取的数据。

**Q: 线程安全保证？**

A: 分配是线程安全的。多个线程可以同时 `allocate` 和使用不同的强引用。弱引用的 `access()` 是线程安全的，只要操作的是不同的对象或正确通过 `access` 访问。
