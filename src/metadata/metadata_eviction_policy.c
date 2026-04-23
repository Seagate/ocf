/*
 * Copyright(c) 2020-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf/ocf.h"
#include "metadata.h"
#include "metadata_eviction_policy.h"
#include "metadata_internal.h"

/*
 * Eviction policy - Get
 */
struct ocf_lru_meta * ocf_metadata_get_lru(struct ocf_cache *cache,
		ocf_cache_line_t line)
{
	ENV_BUG_ON(cache->conf_meta->eviction_policy_type != ocf_eviction_lru);

	struct ocf_metadata_ctrl *ctrl
		= (struct ocf_metadata_ctrl *) cache->metadata.priv;

	return (struct ocf_lru_meta *)ocf_metadata_raw_wr_access(cache,
			&(ctrl->raw_desc[metadata_segment_eviction]), line);
}

struct ocf_lfu_meta * ocf_metadata_get_lfu(struct ocf_cache *cache,
		ocf_cache_line_t line)
{
	ENV_BUG_ON(cache->conf_meta->eviction_policy_type != ocf_eviction_lfu);

	struct ocf_metadata_ctrl *ctrl
		= (struct ocf_metadata_ctrl *) cache->metadata.priv;

	return (struct ocf_lfu_meta *)ocf_metadata_raw_wr_access(cache,
			&(ctrl->raw_desc[metadata_segment_eviction]), line);
}


