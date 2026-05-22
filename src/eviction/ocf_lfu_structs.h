/*
 * Copyright(c) 2026 Seagate Technology LLC and/or its affiliates
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __EVICTION_LFU_STRUCTS_H__

#define __EVICTION_LFU_STRUCTS_H__

/* Raw hit counter */
typedef uint16_t ocf_lfu_hits_t;
#define LFU_HITS_BITS (sizeof(ocf_lfu_hits_t) * 8)

#define LFU_FREQ_BITS \
    ((LFU_MAX_FREQ) <= 1 ? 0 : (32 - __builtin_clz((LFU_MAX_FREQ) - 1)))

struct ocf_lfu_meta {
    /* 1st 64-bit word */
    uint64_t prev : OCF_CACHE_LINE_BITS;
    uint64_t next : OCF_CACHE_LINE_BITS;
    uint64_t clean : 1;
    uint64_t freq : LFU_FREQ_BITS;
    uint64_t unused : 1;

    /* 2nd 64-bit word */
    uint64_t hits : LFU_HITS_BITS;
    uint64_t partition_id : 8;
    uint64_t unused2 : (64 - LFU_HITS_BITS - 8);
} __attribute__((packed));

struct ocf_lfu_list {
    uint32_t head;          // Head of list for this frequency
    uint32_t tail;          // Tail of list
    uint32_t num_nodes;     // Total nodes in this freq bucket
};

struct ocf_lfu_bucket {
    struct ocf_lfu_list clean; // Clean list in freq bucket
    struct ocf_lfu_list dirty; // Dirty list in freq bucket
};

struct ocf_lfu_part_runtime {
	struct ocf_lfu_bucket freq_buckets[LFU_NUM_SHARDS][LFU_MAX_FREQ];
};

#endif