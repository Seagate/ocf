/*
 * Copyright(c) 2026 Seagate Technology LLC and/or its affiliates
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef EVICTION_OPS_H_
#define EVICTION_OPS_H_

#include "../metadata/metadata.h"

struct ocf_eviction_policy {
	ocf_cache_t owner;

	ocf_eviction_t type;

	void *config;
	/* Pointer to config values stored in cache superblock */

	void *ctx;
};

struct eviction_policy_ops {
	const char *name;
		/*!< Eviction policy name */

	void (*setup)(ocf_cache_t cache);
		/*!< Initialize eviction policy default config */

	void (*init)(ocf_cache_t cache, struct ocf_part *part);
		/*!< Initialize/reset eviction policy structures */

	int (*init_part)(ocf_cache_t cache, struct ocf_part *part);
		/*!< Allocate memory for partition and initialize eviction policy */

	void (*deinit_part)(ocf_cache_t cache, struct ocf_part *part);
		/*!< Deinitialize and free eviction policy */

	// ocf_error_t (*set_param)(ocf_cache_t cache, uint8_t param_id,
	// 		uint32_t param_value);
	// 	/*!< Set eviction policy parameter */

	// ocf_error_t (*get_param)(ocf_cache_t cache, uint8_t param_id,
	// 		uint32_t *param_value);
	// 	/*!< Get eviction policy parameter */

	void (*init_cline)(ocf_cache_t cache, ocf_cache_line_t cline);
		/*!< Initialize cacheline metadata for eviction policy */

	void (*hot_cline)(ocf_cache_t cache, ocf_cache_line_t cline);
		/*!< Called when cached cacheline is hit */

	void (*add)(ocf_cache_t cache, ocf_cache_line_t cline);
		/*!< Called when cacheline is inserted */

    void (*add_free)(ocf_cache_t cache, ocf_cache_line_t cline);
		/*!< Called when cacheline is inserted into freelist */

	void (*rm_cline)(ocf_cache_t cache, ocf_cache_line_t cline);
		/*!< Remove cacheline from eviction structures */

	uint32_t (*req_clines)(struct ocf_request *req,
			struct ocf_part *src_part, uint32_t cline_no);
		/*!< Select/request cachelines for eviction */

	void (*repart)(ocf_cache_t cache, ocf_cache_line_t cline,
			struct ocf_part *src_part, struct ocf_part *dst_part);
		/*!< Repartition cacheline between partitions */

	void (*dirty_cline)(ocf_cache_t cache, struct ocf_part *part,
			ocf_cache_line_t cline);
		/*!< Mark cacheline dirty in eviction metadata */

	void (*clean_cline)(ocf_cache_t cache, struct ocf_part *part,
			ocf_cache_line_t cline);
		/*!< Mark cacheline clean in eviction metadata */

	void (*clean)(ocf_cache_t cache, struct ocf_user_part *user_part,
		ocf_queue_t io_queue, uint32_t count);
		/*!< Start cacheline cleaning */

	void (*populate)(ocf_cache_t cache,
			void (*cmpl)(void *priv, int error), void *priv);
		/*!< Populate runtime eviction metadata from current cache state */

	int (*restore_runtime)(ocf_cache_t cache);
		/*!< Restore eviction runtime metadata on cache load */

	int (*metadata_actor)(struct ocf_cache *cache,
                       ocf_part_id_t part_id, ocf_core_id_t core_id,
                       uint64_t start_byte, uint64_t end_byte,
                       ocf_metadata_actor_t actor);
		/*!< Get implementation of ocf_metadata_actor according to the eviction policy */

	void (*detach)(ocf_cache_t cache, struct ocf_part *part,
		ocf_cache_line_t cline);
		/*!< Move cacheline to detached freelist */

	void (*reattach)(ocf_cache_t cache, ocf_cache_line_t cline);
		/*!< Move cacheline back to available list */
};

extern struct eviction_policy_ops
	ocf_eviction_policies[ocf_eviction_max];

#endif /* EVICTION_OPS_H_ */