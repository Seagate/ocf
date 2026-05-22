/*
 * Copyright(c) 2026 Seagate Technology LLC and/or its affiliates
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf_eviction.h"
#include "ocf_eviction_ops.h"
#include "../ocf_request.h"
#include "ocf_lfu.h"
#include "ocf_lru.h"

struct eviction_policy_ops ocf_eviction_policies[ocf_eviction_max] = {
    [ocf_eviction_lru] = {
        .name = "lru",
        .init = ocf_lru_init,
        .init_part = ocf_lru_init_part,
        .deinit_part = ocf_lru_deinit_part,
        .init_cline = ocf_lru_init_cline,
        .hot_cline = ocf_lru_hot_cline,
        .add = ocf_lru_add,
        .add_free = ocf_lru_add_free,
        .rm_cline = ocf_lru_rm_cline,
        .req_clines = ocf_lru_req_clines,
        .repart = ocf_lru_repart,
        .dirty_cline = ocf_lru_dirty_cline,
        .clean_cline = ocf_lru_clean_cline,
        .clean = ocf_lru_clean,
        .populate = ocf_lru_populate,
        .restore_runtime = ocf_lru_restore_runtime,
        .metadata_actor = ocf_lru_metadata_actor,
        .detach = ocf_lru_detach,
        .reattach = ocf_lru_reattach
    },

    [ocf_eviction_lfu] = {
        .name = "lfu", 
        .init = ocf_lfu_init, 
        .init_part = ocf_lfu_init_part,
        .deinit_part = ocf_lfu_deinit_part,
        .init_cline = ocf_lfu_init_cline, 
        .hot_cline = ocf_lfu_increment, 
        .add = ocf_lfu_add, 
        .add_free = ocf_lfu_add, 
        .rm_cline = ocf_lfu_rm_cline, 
        .req_clines = ocf_lfu_req_clines, 
        .repart = ocf_lfu_repart, 
        .dirty_cline = ocf_lfu_dirty_cline, 
        .clean_cline = ocf_lfu_clean_cline, 
        .clean = ocf_lfu_clean,
        .populate = ocf_lfu_populate,
        .restore_runtime = ocf_lfu_restore_runtime,
        .metadata_actor = ocf_lfu_metadata_actor,
        .detach = ocf_lfu_detach,
        .reattach = ocf_lfu_reattach
    }
};

void ocf_eviction_setup(ocf_cache_t cache)
{
}

void ocf_eviction_init(ocf_cache_t cache, struct ocf_part *part)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].init)
    {
        ocf_eviction_policies[type].init(cache, part);
    }
}

int ocf_eviction_init_part(ocf_cache_t cache, struct ocf_part *part) {
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].init_part)
    {
        return ocf_eviction_policies[type].init_part(cache, part);
    } 
    
    return 1;
}

void ocf_eviction_deinit_part(ocf_cache_t cache, struct ocf_part *part) {
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (!part->eviction_runtime)
		return;

    if (ocf_eviction_policies[type].deinit_part)
    {
        ocf_eviction_policies[type].deinit_part(cache, part);
    } 
}

// ocf_error_t ocf_eviction_set_param(ocf_cache_t cache,
// 		uint8_t param_id, uint32_t param_value);
// ocf_error_t ocf_eviction_get_param(ocf_cache_t cache,
// 		uint8_t param_id, uint32_t *param_value);

void ocf_eviction_init_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].init_cline)
    {
        ocf_eviction_policies[type].init_cline(cache, cline);
    }
}

void ocf_eviction_hot_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].hot_cline)
    {
        ocf_eviction_policies[type].hot_cline(cache, cline);
    }
}

void ocf_eviction_add_free(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].add_free)
    {
        ocf_eviction_policies[type].add_free(cache, cline);
    }
}

void ocf_eviction_add(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].add)
    {
        ocf_eviction_policies[type].add(cache, cline);
    }
}

void ocf_eviction_rm_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].rm_cline)
    {
        ocf_eviction_policies[type].rm_cline(cache, cline);
    }
}

uint32_t ocf_eviction_req_clines(struct ocf_request *req,
                                 struct ocf_part *src_part, uint32_t cline_no)
{
    ocf_eviction_t type = req->cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].req_clines)
    {
        return ocf_eviction_policies[type].req_clines(req, src_part, cline_no);
    }

    return 1;
}

void ocf_eviction_repart(ocf_cache_t cache, ocf_cache_line_t cline,
                         struct ocf_part *src_part, struct ocf_part *dst_part)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].repart)
    {
        ocf_eviction_policies[type].repart(cache, cline, src_part, dst_part);
    }
}

void ocf_eviction_dirty_cline(ocf_cache_t cache, struct ocf_part *part,
                              ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].dirty_cline)
    {
        ocf_eviction_policies[type].dirty_cline(cache, part, cline);
    }
}

void ocf_eviction_clean_cline(ocf_cache_t cache, struct ocf_part *part,
                              ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].clean_cline)
    {
        ocf_eviction_policies[type].clean_cline(cache, part, cline);
    }
}

void ocf_eviction_clean(ocf_cache_t cache, struct ocf_user_part *user_part,
                        ocf_queue_t io_queue, uint32_t count)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].clean)
    {
        ocf_eviction_policies[type].clean(cache, user_part, io_queue, count);
    }
}

void ocf_eviction_populate(ocf_cache_t cache,
                           ocf_eviction_populate_end_t cmpl, void *priv)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].populate)
    {
        ocf_eviction_policies[type].populate(cache, cmpl, priv);
    }
}

int ocf_eviction_restore_runtime(ocf_cache_t cache) 
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].restore_runtime)
    {
        return ocf_eviction_policies[type].restore_runtime(cache);
    }
    return -1;
}

/** Functionality copied over from LRU */
bool _is_cache_line_acting(struct ocf_cache *cache,
                                  uint32_t cache_line, ocf_core_id_t core_id,
                                  uint64_t start_line, uint64_t end_line)
{
    ocf_core_id_t tmp_core_id;
    uint64_t core_line;

    ocf_metadata_get_core_info(cache, cache_line,
                               &tmp_core_id, &core_line);

    if (core_id != OCF_CORE_ID_INVALID)
    {
        if (core_id != tmp_core_id)
            return false;

        if (core_line < start_line || core_line > end_line)
            return false;
    }
    else if (tmp_core_id == OCF_CORE_ID_INVALID)
    {
        return false;
    }

    return true;
}

/*
 * Iterates over cache lines that belong to the core device with
 * core ID = core_id  whose core byte addresses are in the range
 * [start_byte, end_byte] and applies actor(cache, cache_line) to all
 * matching cache lines
 *
 * set partition_id to PARTITION_UNSPECIFIED to not care about partition_id
 *
 * global metadata write lock must be held before calling this function
 */
int ocf_metadata_actor(struct ocf_cache *cache,
                       ocf_part_id_t part_id, ocf_core_id_t core_id,
                       uint64_t start_byte, uint64_t end_byte,
                       ocf_metadata_actor_t actor)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].restore_runtime)
    {
        return ocf_eviction_policies[type].metadata_actor(cache, part_id, core_id, start_byte, end_byte, actor);
    }
    return -1;
}

uint32_t ocf_eviction_num_free(ocf_cache_t cache)
{
	return env_atomic_read(&cache->free.runtime->curr_size);
}

void ocf_eviction_detach(ocf_cache_t cache, struct ocf_part *part,
		ocf_cache_line_t cline) 
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].restore_runtime)
    {
        return ocf_eviction_policies[type].detach(cache, part, cline);
    }
}

void ocf_eviction_reattach(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_eviction_t type = cache->eviction_policy;

    ENV_BUG_ON(type >= ocf_eviction_max);

    if (ocf_eviction_policies[type].restore_runtime)
    {
        return ocf_eviction_policies[type].reattach(cache, cline);
    }
}