#pragma once
#include "meton_native_memory_abi.h"
#include <atomic>
#include <cstdint>

/** 独立于 VM 的分配权威；仍存活的分配保有它，旧代关闭不能抹除未释放字节。 */
class MetonNativeMemory {
    std::atomic<uint64_t> references{1};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> peak{0};
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> allocations{0};
    const uint64_t limit;
public:
    struct State { uint64_t bytes; uint64_t peak; uint64_t blocks; uint64_t allocations; uint64_t limit; };
    explicit MetonNativeMemory(uint64_t p_limit) : limit(p_limit) {}
    /** 分配头与受信运行时分别保有引用，跨线程迟到释放不访问已经销毁的 VM。 */
    void retain();
    void release();
    /** 先扣额度再申请，不能在实际分配完成后才发现超限。 */
    void acquire(uint64_t p_bytes);
    void refund(uint64_t p_bytes);
    void attached();
    void detached();
    State state() const;
};

/** 原生入口的动态作用域；嵌套返回后恢复原所有者，不改变已有分配的所属。 */
class MetonNativeMemoryScope {
    MetonNativeMemory *previous;
    MetonNativeMemory *selected;
public:
    static MetonNativeMemory *current();
    static MetonNativeMemory *exchange(MetonNativeMemory *p_budget);
    explicit MetonNativeMemoryScope(MetonNativeMemory *p_budget);
    ~MetonNativeMemoryScope();
    MetonNativeMemoryScope(const MetonNativeMemoryScope &) = delete;
    MetonNativeMemoryScope &operator=(const MetonNativeMemoryScope &) = delete;
};
