#include "ocf_eviction.h"
#include "ocf_lfu.h"
#include "ocf_lru.h"

struct eviction_policy_ops ocf_eviction_policies[ocf_eviction_max] = {
    [ocf_eviction_lru] = {
        .name = "lru",
        .init = ocf_lru_init,
        .init_cline = ocf_lru_init_cline,
        .hot_cline = ocf_lru_hot_cline,
        .add = ocf_lru_add,
        .add_free = ocf_lru_add_free,
        .rm_cline = ocf_lru_rm_cline,
        .req_clines = ocf_lru_req_clines,
        .repart = ocf_lru_repart,
        .dirty_cline = ocf_lru_dirty_cline,
        .clean_cline = ocf_lru_clean_cline,
        .populate = ocf_lru_populate
    },

    [ocf_eviction_lfu] = {
        .name = "lfu", 
        .init = ocf_lfu_init, 
        .init_cline = ocf_lfu_init_cline, 
        .hot_cline = ocf_lfu_hot_cline, 
        .add = ocf_lfu_add, 
        .add_free = ocf_lfu_add,
        .rm_cline = ocf_lfu_rm_cline, 
        .req_clines = ocf_lfu_req_clines, 
        .repart = ocf_lfu_repart, 
        .dirty_cline = ocf_lfu_dirty_cline, 
        .clean_cline = ocf_lfu_clean_cline, 
        .populate = ocf_lfu_populate
    }
}

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

void ocf_eviction_deinit(ocf_cache_t cache) {}

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
    else
    {
        return 0;
    }
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