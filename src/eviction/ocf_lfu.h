// ocf_lfu.h - LFU metadata and API declarations

#ifndef OCF_LFU_H_
#define OCF_LFU_H_

#include "ocf_eviction.h"

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

void ocf_lfu_detach(ocf_cache_t cache, struct ocf_part *part,
		ocf_cache_line_t cline);
void ocf_lfu_reattach(ocf_cache_t cache, ocf_cache_line_t cline);

void ocf_lfu_populate(ocf_cache_t cache,
		ocf_eviction_populate_end_t cmpl, void *priv);

int ocf_lfu_restore_runtime(ocf_cache_t cache);

int ocf_lfu_metadata_actor(struct ocf_cache *cache,
		ocf_part_id_t part_id, ocf_core_id_t core_id,
		uint64_t start_byte, uint64_t end_byte,
		ocf_metadata_actor_t actor);

#endif