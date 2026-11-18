#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/** Godot 与内置 PNG 共用申请/释放配对，保留分配时的预算身份。 */
void *meton_resource_alloc(size_t p_bytes);
void *meton_resource_calloc(size_t p_count, size_t p_bytes);
void *meton_resource_realloc(void *p_memory, size_t p_bytes);
void meton_resource_free(void *p_memory);
/** 对齐重分配同样沿原身份，复制长度不得超过较小的新旧载荷。 */
void *meton_resource_alloc_aligned(size_t p_bytes, size_t p_alignment);
void *meton_resource_realloc_aligned(void *p_memory, size_t p_bytes, size_t p_previous_bytes, size_t p_alignment);
void meton_resource_free_aligned(void *p_memory);
#ifdef __cplusplus
}
#endif
