// ocf_lfu.c - Minimal LFU implementation

#include "ocf_space.h"
#include "ocf_lfu.h"
#include "utils/utils_cleaner.h"
#include "utils/utils_cache_line.h"
#include "utils/utils_generator.h"
#include "utils/utils_parallelize.h"
#include "concurrency/ocf_concurrency.h"
#include "mngt/ocf_mngt_common.h"
#include "engine/engine_zero.h"
#include "ocf_cache_priv.h"
#include "ocf_request.h"
#include "engine/engine_common.h"


static const uint32_t END_MARKER = (uint32_t)-1;

/** Get frequency bucket from cache partition */
static inline struct ocf_lfu_list *ocf_lfu_get_list(struct ocf_part *part, uint32_t freq) {
    ENV_BUG_ON(freq >= MAX_FREQ);
    return &part->runtime->freq_buckets[freq];
}

/** Get frequency bucket cache line belongs to */
static inline struct ocf_lfu_list *lfu_get_cline_list(ocf_cache_t cache,
		ocf_cache_line_t cline)
{
	ocf_part_id_t part_id;
	struct ocf_part *part;

	part_id = ocf_metadata_get_partition_id(cache, cline);

	ENV_BUG_ON(part_id > OCF_USER_IO_CLASS_MAX);
	part = &cache->user_parts[part_id].part;

    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

	return ocf_lfu_get_list(part, meta->freq);
}

/** Initialize cacheline LFU metadata */
void ocf_lfu_init_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    meta->freq = 0;
    meta->prev = END_MARKER;
    meta->next = END_MARKER;
}

/** Add cache line to frequency bucket list head */
static void add_to_freq_bucket(uint32_t freq, ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_list *list = lfu_get_cline_list(cache, cline);
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

/** Remove cache line from its current freq bucket */
static void remove_from_freq_bucket(uint32_t freq, ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_list *list = lfu_get_cline_list(cache, cline);
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

/** Called when a cache line is accessed, increment frequency */
void ocf_lfu_increment(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    uint32_t old_freq = meta->freq;
    uint32_t new_freq = (old_freq < MAX_FREQ - 1) ? old_freq + 1 : old_freq;

    remove_from_freq_bucket(old_freq, cache, cline);

    meta->freq = new_freq;

    add_to_freq_bucket(new_freq, cache, cline);
}

/** Add a new cache line to freq bucket 0 on insertion */
void ocf_lfu_add(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    meta->freq = 0;
    add_to_freq_bucket(0, cache, cline);
}

/** Remove a cache line completely from freq buckets (on eviction) */
void ocf_lfu_remove(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    remove_from_freq_bucket(meta->freq, cache, cline);
}

/** Invalidate a cache line during eviction, including
 * Clearing valid data
 * Removing the line from its partition's metadata list
 * Updating runtime statistics
 */
static void ocf_lfu_invalidate(ocf_cache_t cache, ocf_cache_line_t cline,
                                ocf_core_id_t core_id, ocf_part_id_t part_id)
{
    ocf_core_t core;

    // Step 1: Start shared access to the cache line's metadata
    ocf_metadata_start_collision_shared_access(cache, cline);

    // Step 2: Clear all valid sectors in the cache line
    metadata_clear_valid_sec(cache, cline, 0, ocf_line_end_sector(cache));

    // Step 3: Remove cache line from collision table and from LFU frequency bucket
    ocf_metadata_remove_from_collision(cache, cline, part_id);
    ocf_lfu_remove(cache, cline);  // LFU-specific removal

    // Step 4: End shared access
    ocf_metadata_end_collision_shared_access(cache, cline);

    // Step 5: Update runtime stats for the core and the partition
    core = ocf_cache_get_core(cache, core_id);
    env_atomic_dec(&core->runtime_meta->cached_clines);
    env_atomic_dec(&core->runtime_meta->part_counters[part_id].cached_clines);
}

// Select victim: pick the least frequently used cache line
// ocf_cache_line_t ocf_lfu_select_victim(ocf_cache_t cache)
// {
//     for (uint32_t freq = 0; freq < MAX_FREQ; freq++) {
//         struct ocf_lfu_list *list = &freq_buckets[freq];
//         if (list->num_nodes > 0) {
//             // Pick the tail (oldest in this freq bucket) as victim
//             return list->tail;
//         }
//     }
//     // No victim found (empty cache)
//     return END_MARKER;
// }

/** Initialize LFU eviction iterator */
static inline void lfu_iter_eviction_init(struct ocf_lfu_iter *iter,
                                          ocf_cache_t cache,
                                          struct ocf_part *part,
                                          uint32_t start_freq,
                                          struct ocf_request *req)
{
	iter->cache = cache;
	iter->c = ocf_cache_line_concurrency(cache);
	iter->part = part;
	iter->current_freq = start_freq;
	iter->clean = true;  // Eviction typically scans clean lines first
	iter->hash_locked = ocf_req_hash_in_range;
	iter->req = req;
}

/** Advance LFU eviction iterator, find next eviction candidate */
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

        ocf_metadata_lfu_wr_lock(&cache->metadata.lock, iter->current_freq);

        cline = bucket->tail;

        while (cline != END_MARKER) {
            if (!ocf_cache_line_try_lock_wr(ocf_cache_line_concurrency(cache), cline)) {
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
                continue;
            }

            ocf_metadata_get_core_info(cache, cline, core_id, core_line);

            // Avoid evicting current request target cacheline
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

            ocf_metadata_lfu_wr_unlock(&cache->metadata.lock, iter->current_freq);
            return cline;
        }

        ocf_metadata_lfu_wr_unlock(&cache->metadata.lock, iter->current_freq);
    }

    return END_MARKER;
}

/** Move cache line to another cache partition */
void ocf_lfu_repart_locked(ocf_cache_t cache, ocf_cache_line_t cline,
                           struct ocf_part *src, struct ocf_part *dst)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    uint32_t freq = meta->freq;
    struct ocf_lfu_list *src_list, *dst_list;

    ENV_BUG_ON(freq >= MAX_FREQ);
    ENV_BUG_ON(src == dst);

    src_list = &src->runtime->freq_buckets[freq];
    dst_list = &dst->runtime->freq_buckets[freq];

    // Step 1: Remove from source list
    if (meta->prev != END_MARKER)
        ocf_metadata_get_lfu(cache, meta->prev)->next = meta->next;
    else
        src_list->head = meta->next;

    if (meta->next != END_MARKER)
        ocf_metadata_get_lfu(cache, meta->next)->prev = meta->prev;
    else
        src_list->tail = meta->prev;

    // Step 2: Insert into destination list at head
    meta->prev = END_MARKER;
    meta->next = dst_list->head;

    if (dst_list->head != END_MARKER)
        ocf_metadata_get_lfu(cache, dst_list->head)->prev = cline;
    else
        dst_list->tail = cline;

    dst_list->head = cline;

    // Step 3: Update partition metadata
    ocf_metadata_set_partition_id(cache, cline, dst->id);
}

/**
 * Get next clean cacheline from tail of free LFU frequency buckets.
 * - Caller must not hold any LFU bucket lock.
 * - Returned cacheline is write locked.
 * - Cacheline is moved to the head of the destination partition's LFU list.
 * - One frequency bucket is locked at a time.
 */
static inline ocf_cache_line_t lfu_iter_free_next(struct ocf_lfu_iter *iter,
                                                  struct ocf_part *dst_part)
{
    ocf_cache_t cache = iter->cache;
    struct ocf_part *free = iter->part;
    struct ocf_lfu_list *bucket;
    ocf_cache_line_t cline;

    // Sanity check: cannot repartition to the same freelist
    ENV_BUG_ON(dst_part == free);

    while (iter->current_freq < MAX_FREQ) {
        // Lock metadata for current frequency bucket
        ocf_metadata_lru_wr_lock(&cache->metadata.lock, iter->current_freq);

        // Get the current bucket (tail = least recently used at this frequency)
        bucket = &free->runtime->freq_buckets[iter->current_freq];
        cline = bucket->tail;

        // Iterate over bucket from tail (LFU logic: least frequently used = lower freq)
        while (cline != END_MARKER) {
            if (!ocf_cache_line_try_lock_wr(iter->c, cline)) {
                // Can't lock: go to next older entry
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
                continue;
            }

            // Move cacheline from free partition to destination
            ocf_lfu_repart_locked(cache, cline, free, dst_part);

            ocf_metadata_lru_wr_unlock(&cache->metadata.lock, iter->current_freq);
            return cline;
        }

        // No usable cacheline in this frequency bucket
        ocf_metadata_lru_wr_unlock(&cache->metadata.lock, iter->current_freq);

        // Advance to next frequency bucket
        iter->current_freq++;
    }

    // None found in any bucket
    return END_MARKER;
}

/** Check if hash bucket was locked by the caller and unlock if needed */
static inline void _lfu_unlock_hash(struct ocf_lfu_iter *iter,
		ocf_core_id_t core_id, uint64_t core_line)
{
	if (iter->hash_locked != NULL && iter->hash_locked(
				iter->req, core_id, core_line)) {
		return;
	}

	ocf_hb_cline_naked_unlock_wr(
			&iter->cache->metadata.lock,
			core_id, core_line);
}

/**
 * ocf_lfu_req_clines - Allocate cache lines for a request using LFU eviction
 *
 * @req:       Request for which cache lines need to be allocated
 * @src_part:  Source partition from which to take lines (user or freelist)
 * @cline_no:  Number of cache lines needed
 *
 * This function assigns cache lines from a source partition to the given request,
 * using an LFU-based eviction policy. Cache lines are either evicted (if from a
 * user partition) or reused (if from the freelist). All lines returned will be:
 *  - write-locked,
 *  - remapped in the request,
 *  - validated and marked as LOOKUP_REMAPPED.
 *
 * Returns the number of successfully assigned cache lines.
 */
uint32_t ocf_lfu_req_clines(struct ocf_request *req,
                            struct ocf_part *src_part,
                            uint32_t cline_no)
{
    struct ocf_alock* alock;
    struct ocf_lfu_iter iter;
    uint32_t i = 0;
    ocf_cache_line_t cline;
    uint64_t core_line;
    ocf_core_id_t core_id;
    ocf_cache_t cache = req->cache;
    unsigned req_idx = 0;
    struct ocf_part *dst_part;

    // Return early if nothing is requested
    if (cline_no == 0)
        return 0;

    // Safety check: ensure request has enough unmapped lines for assignment
    if (unlikely(ocf_engine_unmapped_count(req) < cline_no)) {
        ocf_cache_log(cache, log_err, "Not enough space in request: unmapped %u, requested %u",
                      ocf_engine_unmapped_count(req), cline_no);
        ENV_BUG();
    }

    // Target partition (where lines will be assigned) must not be the freelist
    ENV_BUG_ON(req->part_id == PARTITION_FREELIST);
    dst_part = &cache->user_parts[req->part_id].part;

    // Initialize the LFU iterator starting from lowest frequency bucket (0)
    lfu_iter_eviction_init(&iter, cache, src_part, 0, req);

    // Try to assign 'cline_no' cache lines
    while (i < cline_no) {
        // Select next candidate cache line depending on source partition
        if (src_part->id != PARTITION_FREELIST) {
            cline = lfu_iter_eviction_next(&iter, dst_part, &core_id, &core_line);
        } else {
            cline = lfu_iter_free_next(&iter, dst_part);
        }

        // No more evictable lines found
        if (cline == END_MARKER)
            break;

        // Sanity check: we should not be assigning dirty cache lines (clean eviction)
        ENV_BUG_ON(metadata_test_dirty(cache, cline));

        // Find the next unmapped (LOOKUP_MISS) entry in the request
        while (req_idx + 1 < req->core_line_count &&
               req->map[req_idx].status != LOOKUP_MISS) {
            req_idx++;
        }

        // Final validation: ensure current target is indeed unmappe
        ENV_BUG_ON(req->map[req_idx].status != LOOKUP_MISS);

        // If coming from a user partition (not freelist), perform eviction
        if (src_part->id != PARTITION_FREELIST) {
            // Invalidate the cache line: remove metadata and old mapping
            ocf_lfu_invalidate(cache, cline, core_id, src_part->id);

            // Release the hash bucket lock if we acquired it in the iterator
            _lfu_unlock_hash(&iter, core_id, core_line);
        }

        // Assign cache line to this request
        ocf_map_cache_line(req, req_idx, cline);

        // Mark as remapped (i.e., assigned a cache line by eviction or allocation)
        req->map[req_idx].status = LOOKUP_REMAPPED;

        // Update internal metadata about this request's mapping
        ocf_engine_patch_req_info(cache, req, req_idx);

        // Track that this cache line is locked for writing (needed for write access)
        alock = ocf_cache_line_concurrency(iter.cache);
        ocf_alock_mark_index_locked(alock, req, req_idx, true);
        req->alock_rw = OCF_WRITE;

        // Move to next cache line
        ++req_idx;
        ++i;

        // Consistency check: if all map entries are consumed, we must have fulfilled all lines
        ENV_BUG_ON(req_idx == req->core_line_count && i != cline_no);
    }

    // Return the number of cache lines actually assigned
    return i;
}