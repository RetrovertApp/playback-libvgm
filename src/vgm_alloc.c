///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Allocator - Implementation
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "vgm_alloc.h"
#include <stdlib.h>
#include <string.h>

#define VGM_ALLOC_INITIAL_CAPACITY 64

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int vgm_alloc_track(VgmAllocator* alloc, void* ptr) {
    if (alloc->count >= alloc->capacity) {
        u32 new_cap = alloc->capacity * 2;
        void** new_ptrs = (void**)realloc(alloc->ptrs, new_cap * sizeof(void*));
        if (new_ptrs == NULL) {
            return -1;
        }
        alloc->ptrs = new_ptrs;
        alloc->capacity = new_cap;
    }
    alloc->ptrs[alloc->count++] = ptr;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VgmAllocator* vgm_alloc_create(void) {
    VgmAllocator* alloc = (VgmAllocator*)calloc(1, sizeof(VgmAllocator));
    if (alloc == NULL) {
        return NULL;
    }
    alloc->ptrs = (void**)malloc(VGM_ALLOC_INITIAL_CAPACITY * sizeof(void*));
    if (alloc->ptrs == NULL) {
        free(alloc);
        return NULL;
    }
    alloc->capacity = VGM_ALLOC_INITIAL_CAPACITY;
    return alloc;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vgm_alloc_destroy(VgmAllocator* alloc) {
    if (alloc == NULL) {
        return;
    }
    for (u32 i = 0; i < alloc->count; i++) {
        free(alloc->ptrs[i]);
    }
    free(alloc->ptrs);
    free(alloc);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vgm_alloc_rewind(VgmAllocator* alloc) {
    if (alloc == NULL) {
        return;
    }
    for (u32 i = 0; i < alloc->count; i++) {
        free(alloc->ptrs[i]);
    }
    alloc->count = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void* vgm_alloc(VgmAllocator* alloc, u32 size) {
    void* ptr = calloc(1, size);
    if (ptr == NULL) {
        return NULL;
    }
    if (vgm_alloc_track(alloc, ptr) != 0) {
        free(ptr);
        return NULL;
    }
    return ptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void* vgm_alloc_array(VgmAllocator* alloc, u32 size) {
    return vgm_alloc(alloc, size);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void* vgm_realloc(VgmAllocator* alloc, void* old_ptr, u32 new_size) {
    void* new_ptr = realloc(old_ptr, new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    // Update tracking entry
    if (new_ptr != old_ptr) {
        for (u32 i = 0; i < alloc->count; i++) {
            if (alloc->ptrs[i] == old_ptr) {
                alloc->ptrs[i] = new_ptr;
                return new_ptr;
            }
        }
        // old_ptr wasn't tracked (shouldn't happen), track new_ptr
        vgm_alloc_track(alloc, new_ptr);
    }
    return new_ptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
