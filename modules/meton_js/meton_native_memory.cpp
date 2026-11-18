#include "meton_native_memory.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#ifdef WEB_ENABLED
[[noreturn]] void meton_web_resource_exhausted();
#endif

namespace {
thread_local MetonNativeMemory *current_budget = nullptr;
struct alignas(std::max_align_t) Allocation {
    MetonNativeMemory *owner;
    size_t size;
};

/** 任意原生构造点不能保证从空指针恢复；专属进程以固定事实退出，不继续提交半初始化对象。 */
[[noreturn]] void exhausted() {
#ifdef WEB_ENABLED
    meton_web_resource_exhausted();
#else
    std::fputs("METON_NATIVE_RESOURCE_BUDGET_EXCEEDED\n", stderr);
    std::fflush(stderr);
    std::_Exit(197);
#endif
}

/** 分配头也计入额度；整数溢出不得折返为较小的收费。 */
bool total_size(size_t p_bytes, size_t &r_size, MetonNativeMemory *p_budget) {
    if (p_bytes > std::numeric_limits<size_t>::max() - sizeof(Allocation)) {
        if (p_budget) exhausted();
        return false;
    }
    r_size = p_bytes + sizeof(Allocation);
    return true;
}
}

void MetonNativeMemory::retain() { references.fetch_add(1, std::memory_order_relaxed); }
void MetonNativeMemory::release() {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
}
void MetonNativeMemory::acquire(uint64_t p_bytes) {
    uint64_t old = bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (old > limit || p_bytes > limit - old) exhausted();
        if (bytes.compare_exchange_weak(old, old + p_bytes, std::memory_order_acq_rel)) break;
    }
    uint64_t high = peak.load(std::memory_order_relaxed);
    while (high < old + p_bytes && !peak.compare_exchange_weak(high, old + p_bytes, std::memory_order_relaxed)) {}
}
void MetonNativeMemory::refund(uint64_t p_bytes) { bytes.fetch_sub(p_bytes, std::memory_order_acq_rel); }
void MetonNativeMemory::attached() {
    retain(); blocks.fetch_add(1, std::memory_order_relaxed); allocations.fetch_add(1, std::memory_order_relaxed);
}
void MetonNativeMemory::detached() { blocks.fetch_sub(1, std::memory_order_relaxed); release(); }
MetonNativeMemory::State MetonNativeMemory::state() const {
    return {bytes.load(), peak.load(), blocks.load(), allocations.load(), limit};
}
MetonNativeMemory *MetonNativeMemoryScope::current() { return current_budget; }
MetonNativeMemory *MetonNativeMemoryScope::exchange(MetonNativeMemory *p_budget) {
    auto *previous = current_budget;
    current_budget = p_budget;
    return previous;
}
MetonNativeMemoryScope::MetonNativeMemoryScope(MetonNativeMemory *p_budget) : previous(current_budget), selected(p_budget) {
    if (selected) selected->retain();
    current_budget = selected;
}
MetonNativeMemoryScope::~MetonNativeMemoryScope() {
    current_budget = previous;
    if (selected) selected->release();
}

extern "C" void *meton_resource_alloc(size_t p_bytes) {
    auto *budget = current_budget;
    size_t size;
    if (!total_size(p_bytes, size, budget)) return nullptr;
    if (budget) budget->acquire(size);
    auto *allocation = static_cast<Allocation *>(std::malloc(size));
    if (!allocation) { if (budget) budget->refund(size); return nullptr; }
    allocation->owner = budget; allocation->size = size;
    if (budget) budget->attached();
    return allocation + 1;
}
extern "C" void *meton_resource_calloc(size_t p_count, size_t p_bytes) {
    if (p_count && p_bytes > std::numeric_limits<size_t>::max() / p_count) {
        if (current_budget) exhausted();
        return nullptr;
    }
    const size_t size = p_count * p_bytes;
    void *memory = meton_resource_alloc(size);
    if (memory) std::memset(memory, 0, size);
    return memory;
}
extern "C" void meton_resource_free(void *p_memory) {
    if (!p_memory) return;
    auto *allocation = static_cast<Allocation *>(p_memory) - 1;
    auto *budget = allocation->owner;
    const size_t size = allocation->size;
    std::free(allocation);
    if (budget) { budget->refund(size); budget->detached(); }
}
extern "C" void *meton_resource_realloc(void *p_memory, size_t p_bytes) {
    if (!p_memory) return meton_resource_alloc(p_bytes);
    if (!p_bytes) { meton_resource_free(p_memory); return nullptr; }
    auto *allocation = static_cast<Allocation *>(p_memory) - 1;
    // 已归属的分配不能在另一作用域洗掉费用；首次扩展共享未计费块时保守承担整块。
    auto *original = allocation->owner;
    auto *budget = original ? original : current_budget;
    const size_t old_size = allocation->size;
    size_t size;
    if (!total_size(p_bytes, size, budget)) return nullptr;
    const size_t added = original ? (size > old_size ? size - old_size : 0) : size;
    if (budget) budget->acquire(added);
    auto *updated = static_cast<Allocation *>(std::realloc(allocation, size));
    if (!updated) { if (budget) budget->refund(added); return nullptr; }
    updated->owner = budget; updated->size = size;
    if (budget) {
        if (!original) budget->attached();
        else if (size < old_size) budget->refund(old_size - size);
    }
    return updated + 1;
}

extern "C" void *meton_resource_alloc_aligned(size_t p_bytes, size_t p_alignment) {
    if (!p_alignment || (p_alignment & (p_alignment - 1)) || p_alignment > std::numeric_limits<uint32_t>::max()) return nullptr;
    const size_t padding = p_alignment - 1 + sizeof(uint32_t);
    if (p_bytes > std::numeric_limits<size_t>::max() - padding) {
        if (current_budget) exhausted();
        return nullptr;
    }
    void *base = meton_resource_alloc(p_bytes + padding);
    if (!base) return nullptr;
    const uintptr_t address = (reinterpret_cast<uintptr_t>(base) + sizeof(uint32_t) + p_alignment - 1) & ~(p_alignment - 1);
    auto *memory = reinterpret_cast<uint8_t *>(address);
    *(reinterpret_cast<uint32_t *>(memory) - 1) = uint32_t(memory - static_cast<uint8_t *>(base));
    return memory;
}
extern "C" void meton_resource_free_aligned(void *p_memory) {
    if (!p_memory) return;
    const uint32_t offset = *(static_cast<uint32_t *>(p_memory) - 1);
    meton_resource_free(static_cast<uint8_t *>(p_memory) - offset);
}
extern "C" void *meton_resource_realloc_aligned(void *p_memory, size_t p_bytes, size_t p_previous_bytes, size_t p_alignment) {
    if (!p_memory) return meton_resource_alloc_aligned(p_bytes, p_alignment);
    if (!p_bytes) { meton_resource_free_aligned(p_memory); return nullptr; }
    const uint32_t offset = *(static_cast<uint32_t *>(p_memory) - 1);
    auto *allocation = reinterpret_cast<Allocation *>(static_cast<uint8_t *>(p_memory) - offset) - 1;
    // 不能用本次线程上的预算替换旧块所属；新旧块同时存活时按真实申请峰值扣额度。
    MetonNativeMemoryScope scope(allocation->owner ? allocation->owner : current_budget);
    void *replacement = meton_resource_alloc_aligned(p_bytes, p_alignment);
    if (replacement) std::memcpy(replacement, p_memory, p_previous_bytes < p_bytes ? p_previous_bytes : p_bytes);
    meton_resource_free_aligned(p_memory);
    return replacement;
}
