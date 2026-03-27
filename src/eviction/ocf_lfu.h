// ocf_lfu.h - LFU metadata and API declarations

#ifndef OCF_LFU_H_
#define OCF_LFU_H_

#include "../ocf_space.h"
#include "ocf_eviction.h"

#define MAX_FREQ 32  // Maximum frequency bucket count (tune as needed)

struct ocf_lfu_meta {
    uint32_t freq;          // Access frequency counter
    uint32_t prev;          // For doubly linked list in freq bucket
    uint32_t next;
    bool clean;
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

struct ocf_part;
struct ocf_user_part;
struct ocf_part_runtime;
struct ocf_part_cleaning_ctx;
struct ocf_request;

int ocf_lfu_init_part(ocf_cache_t cache, struct ocf_part *part);
void ocf_lfu_deinit_part(ocf_cache_t cache, struct ocf_part *part);
void ocf_lfu_init_cline(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_lfu_increment(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_lfu_add(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_lfu_remove(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_lfu_init(ocf_cache_t cache, struct ocf_part *part);
void ocf_lfu_rm_cline(struct ocf_cache *cache, ocf_cache_line_t cline);
uint32_t ocf_lfu_req_clines(struct ocf_request *req,
                            struct ocf_part *src_part,
                            uint32_t cline_no);
void ocf_lfu_repart(ocf_cache_t cache, ocf_cache_line_t cline,
                    struct ocf_part *src_part, struct ocf_part *dst_part);

void ocf_lfu_dirty_cline(ocf_cache_t cache, struct ocf_part *part, ocf_cache_line_t cline);
void ocf_lfu_clean_cline(ocf_cache_t cache, struct ocf_part *part, ocf_cache_line_t cline);
void ocf_lfu_clean(ocf_cache_t cache, struct ocf_user_part *user_part,
                   ocf_queue_t io_queue, uint32_t count);

void ocf_lfu_populate(ocf_cache_t cache,
		ocf_eviction_populate_end_t cmpl, void *priv);

int ocf_lfu_restore_runtime(ocf_cache_t cache);

#endif