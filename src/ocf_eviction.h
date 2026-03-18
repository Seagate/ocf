#ifndef EVICTION_H_
#define EVICTION_H_

#include "ocf/ocf.h"

#define EVICTION_POLICY_CONFIG_BYTES 256
#define EVICTION_POLICY_TYPE_MAX 2

struct ocf_part;
struct ocf_request;
struct ocf_user_part;

struct eviction_policy_config {
	uint8_t data[EVICTION_POLICY_CONFIG_BYTES];
};

typedef struct ocf_eviction_policy *ocf_eviction_policy_t;

typedef void (*ocf_eviction_populate_end_t)(void *priv, int error);

void ocf_eviction_setup(ocf_cache_t cache);
void ocf_eviction_init(ocf_cache_t cache, struct ocf_part *part);
void ocf_eviction_deinit(ocf_cache_t cache);

// ocf_error_t ocf_eviction_set_param(ocf_cache_t cache,
// 		uint8_t param_id, uint32_t param_value);
// ocf_error_t ocf_eviction_get_param(ocf_cache_t cache,
// 		uint8_t param_id, uint32_t *param_value);

void ocf_eviction_init_cline(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_eviction_hot_cline(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_eviction_add(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_eviction_add_free(ocf_cache_t cache, ocf_cache_line_t cline);
void ocf_eviction_rm_cline(ocf_cache_t cache, ocf_cache_line_t cline);

uint32_t ocf_eviction_req_clines(struct ocf_request *req,
		struct ocf_part *src_part, uint32_t cline_no);

void ocf_eviction_repart(ocf_cache_t cache, ocf_cache_line_t cline,
		struct ocf_part *src_part, struct ocf_part *dst_part);

void ocf_eviction_dirty_cline(ocf_cache_t cache, struct ocf_part *part,
		ocf_cache_line_t cline);
void ocf_eviction_clean_cline(ocf_cache_t cache, struct ocf_part *part,
		ocf_cache_line_t cline);

void ocf_eviction_clean(ocf_cache_t cache, struct ocf_user_part *user_part,
		ocf_queue_t io_queue, uint32_t count);

void ocf_eviction_populate(ocf_cache_t cache,
		ocf_eviction_populate_end_t cmpl, void *priv);

#endif /* EVICTION_H_ */