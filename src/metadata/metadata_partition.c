/*
 * Copyright(c) 2012-2021 Intel Corporation
 * Copyright(c) 2022-2023 Huawei Technologies
 * Copyright(c) 2026 Unvertical
 * Copyright(c) 2026 Seagate Technology LLC and/or its affiliates
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf/ocf.h"
#include "metadata.h"
#include "metadata_internal.h"
#include "../utils/utils_user_part.h"

ocf_part_id_t ocf_metadata_get_partition_id(struct ocf_cache *cache,
											ocf_cache_line_t line)
{
	struct ocf_metadata_ctrl *ctrl =
		(struct ocf_metadata_ctrl *)cache->metadata.priv;

	// TODO: Move the logic to eviction?
	switch (cache->eviction_policy)
	{
		case ocf_eviction_lru: {
			const struct ocf_lru_meta *info;

			info = (const struct ocf_lru_meta *)ocf_metadata_raw_rd_access(cache,
											&(ctrl->raw_desc[metadata_segment_eviction]), line);

			ENV_BUG_ON(!info);

			return info->partition_id;

			break;
		}
		case ocf_eviction_lfu: {
			const struct ocf_lfu_meta *info;

			info = (const struct ocf_lfu_meta *)ocf_metadata_raw_rd_access(cache,
											&(ctrl->raw_desc[metadata_segment_eviction]), line);

			ENV_BUG_ON(!info);

			return info->partition_id;

			break;
		}
		default: {
			ocf_metadata_error(cache);
			ENV_BUG();
			return OCF_NUM_PARTITIONS + 1;
			break;
		}
	}
}

void ocf_metadata_set_partition_id(struct ocf_cache *cache,
								   ocf_cache_line_t line, ocf_part_id_t part_id)
{

	struct ocf_metadata_ctrl *ctrl =
		(struct ocf_metadata_ctrl *)cache->metadata.priv;

	switch (cache->eviction_policy)
	{
		case ocf_eviction_lru: {
			struct ocf_lru_meta *info;

			info = (struct ocf_lru_meta *)ocf_metadata_raw_wr_access(cache,
											&(ctrl->raw_desc[metadata_segment_eviction]), line);

			if (info)
				info->partition_id = part_id;
			else
				ocf_metadata_error(cache);

			break;
		}
		case ocf_eviction_lfu: {
			struct ocf_lfu_meta *info;

			info = (struct ocf_lfu_meta *)ocf_metadata_raw_rd_access(cache,
											&(ctrl->raw_desc[metadata_segment_eviction]), line);

			if (info)
				info->partition_id = part_id;
			else
				ocf_metadata_error(cache);
			
			break;
		}
		default: {
			ocf_metadata_error(cache);

			break;
		}
	}
}
