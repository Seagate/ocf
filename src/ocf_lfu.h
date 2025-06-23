// ocf_lfu.h - LFU metadata and API declarations

#ifndef OCF_LFU_H_
#define OCF_LFU_H_

#include "ocf_space.h"

#define MAX_FREQ 1024  // Maximum frequency bucket count (tune as needed)

struct ocf_lfu_meta {
    uint32_t freq;          // Access frequency counter
    uint32_t prev;          // For doubly linked list in freq bucket
    uint32_t next;
};

struct ocf_lfu_list {
    uint32_t head;          // Head of list for this frequency
    uint32_t tail;          // Tail of list
    uint32_t num_nodes;     // Total nodes in this freq bucket
};

void ocf_lfu_init_cline(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_lfu_increment(ocf_cache_t cache, ocf_cache_line_t cline);
ocf_cache_line_t ocf_lfu_select_victim(ocf_cache_t cache);
void ocf_lfu_add(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_lfu_remove(ocf_cache_t cache, ocf_cache_line_t cline);

#endif