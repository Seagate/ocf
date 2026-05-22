/*
 * Copyright(c) 2012-2021 Intel Corporation
 * Copyright(c) 2026 Seagate Technology LLC and/or its affiliates
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __METADATA_EVICTION_H__
#define __METADATA_EVICTION_H__

struct ocf_lru_meta *
ocf_metadata_get_lru(
		struct ocf_cache *cache, ocf_cache_line_t line);

/* Get LFU metadata */
struct ocf_lfu_meta *
ocf_metadata_get_lfu(
		struct ocf_cache *cache, ocf_cache_line_t line);

#endif /* METADATA_EVICTION_H_ */
