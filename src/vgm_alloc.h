///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Allocator - Simple allocation tracker with bulk free
//
// Replaces arena allocator dependency, allowing standalone builds.
// Tracks all allocations for bulk deallocation (rewind/destroy).
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdint.h>

typedef uint32_t u32;

typedef struct VgmAllocator {
    void** ptrs;  // Array of allocated pointers
    u32 count;    // Number of active allocations
    u32 capacity; // Capacity of ptrs array
} VgmAllocator;

// Create allocator
VgmAllocator* vgm_alloc_create(void);

// Destroy allocator and free all tracked allocations
void vgm_alloc_destroy(VgmAllocator* alloc);

// Free all tracked allocations but keep the allocator alive
void vgm_alloc_rewind(VgmAllocator* alloc);

// Allocate zero-initialized memory and track it
void* vgm_alloc(VgmAllocator* alloc, u32 size);

// Allocate zero-initialized array and track it
void* vgm_alloc_array(VgmAllocator* alloc, u32 size);

// Reallocate tracked memory (updates tracking entry)
void* vgm_realloc(VgmAllocator* alloc, void* old_ptr, u32 new_size);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
