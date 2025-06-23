// ocf_lfu.c - Minimal LFU implementation

#include "ocf_lfu.h"
#include "ocf_env.h"
#include "ocf_cache_priv.h"


static const uint32_t END_MARKER = (uint32_t)-1;

// Create an array of doubly linked lists for each frequency
struct ocf_lfu_list freq_buckets[MAX_FREQ];


// Initialize cacheline LFU metadata
void ocf_lfu_init_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    meta->freq = 0;
    meta->prev = END_MARKER;
    meta->next = END_MARKER;
}

// Add cache line to frequency bucket list head
static void add_to_freq_bucket(uint32_t freq, ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_list *list = &freq_buckets[freq];
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

    meta->prev = END_MARKER;
    meta->next = list->head;

    if (list->head != END_MARKER) {
        struct ocf_lfu_meta *head_meta = ocf_metadata_get_lfu(cache, list->head);
        head_meta->prev = cline;
    }
    list->head = cline;

    if (list->tail == END_MARKER)
        list->tail = cline;

    list->num_nodes++;
}

// Remove cache line from its current freq bucket
static void remove_from_freq_bucket(uint32_t freq, ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_list *list = &freq_buckets[freq];
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

    if (meta->prev != END_MARKER) {
        struct ocf_lfu_meta *prev_meta = ocf_metadata_get_lfu(cache, meta->prev);
        prev_meta->next = meta->next;
    } else {
        list->head = meta->next;
    }

    if (meta->next != END_MARKER) {
        struct ocf_lfu_meta *next_meta = ocf_metadata_get_lfu(cache, meta->next);
        next_meta->prev = meta->prev;
    } else {
        list->tail = meta->prev;
    }

    meta->prev = END_MARKER;
    meta->next = END_MARKER;

    list->num_nodes--;
}

// Called when a cache line is accessed, increment frequency
void ocf_lfu_increment(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    uint32_t old_freq = meta->freq;
    uint32_t new_freq = (old_freq < MAX_FREQ - 1) ? old_freq + 1 : old_freq;

    remove_from_freq_bucket(old_freq, cache, cline);

    meta->freq = new_freq;

    add_to_freq_bucket(new_freq, cache, cline);
}

// Add a new cache line to freq bucket 0 on insertion
void ocf_lfu_add(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    meta->freq = 0;
    add_to_freq_bucket(0, cache, cline);
}

// Remove a cache line completely from freq buckets (on eviction)
void ocf_lfu_remove(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    remove_from_freq_bucket(meta->freq, cache, cline);
}

// Select victim: pick the least frequently used cache line
ocf_cache_line_t ocf_lfu_select_victim(ocf_cache_t cache)
{
    for (uint32_t freq = 0; freq < MAX_FREQ; freq++) {
        struct ocf_lfu_list *list = &freq_buckets[freq];
        if (list->num_nodes > 0) {
            // Pick the tail (oldest in this freq bucket) as victim
            return list->tail;
        }
    }
    // No victim found (empty cache)
    return END_MARKER;
}

static inline void lfu_iter_eviction_init(struct ocf_lfu_iter *iter,
        ocf_cache_t cache, struct ocf_part *part,
        struct ocf_request *req)
{
    iter->cache = cache;
    iter->part = part;
    iter->req = req;
    iter->current_freq = 0;
}

static inline ocf_cache_line_t lfu_iter_eviction_next(struct ocf_lfu_iter *iter,
        struct ocf_part *dst_part, ocf_core_id_t *core_id,
        uint64_t *core_line)
{
    ocf_cache_t cache = iter->cache;
    struct ocf_part *part = iter->part;
    struct ocf_lfu_list *bucket;
    ocf_cache_line_t cline;

    for (; iter->current_freq < MAX_FREQ; iter->current_freq++) {
        bucket = &part->runtime->freq_buckets[iter->current_freq];

        ocf_metadata_lru_wr_lock(&cache->metadata.lock, iter->current_freq);

        cline = bucket->tail;

        while (cline != END_MARKER) {
            if (!ocf_cache_line_try_lock_wr(ocf_cache_line_concurrency(cache), cline)) {
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
                continue;
            }

            ocf_metadata_get_core_info(cache, cline, core_id, core_line);

            if (*core_id == ocf_core_get_id(iter->req->core) &&
                *core_line >= iter->req->core_line_first &&
                *core_line <= iter->req->core_line_last) {
                ocf_cache_line_unlock_wr(ocf_cache_line_concurrency(cache), cline);
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
                continue;
            }

            // Hash locking (if needed)
            if (!ocf_hb_cline_naked_trylock_wr(&cache->metadata.lock, *core_id, *core_line)) {
                ocf_cache_line_unlock_wr(ocf_cache_line_concurrency(cache), cline);
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
                continue;
            }

            // Move to target partition
            if (dst_part != part) {
                ocf_lfu_repart_locked(cache, cline, part, dst_part);
            }

            ocf_metadata_lru_wr_unlock(&cache->metadata.lock, iter->current_freq);
            return cline;
        }

        ocf_metadata_lru_wr_unlock(&cache->metadata.lock, iter->current_freq);
    }

    return END_MARKER;
}
