#ifndef __EVICTION_LFU_STRUCTS_H__

#define __EVICTION_LFU_STRUCTS_H__

struct ocf_lfu_meta {
    uint32_t freq;          // Access frequency counter
    uint32_t prev;          // For doubly linked list in freq bucket
    uint32_t next;
    bool clean;
    ocf_part_id_t partition_id;
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
	struct ocf_lfu_bucket freq_buckets[LFU_MAX_FREQ];
};

#endif