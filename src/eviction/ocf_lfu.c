// ocf_lfu.c - Minimal LFU implementation

#include "ocf_lfu.h"
#include "../utils/utils_cleaner.h"
#include "../utils/utils_cache_line.h"
#include "../utils/utils_generator.h"
#include "../utils/utils_parallelize.h"
#include "../concurrency/ocf_concurrency.h"
#include "../mngt/ocf_mngt_common.h"
#include "../engine/engine_zero.h"
#include "../ocf_cache_priv.h"
#include "../ocf_request.h"
#include "../engine/engine_common.h"
#include "../utils/utils_user_part.h"

// DEBUG PROFILING
#ifndef OCF_LFU_DEBUG_PROFILE
#define OCF_LFU_DEBUG_PROFILE 0
#endif

#if OCF_LFU_DEBUG_PROFILE
#define OCF_LFU_DEBUG_PROFILE_DUMP_EVERY_INC (1ULL << 17)
#define OCF_LFU_DEBUG_PROFILE_DUMP_EVERY_REQ 64ULL

struct ocf_lfu_debug_profile_stats
{
    env_atomic64 inc_calls;
    env_atomic64 inc_bucket_change_calls;
    env_atomic64 inc_bucket_same_calls;
    env_atomic64 inc_bucket_same_total_ns;
    env_atomic64 inc_total_ns;
    env_atomic64 inc_lock_wait_ns;
    env_atomic64 inc_body_ns;

    env_atomic64 req_calls;
    env_atomic64 req_clines_requested;
    env_atomic64 req_clines_assigned;
    env_atomic64 req_total_ns;

    env_atomic64 dirty_calls;
    env_atomic64 dirty_total_ns;
    env_atomic64 dirty_lock_wait_ns;
    env_atomic64 dirty_body_ns;

    env_atomic64 clean_calls;
    env_atomic64 clean_total_ns;
    env_atomic64 clean_lock_wait_ns;
    env_atomic64 clean_body_ns;

    env_atomic64 add_calls;
    env_atomic64 remove_calls;
    env_atomic64 repart_calls;
    env_atomic64 rm_cline_calls;

    /* iterator / search cost */
    env_atomic64 evict_calls;
    env_atomic64 evict_total_ns;
    env_atomic64 evict_bucket_lock_wait_ns;
    env_atomic64 evict_buckets_scanned;
    env_atomic64 evict_clines_scanned;
    env_atomic64 evict_found;

    env_atomic64 free_calls;
    env_atomic64 free_total_ns;
    env_atomic64 free_bucket_lock_wait_ns;
    env_atomic64 free_buckets_scanned;
    env_atomic64 free_clines_scanned;
    env_atomic64 free_found;

    /* trylock failure reasons */
    env_atomic64 evict_trylock_fail_cacheline;
    env_atomic64 evict_trylock_fail_self;
    env_atomic64 evict_trylock_fail_hash;
    env_atomic64 evict_trylock_fail_waiters;

    /* frequency distributions */
    env_atomic64 dirty_freq_calls[LFU_MAX_FREQ];
    env_atomic64 clean_freq_calls[LFU_MAX_FREQ];
    env_atomic64 evict_found_freq[LFU_MAX_FREQ];
    env_atomic64 free_found_freq[LFU_MAX_FREQ];
};

static struct ocf_lfu_debug_profile_stats ocf_lfu_prof_stats;

static inline u64 ocf_lfu_prof_now_ns(void)
{
    /* both Linux env and POSIX env expose this */
    return env_get_tick_count();
}

static inline u64 ocf_lfu_prof_delta_ns(u64 start_ns, u64 end_ns)
{
    return env_ticks_to_nsecs(end_ns - start_ns);
}

static inline u64 ocf_lfu_prof_avg(u64 total, u64 cnt)
{
    return cnt ? (total / cnt) : 0;
}

static inline void ocf_lfu_prof_dump_named_freq_array(ocf_cache_t cache, const char *tag,
                                                      const char *name, env_atomic64 arr[])
{
    unsigned i;

    for (i = 0; i < LFU_MAX_FREQ; i += 8)
    {
        ocf_cache_log(cache, log_info,
                      "lfu-prof[%s]: %s[%u-%u]=%" ENV_PRIu64 ",%" ENV_PRIu64
                      ",%" ENV_PRIu64 ",%" ENV_PRIu64 ",%" ENV_PRIu64
                      ",%" ENV_PRIu64 ",%" ENV_PRIu64 ",%" ENV_PRIu64 "\n",
                      tag, name,
                      i, i + 7,
                      env_atomic64_read(&arr[i + 0]),
                      env_atomic64_read(&arr[i + 1]),
                      env_atomic64_read(&arr[i + 2]),
                      env_atomic64_read(&arr[i + 3]),
                      env_atomic64_read(&arr[i + 4]),
                      env_atomic64_read(&arr[i + 5]),
                      env_atomic64_read(&arr[i + 6]),
                      env_atomic64_read(&arr[i + 7]));
    }
}

static void ocf_lfu_prof_dump(ocf_cache_t cache, const char *reason)
{
    u64 inc_calls = env_atomic64_read(&ocf_lfu_prof_stats.inc_calls);
    u64 inc_bucket_change_calls =
        env_atomic64_read(&ocf_lfu_prof_stats.inc_bucket_change_calls);
    u64 inc_bucket_same_calls =
        env_atomic64_read(&ocf_lfu_prof_stats.inc_bucket_same_calls);
    u64 inc_bucket_same_total_ns = env_atomic64_read(&ocf_lfu_prof_stats.inc_bucket_same_total_ns);
    u64 inc_total_ns = env_atomic64_read(&ocf_lfu_prof_stats.inc_total_ns);
    u64 inc_lock_wait_ns =
        env_atomic64_read(&ocf_lfu_prof_stats.inc_lock_wait_ns);
    u64 inc_body_ns = env_atomic64_read(&ocf_lfu_prof_stats.inc_body_ns);

    u64 req_calls = env_atomic64_read(&ocf_lfu_prof_stats.req_calls);
    u64 req_clines_requested =
        env_atomic64_read(&ocf_lfu_prof_stats.req_clines_requested);
    u64 req_clines_assigned =
        env_atomic64_read(&ocf_lfu_prof_stats.req_clines_assigned);
    u64 req_total_ns = env_atomic64_read(&ocf_lfu_prof_stats.req_total_ns);

    u64 dirty_calls = env_atomic64_read(&ocf_lfu_prof_stats.dirty_calls);
    u64 dirty_total_ns = env_atomic64_read(&ocf_lfu_prof_stats.dirty_total_ns);
    u64 dirty_lock_wait_ns = env_atomic64_read(&ocf_lfu_prof_stats.dirty_lock_wait_ns);
    u64 dirty_body_ns = env_atomic64_read(&ocf_lfu_prof_stats.dirty_body_ns);

    u64 clean_calls = env_atomic64_read(&ocf_lfu_prof_stats.clean_calls);
    u64 clean_total_ns = env_atomic64_read(&ocf_lfu_prof_stats.clean_total_ns);
    u64 clean_lock_wait_ns = env_atomic64_read(&ocf_lfu_prof_stats.clean_lock_wait_ns);
    u64 clean_body_ns = env_atomic64_read(&ocf_lfu_prof_stats.clean_body_ns);

    u64 add_calls = env_atomic64_read(&ocf_lfu_prof_stats.add_calls);
    u64 remove_calls = env_atomic64_read(&ocf_lfu_prof_stats.remove_calls);
    u64 repart_calls = env_atomic64_read(&ocf_lfu_prof_stats.repart_calls);
    u64 rm_cline_calls = env_atomic64_read(&ocf_lfu_prof_stats.rm_cline_calls);

    u64 avg_inc_ns = ocf_lfu_prof_avg(inc_total_ns, inc_calls);
    u64 avg_inc_bucket_same_ns = ocf_lfu_prof_avg(inc_bucket_same_total_ns, inc_bucket_same_calls);
    u64 avg_inc_lock_wait_ns = ocf_lfu_prof_avg(inc_lock_wait_ns, inc_bucket_change_calls);
    u64 avg_inc_body_ns = ocf_lfu_prof_avg(inc_body_ns, inc_bucket_change_calls);

    u64 avg_req_ns = ocf_lfu_prof_avg(req_total_ns, req_calls);

    u64 evict_calls = env_atomic64_read(&ocf_lfu_prof_stats.evict_calls);
    u64 evict_total_ns =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_total_ns);
    u64 evict_bucket_lock_wait_ns =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_bucket_lock_wait_ns);
    u64 evict_buckets_scanned =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_buckets_scanned);
    u64 evict_clines_scanned =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_clines_scanned);
    u64 evict_found = env_atomic64_read(&ocf_lfu_prof_stats.evict_found);

    u64 free_calls = env_atomic64_read(&ocf_lfu_prof_stats.free_calls);
    u64 free_total_ns =
        env_atomic64_read(&ocf_lfu_prof_stats.free_total_ns);
    u64 free_bucket_lock_wait_ns =
        env_atomic64_read(&ocf_lfu_prof_stats.free_bucket_lock_wait_ns);
    u64 free_buckets_scanned =
        env_atomic64_read(&ocf_lfu_prof_stats.free_buckets_scanned);
    u64 free_clines_scanned =
        env_atomic64_read(&ocf_lfu_prof_stats.free_clines_scanned);
    u64 free_found = env_atomic64_read(&ocf_lfu_prof_stats.free_found);

    u64 fail_cacheline =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_trylock_fail_cacheline);
    u64 fail_self =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_trylock_fail_self);
    u64 fail_hash =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_trylock_fail_hash);
    u64 fail_waiters =
        env_atomic64_read(&ocf_lfu_prof_stats.evict_trylock_fail_waiters);

    u64 avg_dirty_ns = ocf_lfu_prof_avg(dirty_total_ns, dirty_calls);
    u64 avg_dirty_lock_wait_ns =
        ocf_lfu_prof_avg(dirty_lock_wait_ns, dirty_calls);
    u64 avg_dirty_body_ns =
        ocf_lfu_prof_avg(dirty_body_ns, dirty_calls);

    u64 avg_clean_ns = ocf_lfu_prof_avg(clean_total_ns, clean_calls);
    u64 avg_clean_lock_wait_ns =
        ocf_lfu_prof_avg(clean_lock_wait_ns, clean_calls);
    u64 avg_clean_body_ns =
        ocf_lfu_prof_avg(clean_body_ns, clean_calls);

    u64 avg_evict_ns = ocf_lfu_prof_avg(evict_total_ns, evict_calls);
    u64 avg_evict_lock_wait_ns =
        ocf_lfu_prof_avg(evict_bucket_lock_wait_ns, evict_calls);
    u64 avg_evict_buckets =
        ocf_lfu_prof_avg(evict_buckets_scanned, evict_calls);
    u64 avg_evict_clines =
        ocf_lfu_prof_avg(evict_clines_scanned, evict_calls);

    u64 avg_free_ns = ocf_lfu_prof_avg(free_total_ns, free_calls);
    u64 avg_free_lock_wait_ns =
        ocf_lfu_prof_avg(free_bucket_lock_wait_ns, free_calls);
    u64 avg_free_buckets =
        ocf_lfu_prof_avg(free_buckets_scanned, free_calls);
    u64 avg_free_clines =
        ocf_lfu_prof_avg(free_clines_scanned, free_calls);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: inc=%" ENV_PRIu64
                  " bucket_same=%" ENV_PRIu64 " bucket_change=%" ENV_PRIu64
                  " avg_inc_ns=%" ENV_PRIu64 " avg_inc_bucket_same_ns=%" ENV_PRIu64 " avg_lock_wait_ns=%" ENV_PRIu64
                  " avg_body_ns=%" ENV_PRIu64 "\n",
                  reason,
                  inc_calls,
                  inc_bucket_same_calls,
                  inc_bucket_change_calls,
                  avg_inc_ns,
                  avg_inc_bucket_same_ns,
                  avg_inc_lock_wait_ns,
                  avg_inc_body_ns);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: req=%" ENV_PRIu64 " req_clines=%" ENV_PRIu64
                  "/%" ENV_PRIu64 " avg_req_ns=%" ENV_PRIu64 "\n",
                  reason,
                  req_calls,
                  req_clines_assigned,
                  req_clines_requested,
                  avg_req_ns);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: dirty=%" ENV_PRIu64
                  " avg_dirty_ns=%" ENV_PRIu64
                  " avg_dirty_lock_wait_ns=%" ENV_PRIu64
                  " avg_dirty_body_ns=%" ENV_PRIu64
                  " clean=%" ENV_PRIu64
                  " avg_clean_ns=%" ENV_PRIu64
                  " avg_clean_lock_wait_ns=%" ENV_PRIu64
                  " avg_clean_body_ns=%" ENV_PRIu64 "\n",
                  reason,
                  dirty_calls,
                  avg_dirty_ns,
                  avg_dirty_lock_wait_ns,
                  avg_dirty_body_ns,
                  clean_calls,
                  avg_clean_ns,
                  avg_clean_lock_wait_ns,
                  avg_clean_body_ns);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: evict=%" ENV_PRIu64
                  " found=%" ENV_PRIu64
                  " avg_evict_ns=%" ENV_PRIu64
                  " avg_evict_lock_wait_ns=%" ENV_PRIu64
                  " avg_buckets_scanned=%" ENV_PRIu64
                  " avg_clines_scanned=%" ENV_PRIu64 "\n",
                  reason,
                  evict_calls,
                  evict_found,
                  avg_evict_ns,
                  avg_evict_lock_wait_ns,
                  avg_evict_buckets,
                  avg_evict_clines);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: free=%" ENV_PRIu64
                  " found=%" ENV_PRIu64
                  " avg_free_ns=%" ENV_PRIu64
                  " avg_free_lock_wait_ns=%" ENV_PRIu64
                  " avg_buckets_scanned=%" ENV_PRIu64
                  " avg_clines_scanned=%" ENV_PRIu64 "\n",
                  reason,
                  free_calls,
                  free_found,
                  avg_free_ns,
                  avg_free_lock_wait_ns,
                  avg_free_buckets,
                  avg_free_clines);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: trylock_fail cacheline=%" ENV_PRIu64
                  " self=%" ENV_PRIu64
                  " hash=%" ENV_PRIu64
                  " waiters=%" ENV_PRIu64 "\n",
                  reason,
                  fail_cacheline,
                  fail_self,
                  fail_hash,
                  fail_waiters);

    ocf_cache_log(cache, log_info,
                  "lfu-prof[%s]: add=%" ENV_PRIu64 " remove=%" ENV_PRIu64
                  " repart=%" ENV_PRIu64 " rm_cline=%" ENV_PRIu64 "\n",
                  reason,
                  add_calls,
                  remove_calls,
                  repart_calls,
                  rm_cline_calls);

    ocf_lfu_prof_dump_named_freq_array(cache, reason, "dirty_freq_calls",
                                       ocf_lfu_prof_stats.dirty_freq_calls);
    ocf_lfu_prof_dump_named_freq_array(cache, reason, "clean_freq_calls",
                                       ocf_lfu_prof_stats.clean_freq_calls);
    ocf_lfu_prof_dump_named_freq_array(cache, reason, "evict_found_freq",
                                       ocf_lfu_prof_stats.evict_found_freq);
    ocf_lfu_prof_dump_named_freq_array(cache, reason, "free_found_freq",
                                       ocf_lfu_prof_stats.free_found_freq);
}

static inline void ocf_lfu_prof_maybe_dump_inc(ocf_cache_t cache, u64 inc_call_no)
{
    if ((inc_call_no & (OCF_LFU_DEBUG_PROFILE_DUMP_EVERY_INC - 1)) == 0)
        ocf_lfu_prof_dump(cache, "inc");
}

static inline void ocf_lfu_prof_maybe_dump_req(ocf_cache_t cache, u64 req_call_no)
{
    if (req_call_no &&
        (req_call_no % OCF_LFU_DEBUG_PROFILE_DUMP_EVERY_REQ) == 0)
        ocf_lfu_prof_dump(cache, "req");
}
#else
static inline u64 ocf_lfu_prof_now_ns(void) { return 0; }
static inline u64 ocf_lfu_prof_delta_ns(u64 start_ns, u64 end_ns) { return 0; }
static inline void ocf_lfu_prof_dump(ocf_cache_t cache, const char *reason) {}
static inline void ocf_lfu_prof_maybe_dump_inc(ocf_cache_t cache, u64 inc_call_no) {}
static inline void ocf_lfu_prof_maybe_dump_req(ocf_cache_t cache, u64 req_call_no) {}
#endif

static const ocf_cache_line_t END_MARKER = OCF_CACHE_LINE_INVALID;

/* Helper to get the partition eviction runtime information */
static inline struct ocf_lfu_part_runtime *ocf_part_lfu(struct ocf_part *part)
{
    return (struct ocf_lfu_part_runtime *)(part->eviction_runtime);
}

/** Get frequency bucket from cache partition */
static inline struct ocf_lfu_list *ocf_lfu_get_list(struct ocf_part *part, uint32_t shard_idx, uint32_t freq, bool clean)
{
    struct ocf_lfu_part_runtime *rt = ocf_part_lfu(part);

    ENV_BUG_ON(!rt);

    // ENV_BUG_ON(freq >= LFU_MAX_FREQ);
    if (part->id == PARTITION_FREELIST)
        clean = true;

    return clean ? &rt->freq_buckets[shard_idx][freq].clean : &rt->freq_buckets[shard_idx][freq].dirty;
}

/* Helper to increment hits to a certain cap */
static inline ocf_lfu_hits_t ocf_lfu_increment_hits(ocf_lfu_hits_t hits)
{
    return (hits == (ocf_lfu_hits_t)-1) ? (ocf_lfu_hits_t)-1 : (ocf_lfu_hits_t)(hits + 1);
}

/* Helper to convert raw hit count into frequency buckets */
static inline uint32_t ocf_lfu_hits_to_freq(ocf_lfu_hits_t hits)
{
    uint32_t bucket = 0;

    if (hits <= 1)
        return 0;

    hits >>= 1;

    while (hits)
    {
        bucket++;
        hits >>= 1;
    }

    return (bucket < (LFU_MAX_FREQ - 1)) ? bucket : (LFU_MAX_FREQ - 1);
}

/** Get cache partition that cache line belongs to */
static inline struct ocf_part *lfu_get_cline_part(ocf_cache_t cache,
                                                  ocf_cache_line_t cline)
{
    ocf_part_id_t part_id;
    struct ocf_part *part;

    part_id = ocf_metadata_get_partition_id(cache, cline);

    // ocf_cache_log(cache, log_debug, "%d\n", part_id);
    // ENV_BUG_ON(part_id > OCF_USER_IO_CLASS_MAX);
    if (part_id == PARTITION_FREELIST)
    {
        part = &cache->free;
    }
    else
    {
        part = &cache->user_parts[part_id].part;
    }

    return part;
}

/** Initialize cacheline LFU metadata */
void ocf_lfu_init_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    meta->freq = 0;
    meta->prev = END_MARKER;
    meta->next = END_MARKER;
    meta->clean = true;
    meta->hits = 0;
}

static inline void _lfu_init(struct ocf_lfu_list *list)
{
    list->num_nodes = 0;
    list->head = END_MARKER;
    list->tail = END_MARKER;
}

/*
 * Allocate and initialize LRU runtime for a single partition.
 * Call this from your eviction-policy init path for every partition.
 */
int ocf_lfu_init_part(ocf_cache_t cache, struct ocf_part *part)
{
    struct ocf_lfu_part_runtime *rt;

    rt = env_vzalloc(sizeof(*rt));
    if (!rt)
        return -OCF_ERR_NO_MEM;

    part->eviction_runtime = rt;

    ocf_lfu_init(cache, part);

    ocf_cache_log(cache, log_info, "LFU initialized for part %u", part->id);

    return 0;
}

/*
 * Free per-part LRU runtime.
 * Call this from your eviction-policy deinit / switch path.
 */
void ocf_lfu_deinit_part(ocf_cache_t cache, struct ocf_part *part)
{
    if (!part->eviction_runtime)
        return;

    env_vfree(part->eviction_runtime);
    part->eviction_runtime = NULL;
}

void ocf_lfu_init(ocf_cache_t cache, struct ocf_part *part)
{
    uint32_t i, j;
    struct ocf_lfu_list *clean_list;
    struct ocf_lfu_list *dirty_list;

    for (i = 0; i < LFU_NUM_SHARDS; i++)
    {
        for (j = 0; j < LFU_MAX_FREQ; j++)
        {
            clean_list = ocf_lfu_get_list(part, i, j, true);
            dirty_list = ocf_lfu_get_list(part, i, j, false);

            _lfu_init(clean_list);
            _lfu_init(dirty_list);
        }
    }

    // Reset the global count of cache lines in this partition
    env_atomic_set(&part->runtime->curr_size, 0);
    env_atomic_set(&part->runtime->evict_counter, 0);

    ocf_cache_log(cache, log_info, "LFU initialized!");
}

static void add_to_list(struct ocf_lfu_list *list, ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

    // ocf_cache_log(cache, log_debug, "[add_to_list] before | req: cline=%u meta_freq=%u list_nodes=%u\n",
    //               cline, meta->freq, list->num_nodes);

    meta->prev = END_MARKER;
    meta->next = list->head;

    if (list->head != END_MARKER)
    {
        struct ocf_lfu_meta *head_meta = ocf_metadata_get_lfu(cache, list->head);
        head_meta->prev = cline;
    }
    list->head = cline;

    if (list->tail == END_MARKER)
        list->tail = cline;

    list->num_nodes++;

    // ocf_cache_log(cache, log_debug, "[add_to_list] after | req: cline=%u meta_freq=%u list_nodes=%u\n",
    //               cline, meta->freq, list->num_nodes);
}

/** Add cache line to frequency bucket list head 
 * Caller must hold write locks
*/
static inline void add_to_freq_bucket(uint32_t freq, ocf_cache_t cache, ocf_cache_line_t cline, bool clean)
{
    struct ocf_part *part = lfu_get_cline_part(cache, cline);
    struct ocf_lfu_list *list = ocf_lfu_get_list(part, OCF_LFU_GET_SHARD_INDEX(cline), freq, clean);

    // bool clean_current = !metadata_test_dirty(cache, cline);
    // ocf_cache_log(cache, log_debug, "[add_to_freq_bucket] req: cline=%u part=%p freq_arg=%u clean_arg=%d clean=%d meta_clean=%d list_nodes=%u head=%u tail=%u\n",
    //               cline, part, freq, clean, clean_current, ocf_metadata_get_lfu(cache, cline)->clean, list->num_nodes, list->head, list->tail);

    add_to_list(list, cache, cline);
}

/** Remove cache line from a list */
static void remove_from_list(struct ocf_lfu_list *list, ocf_cache_t cache, ocf_cache_line_t cline)
{

    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

    ENV_BUG_ON(list->num_nodes == 0);

    if (meta->prev == END_MARKER)
        ENV_BUG_ON(list->head != cline);
    else
        ENV_BUG_ON(ocf_metadata_get_lfu(cache, meta->prev)->next != cline);

    if (meta->next == END_MARKER)
        ENV_BUG_ON(list->tail != cline);
    else
        ENV_BUG_ON(ocf_metadata_get_lfu(cache, meta->next)->prev != cline);

    // ocf_cache_log(cache, log_debug, "[remove_from_list] before | req: cline=%u meta_freq=%u list_nodes=%u\n",
    //               cline, meta->freq, list->num_nodes);

    if (meta->prev != END_MARKER)
    {
        struct ocf_lfu_meta *prev_meta = ocf_metadata_get_lfu(cache, meta->prev);
        prev_meta->next = meta->next;
    }
    else
    {
        list->head = meta->next;
    }

    if (meta->next != END_MARKER)
    {
        struct ocf_lfu_meta *next_meta = ocf_metadata_get_lfu(cache, meta->next);
        next_meta->prev = meta->prev;
    }
    else
    {
        list->tail = meta->prev;
    }

    meta->prev = END_MARKER;
    meta->next = END_MARKER;

    list->num_nodes--;

    // ocf_cache_log(cache, log_debug, "[remove_from_list] after | req: cline=%u meta_freq=%u list_nodes=%u\n",
    //               cline, meta->freq, list->num_nodes);
}

/** Remove cache line from its current freq bucket 
 * Caller must hold write locks
*/
static inline void remove_from_freq_bucket(uint32_t freq, ocf_cache_t cache, ocf_cache_line_t cline, bool clean)
{
    struct ocf_part *part = lfu_get_cline_part(cache, cline);
    struct ocf_lfu_list *list = ocf_lfu_get_list(part, OCF_LFU_GET_SHARD_INDEX(cline), freq, clean);

    // bool clean_current = !metadata_test_dirty(cache, cline);
    // ocf_cache_log(cache, log_debug, "[remove_from_freq_bucket] req: cline=%u part=%p freq_arg=%u clean_arg=%d clean=%d meta_clean=%d list_nodes=%u head=%u tail=%u\n",
    //               cline, part, freq, clean, clean_current, ocf_metadata_get_lfu(cache, cline)->clean, list->num_nodes, list->head, list->tail);

    ENV_BUG_ON(list->num_nodes == 0);

    remove_from_list(list, cache, cline);
}

/** Called when a cache line is accessed, increment frequency */
void ocf_lfu_increment(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    uint32_t old_freq, new_freq;
    ocf_lfu_hits_t old_hits, new_hits;
    uint32_t shard_idx;

#if OCF_LFU_DEBUG_PROFILE
    u64 prof_call_no;
    u64 t0, t1, t2, t3;
    t0 = env_get_tick_count();
#endif

#if OCF_LFU_DEBUG_PROFILE
    prof_call_no = (u64)env_atomic64_inc_return(&ocf_lfu_prof_stats.inc_calls);
#endif

    shard_idx = OCF_LFU_GET_SHARD_INDEX(cline);

    old_hits = meta->hits;
    new_hits = ocf_lfu_increment_hits(old_hits);

    old_freq = meta->freq;
    new_freq = ocf_lfu_hits_to_freq(new_hits);

    /**
     * If the bucket is the same, no need to relink
     */
    if (likely(new_freq == old_freq))
    {
        meta->hits = new_hits;
#if OCF_LFU_DEBUG_PROFILE
        env_atomic64_inc(&ocf_lfu_prof_stats.inc_bucket_same_calls);
        t2 = env_get_tick_count();
        env_atomic64_add(t2 - t0,
                         &ocf_lfu_prof_stats.inc_bucket_same_total_ns);
        env_atomic64_add(t2 - t0,
                         &ocf_lfu_prof_stats.inc_total_ns);
        ocf_lfu_prof_maybe_dump_inc(cache, prof_call_no);
#endif
        return;
    }

#if OCF_LFU_DEBUG_PROFILE
    env_atomic64_inc(&ocf_lfu_prof_stats.inc_bucket_change_calls);
#endif

    ocf_metadata_lfu_lock(&cache->metadata.lock, shard_idx);

#if OCF_LFU_DEBUG_PROFILE
    t1 = env_get_tick_count();
#endif

    ocf_lfu_remove(cache, cline);
    meta->hits = new_hits;
    meta->freq = new_freq;
    add_to_freq_bucket(meta->freq, cache, cline, meta->clean);

#if OCF_LFU_DEBUG_PROFILE
    t2 = env_get_tick_count();
#endif

    ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);

#if OCF_LFU_DEBUG_PROFILE
    t3 = env_get_tick_count();
    env_atomic64_add(t3 - t0, &ocf_lfu_prof_stats.inc_total_ns);
    env_atomic64_add(t1 - t0, &ocf_lfu_prof_stats.inc_lock_wait_ns);
    env_atomic64_add(t2 - t1, &ocf_lfu_prof_stats.inc_body_ns);
    ocf_lfu_prof_maybe_dump_inc(cache, prof_call_no);
#endif
}

/** Add a new cache line to freq bucket 0 on insertion 
 * Caller must hold write locks
*/
void ocf_lfu_add(ocf_cache_t cache, ocf_cache_line_t cline)
{
#if OCF_LFU_DEBUG_PROFILE
    env_atomic64_inc(&ocf_lfu_prof_stats.add_calls);
#endif
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    meta->hits = 1;
    meta->freq = 0;
    meta->clean = !metadata_test_dirty(cache, cline);
    add_to_freq_bucket(meta->freq, cache, cline, meta->clean);
}

/** Remove a cache line completely from freq buckets (on eviction) */
void ocf_lfu_remove(ocf_cache_t cache, ocf_cache_line_t cline)
{
#if OCF_LFU_DEBUG_PROFILE
    env_atomic64_inc(&ocf_lfu_prof_stats.remove_calls);
#endif
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    remove_from_freq_bucket(meta->freq, cache, cline, meta->clean);
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
    metadata_clear_valid_sec(cache, cline, 0, ocf_line_end_block(cache));

    // Step 3: Remove cache line from collision table and from LFU frequency bucket
    ocf_metadata_remove_from_collision(cache, cline, part_id);

    // Step 4: End shared access
    ocf_metadata_end_collision_shared_access(cache, cline);

    // Step 5: Update runtime stats for the core and the partition
    core = ocf_cache_get_core(cache, core_id);
    env_atomic_dec(&core->runtime_meta->cached_clines);
    env_atomic_dec(&core->runtime_meta->part_counters[part_id].cached_clines);
}

/** Try to get hash bucket lock */
static inline bool _lfu_trylock_hash(struct ocf_lfu_iter *iter,
                                     ocf_core_id_t core_id, uint64_t core_line)
{
    if (iter->hash_locked != NULL && iter->hash_locked(
                                         iter->req, core_id, core_line))
    {
        return true;
    }

    return ocf_hb_cline_naked_trylock_wr(
        &iter->cache->metadata.lock,
        core_id, core_line);
}

/** Check if hash bucket was locked by the caller and unlock if needed (utility function) */
static inline void _lfu_unlock_hash(struct ocf_lfu_iter *iter,
                                    ocf_core_id_t core_id, uint64_t core_line)
{
    if (iter->hash_locked != NULL && iter->hash_locked(
                                         iter->req, core_id, core_line))
    {
        return;
    }

    ocf_hb_cline_naked_unlock_wr(
        &iter->cache->metadata.lock,
        core_id, core_line);
}

/** Initialize LFU iterator (utility funtion) */
static inline void lfu_iter_init(struct ocf_lfu_iter *iter,
                                 ocf_cache_t cache,
                                 struct ocf_part *part,
                                 uint32_t start_shard,
                                 bool clean,
                                 _lru_hash_locked_pfn hash_locked,
                                 struct ocf_request *req)
{
    iter->cache = cache;
    iter->c = ocf_cache_line_concurrency(cache);
    iter->part = part;
    iter->current_shard = start_shard;
    iter->current_freq = 0;
    iter->clean = clean;
    iter->last_cline = END_MARKER;
    iter->hash_locked = hash_locked;
    iter->req = req;
}


/** Initialize LFU eviction iterator */
static inline void lfu_iter_eviction_init(struct ocf_lfu_iter *iter,
                                          ocf_cache_t cache,
                                          struct ocf_part *part,
                                          uint32_t start_shard,
                                          struct ocf_request *req)
{
    // Eviction typically scans clean lines first
    lfu_iter_init(iter, cache, part, start_shard, true, ocf_req_hash_in_range, req);
}

/** See if you can aquire cacheline lock for eviction */
static inline bool _lfu_iter_eviction_lock(struct ocf_lfu_iter *iter,
                                           ocf_cache_line_t cache_line,
                                           ocf_core_id_t *core_id,
                                           uint64_t *core_line)
{

    struct ocf_request *req = iter->req;

    // If cannot evict current tail
    if (!ocf_cache_line_try_lock_wr(iter->c, cache_line))
    {
#if OCF_LFU_DEBUG_PROFILE
        env_atomic64_inc(
            &ocf_lfu_prof_stats.evict_trylock_fail_cacheline);
#endif
        return false;
    }

    ocf_metadata_get_core_info(iter->cache, cache_line, core_id, core_line);

    // Avoid evicting current request target cacheline
    if (*core_id == ocf_core_get_id(req->core) &&
        *core_line >= req->core_line_first &&
        *core_line <= req->core_line_last)
    {
#if OCF_LFU_DEBUG_PROFILE
        env_atomic64_inc(&ocf_lfu_prof_stats.evict_trylock_fail_self);
#endif
        // Release lock on cache line
        ocf_cache_line_unlock_wr(iter->c, cache_line);

        return false;
    }

    // If cannot acquire hash lock
    if (!_lfu_trylock_hash(iter, *core_id, *core_line))
    {
#if OCF_LFU_DEBUG_PROFILE
        env_atomic64_inc(&ocf_lfu_prof_stats.evict_trylock_fail_hash);
#endif
        // Release lock on cache line
        ocf_cache_line_unlock_wr(iter->c, cache_line);

        return false;
    }

    // If there are waiters on the waiting list
    if (ocf_cache_line_are_waiters(iter->c, cache_line))
    {
#if OCF_LFU_DEBUG_PROFILE
        env_atomic64_inc(&ocf_lfu_prof_stats.evict_trylock_fail_waiters);
#endif
        _lfu_unlock_hash(iter, *core_id, *core_line);
        ocf_cache_line_unlock_wr(iter->c, cache_line);
        return false;
    }

    // If all these locks can be acquired, return true
    return true;
}

/** Move cache line to another cache partition
 * Caller must acquire shard locks
 */
static void ocf_lfu_repart_locked(ocf_cache_t cache, ocf_cache_line_t cline,
                                  struct ocf_part *src, struct ocf_part *dst)
{
    struct ocf_lfu_list *src_list, *dst_list;
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

    // ocf_cache_log(cache, log_debug, "[ocf_lfu_repart_locked]");

    ENV_BUG_ON(src == dst);

    bool clean = meta->clean;
    uint32_t shard_idx = OCF_LFU_GET_SHARD_INDEX(cline);
    src_list = ocf_lfu_get_list(src, shard_idx, meta->freq, clean);
    dst_list = ocf_lfu_get_list(dst, shard_idx, meta->freq, clean);

    ENV_BUG_ON(src_list->num_nodes == 0);
    if (src_list->head == END_MARKER)
        ENV_BUG_ON(src_list->tail != END_MARKER);
    if (src_list->tail == END_MARKER)
        ENV_BUG_ON(src_list->head != END_MARKER);

    // Step 1: Remove from source list
    remove_from_list(src_list, cache, cline);

    // Step 2: Insert into destination list at head
    add_to_list(dst_list, cache, cline);

    // Step 3: Update partition metadata
    ocf_metadata_set_partition_id(cache, cline, dst->id);
    env_atomic_dec(&src->runtime->curr_size);
    env_atomic_inc(&dst->runtime->curr_size);
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
    uint32_t f;
    uint32_t shard_idx;
    uint32_t shards_scanned = 0;

#if OCF_LFU_DEBUG_PROFILE
    uint64_t t0, t1 = 0, t2;
    uint64_t lists_scanned = 0;
    uint64_t clines_scanned = 0;

    env_atomic64_inc(&ocf_lfu_prof_stats.evict_calls);
    t0 = env_get_tick_count();
#endif

    // Starting from the lowest frequency
    do
    {
        iter->current_shard = (iter->current_shard + 1) % LFU_NUM_SHARDS;
        shard_idx = iter->current_shard;

        shards_scanned++;

        // Acquire shard lock
        ocf_metadata_lfu_lock(&cache->metadata.lock, shard_idx);
        
#if OCF_LFU_DEBUG_PROFILE
        if (shards_scanned == 1)
            t1 = env_get_tick_count();
#endif

        // Look through all the freqs
        for (f = 0; f < LFU_MAX_FREQ; f++)
        {
#if OCF_LFU_DEBUG_PROFILE
            lists_scanned++;
#endif
            // Update iter->current_freq even though it's not useful for this one
            iter->current_freq = f;

            bucket = ocf_lfu_get_list(part, shard_idx, f, iter->clean);

            // Get the tail (LRU of the bucket)
            cline = bucket->tail;

            // If the list is empty / none of the cache lines can be evicted, move on to the higher freq bucket
            while (cline != END_MARKER && !_lfu_iter_eviction_lock(iter, cline, core_id, core_line))
            {
#if OCF_LFU_DEBUG_PROFILE
                clines_scanned++;
#endif
                // Get the next oldest entry in the bucket
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
            }

            // If we can acquire an eviction candidate
            if (cline != END_MARKER)
            {
#if OCF_LFU_DEBUG_PROFILE
                clines_scanned++;
                env_atomic64_inc(&ocf_lfu_prof_stats.evict_found);
                env_atomic64_inc(
                    &ocf_lfu_prof_stats.evict_found_freq[f]);
#endif
                // Move to target partition at bucket 0
                if (dst_part != part)
                {
                    ocf_lfu_repart_locked(cache, cline, part, dst_part);

                    ocf_lfu_remove(cache, cline);
                    ocf_lfu_add(cache, cline);
                }

                ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);
                goto out;
            }
        }

        // Release shard lock
        ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);
        
    } while (cline == END_MARKER && shards_scanned < LFU_NUM_SHARDS);

out:
#if OCF_LFU_DEBUG_PROFILE
    t2 = env_get_tick_count();
    env_atomic64_add(t2 - t0, &ocf_lfu_prof_stats.evict_total_ns);
    if (t1 > t0)
        env_atomic64_add(t1 - t0,
                         &ocf_lfu_prof_stats.evict_bucket_lock_wait_ns);
    env_atomic64_add(lists_scanned,
                     &ocf_lfu_prof_stats.evict_buckets_scanned);
    env_atomic64_add(clines_scanned,
                     &ocf_lfu_prof_stats.evict_clines_scanned);
#endif

    return cline;
}

/** Move cache line to another cache partition without changing frequency, wrapped with locks */
void ocf_lfu_repart(ocf_cache_t cache, ocf_cache_line_t cline,
                    struct ocf_part *src_part, struct ocf_part *dst_part)
{
    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    uint32_t freq = meta->freq;
    uint32_t shard_idx = OCF_LFU_GET_SHARD_INDEX(cline);

#if OCF_LFU_DEBUG_PROFILE
    env_atomic64_inc(&ocf_lfu_prof_stats.repart_calls);
#endif

    ocf_metadata_lfu_lock(&cache->metadata.lock, shard_idx);

    /* revalidate after taking lock */
    ENV_BUG_ON(meta->freq != freq);
    ENV_BUG_ON(lfu_get_cline_part(cache, cline) != src_part);

    ocf_lfu_repart_locked(cache, cline, src_part, dst_part);

    ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);
}

/**
 * Remap cacheline
 * Caller must hold metadata lock
 */
void ocf_lfu_rm_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_part_id_t part_id = ocf_metadata_get_partition_id(cache, cline);
    struct ocf_part *part = &cache->user_parts[part_id].part;

    ocf_lfu_repart(cache, cline, part, &cache->free);
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
    uint32_t f, shard_idx, shards_scanned = 0;

#if OCF_LFU_DEBUG_PROFILE
    uint64_t t0, t1 = 0, t2;
    uint64_t lists_scanned = 0;
    uint64_t clines_scanned = 0;

    env_atomic64_inc(&ocf_lfu_prof_stats.free_calls);
    t0 = env_get_tick_count();
#endif

    // Sanity check: cannot repartition to the same freelist
    ENV_BUG_ON(dst_part == free);

    do
    {
        iter->current_shard = (iter->current_shard + 1) % LFU_NUM_SHARDS;
        shard_idx = iter->current_shard;

        shards_scanned++;

        // Acquire shard lock
        ocf_metadata_lfu_lock(&cache->metadata.lock, shard_idx);

#if OCF_LFU_DEBUG_PROFILE
        if (shards_scanned == 1)
            t1 = env_get_tick_count();
#endif

        // Look through all the buckets in the shard
        for (f = 0; f < LFU_MAX_FREQ; f++)
        {
            // Update iter->current_freq even though it's not useful for this one
            iter->current_freq = f;

#if OCF_LFU_DEBUG_PROFILE
            lists_scanned++;
#endif

            // Get the current bucket
            bucket = ocf_lfu_get_list(free, shard_idx, f, true);
            cline = bucket->tail;

            // ocf_cache_log(cache, log_debug, "[lfu_iter_free_next] cline: %u", cline);

            // Iterate over bucket from tail
            while (cline != END_MARKER && !ocf_cache_line_try_lock_wr(
                                              iter->c, cline))
            {
#if OCF_LFU_DEBUG_PROFILE
                clines_scanned++;
#endif
                // Go to next older entry
                cline = ocf_metadata_get_lfu(cache, cline)->prev;
            }

            if (cline != END_MARKER)
            {
#if OCF_LFU_DEBUG_PROFILE
                clines_scanned++;
                env_atomic64_inc(&ocf_lfu_prof_stats.free_found);
                env_atomic64_inc(
                    &ocf_lfu_prof_stats.free_found_freq[f]);
#endif
                // Move cacheline from free partition to destination at bucket 0
                ocf_lfu_repart_locked(cache, cline, free, dst_part);
                ocf_lfu_remove(cache, cline);
                ocf_lfu_add(cache, cline);
                
                // Release lock
                ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);

                goto out;
            }
        }

        // Release shard lock
        ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);

    } while (cline == END_MARKER && shards_scanned < LFU_NUM_SHARDS);

out:
#if OCF_LFU_DEBUG_PROFILE
    t2 = env_get_tick_count();
    env_atomic64_add(t2 - t0, &ocf_lfu_prof_stats.free_total_ns);
    if (t1 > t0)
        env_atomic64_add(t1 - t0,
                         &ocf_lfu_prof_stats.free_bucket_lock_wait_ns);
    env_atomic64_add(lists_scanned,
                     &ocf_lfu_prof_stats.free_buckets_scanned);
    env_atomic64_add(clines_scanned,
                     &ocf_lfu_prof_stats.free_clines_scanned);
#endif

    return cline;
}

/**
 * Get next clean cacheline from the lowest frequency bucket of the current shard.
 * Caller must not hold any shard lock.
 * - the function doesn't return cache lines from freelist
 * - returned cacheline is write locked
 * - returned cacheline has the corresponding metadata hash bucket write locked
 * - cacheline is moved to the head of destination partition shard before
 *   being returned.
 * All this is packed into a single function to lock shard once per each
 * replaced cacheline.
*/
static inline ocf_cache_line_t lfu_req_next_cline(struct ocf_request *req,
		ocf_cache_t cache, struct ocf_lfu_iter *iter,
		ocf_cache_line_t cline, struct ocf_part *dst_part,
		ocf_core_id_t *core_id, uint64_t *core_line,
		ocf_part_id_t *src_part_id)
{
    uint32_t current_shard = OCF_LFU_GET_SHARD_INDEX(cline);
    struct ocf_alock *c = iter->c;
    struct ocf_part *part;
    ocf_cache_line_t ret = END_MARKER;
    ocf_core_id_t t_core_id;
    uint64_t t_core_line;
    ocf_part_id_t tmp_part_id;
    struct ocf_lfu_meta *meta;
    uint32_t f;

    // Lock the current shard
    ocf_metadata_lfu_lock(&cache->metadata.lock, current_shard);

    // Get partition ID of current cache line
    tmp_part_id = ocf_metadata_get_partition_id(cache, cline);

    if(tmp_part_id == PARTITION_FREELIST)
        goto lfu_unlock; // Skip if it's from freelist

    if(!cache->user_parts[tmp_part_id].config->flags.eviction)
        goto lfu_unlock; // Skip if it belongs to a priority partition

    if(!ocf_cache_line_try_lock_wr(c, cline)) 
        goto lfu_unlock; // Skip if you cannot acquire cline write lock

    ocf_metadata_get_core_info(cache, cline, &t_core_id, &t_core_line);
	if ((t_core_id == ocf_core_get_id(req->core))
		&& (t_core_line >= req->core_line_first)
		&& (t_core_line <= req->core_line_last))
		goto line_unlock_wr; 

    meta = ocf_metadata_get_lfu(cache, cline);

	if (metadata_test_dirty(cache, cline) || !meta->clean)
		goto line_unlock_wr; // Skip if cache line is dirty

    *src_part_id = tmp_part_id;
	part = &cache->user_parts[*src_part_id].part;

    // TODO: How to make this more efficient?
    if (meta->freq != 0) {
        for(f = 0; f < meta->freq; f++) {
            if (ocf_lfu_get_list(part, current_shard, f, meta->clean)->num_nodes != 0)
                goto line_unlock_wr; // Check if it's LFU in that shard
        }
    }

	if (!_lfu_trylock_hash(iter, t_core_id, t_core_line))
		goto line_unlock_wr; // Skip if you can't take hash bucket lock

    *core_id = t_core_id;
	*core_line = t_core_line;

	if (dst_part->id != *src_part_id) {
		ocf_lfu_repart_locked(cache, cline, part, dst_part);
        ocf_lfu_remove(cache, cline);
        ocf_lfu_add(cache, cline);
	} else {
        ocf_lfu_remove(cache, cline);

        meta->hits = ocf_lfu_increment_hits(meta->hits);
        meta->freq = ocf_lfu_hits_to_freq(meta->hits);
        
        add_to_freq_bucket(meta->freq, cache, cline, meta->clean);
	}

	iter->current_shard = (iter->current_shard + 1) % LFU_NUM_SHARDS;
	ret = cline;

line_unlock_wr:
	if (ret == END_MARKER)
		ocf_cache_line_unlock_wr(c, cline);
lfu_unlock:
	ocf_metadata_lfu_unlock(&cache->metadata.lock, current_shard);
	return ret;
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
    struct ocf_alock *alock;
    struct ocf_lfu_iter iter;
    uint32_t i = 0;
    ocf_cache_line_t cline = END_MARKER;
    uint64_t core_line;
    ocf_core_id_t core_id;
    ocf_cache_t cache = req->cache;
    unsigned req_idx = 0;
    unsigned shard_idx = 0;
    struct ocf_part *dst_part;
    ocf_part_id_t actual_src_part_id;
    uint32_t dst_max_size;

#if OCF_LFU_DEBUG_PROFILE
    u64 req_call_no;
    u64 req_t0;
#endif

    // Return early if nothing is requested
    if (cline_no == 0)
        return 0;

#if OCF_LFU_DEBUG_PROFILE
    req_call_no = (u64)env_atomic64_inc_return(&ocf_lfu_prof_stats.req_calls);
    env_atomic64_add(cline_no, &ocf_lfu_prof_stats.req_clines_requested);
    req_t0 = env_get_tick_count();
#endif

    dst_max_size = ocf_user_part_get_max_size(cache,
                                              &cache->user_parts[req->part_id]);

    // Safety check: ensure request has enough unmapped lines for assignment
    if (unlikely(ocf_engine_unmapped_count(req) < cline_no))
    {
        ocf_cache_log(cache, log_err, "Not enough space in request: unmapped %u, requested %u",
                      ocf_engine_unmapped_count(req), cline_no);
        ENV_BUG();
    }

    // Target partition (where lines will be assigned) must not be the freelist
    ENV_BUG_ON(req->part_id == PARTITION_FREELIST);
    dst_part = &cache->user_parts[req->part_id].part;

    // Initialize the LFU iterator from shard index
    shard_idx = req->io_queue->lru_idx++ % LFU_NUM_SHARDS;
    lfu_iter_eviction_init(&iter, cache, src_part, shard_idx, req);

    // Try to assign 'cline_no' cache lines
    while (i < cline_no)
    {
        // ocf_cache_log(cache, log_debug, "[ocf_lfu_req_clines] Iter freq before advancing: %u",
        //               iter.current_freq);

        // Select next candidate cache line depending on source partition
        if (src_part->id != PARTITION_FREELIST)
        {
            /*
             * Try to evict contiguous cache lines:
			 * Check if there is a sequential successor line.
			 * We know that end_marker > number of cache lines,
			 * so this is also an end_marker check.
			 */
			if (cline < cache->conf_meta->cachelines - 1) {
				cline = lfu_req_next_cline(req, cache, &iter,
						cline + 1, dst_part,
						&core_id, &core_line,
						&actual_src_part_id);
				if (cline != END_MARKER)
					goto got_cline;
			}

            actual_src_part_id = src_part->id;
            cline = lfu_iter_eviction_next(&iter, dst_part, &core_id, &core_line);
        }
        else
        {
            actual_src_part_id = src_part->id;
            cline = lfu_iter_free_next(&iter, dst_part);
        }

        // No more evictable lines found
        if (cline == END_MARKER)
            break;

got_cline:
        // Sanity check: we should not be assigning dirty cache lines (clean eviction)
        ENV_BUG_ON(metadata_test_dirty(cache, cline));

        // Find the next unmapped (LOOKUP_MISS) entry in the request
        while (req_idx + 1 < req->core_line_count &&
               req->map[req_idx].status != LOOKUP_MISS)
        {
            req_idx++;
        }

        // Final validation: ensure current target is indeed unmappe
        ENV_BUG_ON(req->map[req_idx].status != LOOKUP_MISS);

        // If coming from a user partition (not freelist), perform eviction
        if (actual_src_part_id != PARTITION_FREELIST)
        {
            // Invalidate the cache line: remove metadata and old mapping
            ocf_lfu_invalidate(cache, cline, core_id, actual_src_part_id);

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

        /*
         * When allocating from freelist or another partition (not
         * self-eviction), curr_size was incremented. Stop if the
         * destination partition has reached its occupancy limit.
         * This bounds the TOCTOU race in ocf_user_part_has_space()
         * to at most LFU_NUM_SHARDS cache lines of overshoot.
         */
        if (actual_src_part_id != dst_part->id &&
            (uint32_t)env_atomic_read(
                &dst_part->runtime->curr_size) >=
                dst_max_size)
        {
            break;
        }

        // Consistency check: number of cachelines to evict have to match space in the request
        ENV_BUG_ON(req_idx == req->core_line_count && i != cline_no);
    }

#if OCF_LFU_DEBUG_PROFILE
    env_atomic64_add(i, &ocf_lfu_prof_stats.req_clines_assigned);
    env_atomic64_add(env_get_tick_count() - req_t0, &ocf_lfu_prof_stats.req_total_ns);
    ocf_lfu_prof_maybe_dump_req(cache, req_call_no);
#endif

    // Return the number of cache lines actually assigned
    return i;
}

/**
 * Populate functions
 */

struct ocf_lfu_populate_context
{
    ocf_cache_t cache;
    env_atomic curr_size;

    ocf_eviction_populate_end_t cmpl;
    void *priv;
};

/** LFU Populate
 * Function called inside each worker shard */
static int ocf_lfu_populate_handle(ocf_parallelize_t parallelize,
                                   void *priv, unsigned shard_id, unsigned shards_cnt)
{
    struct ocf_lfu_populate_context *context = priv;
    ocf_cache_t cache = context->cache;
    ocf_cache_line_t cnt, cline;
    ocf_cache_line_t entries = ocf_metadata_collision_table_entries(cache);
    struct ocf_generator_bisect_state generator;
    struct ocf_lfu_list *list;
    unsigned step = 0;
    uint32_t num_full_chunks = entries / OCF_LFU_STRIPE_SIZE;
    uint32_t remainder = entries % OCF_LFU_STRIPE_SIZE;
    uint32_t partial_chunk_lines = 0;
	uint32_t num_chunks, chunk_idx, chunk_lines;
    uint32_t ci, j;

    /* Check if this shard has a partial last chunk */
	if (remainder > (uint32_t)shard_id * LFU_CHUNK_SIZE) {
		partial_chunk_lines = remainder -
				(uint32_t)shard_id * LFU_CHUNK_SIZE;
		if (partial_chunk_lines > LFU_CHUNK_SIZE)
			partial_chunk_lines = LFU_CHUNK_SIZE;
	}

	num_chunks = num_full_chunks + (partial_chunk_lines > 0 ? 1 : 0);
	if (num_chunks == 0)
		return 0;

	ocf_generator_bisect_init(&generator, num_chunks, 0);

    list = ocf_lfu_get_list(&cache->free, shard_id, 0, true);

    cnt = 0;
    for (ci = 0; ci < num_chunks; ci++)
    {
        chunk_idx = ocf_generator_bisect_next(&generator);

		if (chunk_idx == num_full_chunks)
			chunk_lines = partial_chunk_lines;
		else
			chunk_lines = LFU_CHUNK_SIZE;

        for(j = 0; j < chunk_lines; j++) {
            OCF_COND_RESCHED_DEFAULT(step);

            uint32_t stripe = chunk_idx * LFU_NUM_SHARDS + shard_id;
            cline = stripe * LFU_CHUNK_SIZE + j;

            ENV_BUG_ON(cline >= entries);
            
            // Put into freelist
            ocf_metadata_set_partition_id(cache, cline, PARTITION_FREELIST);

            // Reset cline metadata
            ocf_lfu_init_cline(cache, cline);
            // ocf_metadata_get_lfu(cache, cline)->freq = freq;

            // Add to the list
            add_to_list(list, cache, cline);

            cnt++;
        }
    }

    env_atomic_add(cnt, &context->curr_size);

    return 0;
}

/** LFU Populate
 * Finish callback */
static void ocf_lfu_populate_finish(ocf_parallelize_t parallelize,
                                    void *priv, int error)
{
    struct ocf_lfu_populate_context *context = priv;

    env_atomic_set(&context->cache->free.runtime->curr_size,
                   env_atomic_read(&context->curr_size));

    context->cmpl(context->priv, error);

    ocf_parallelize_destroy(parallelize);
}

/** LFU Populate
 * put invalid cachelines on freelist partition lru list  */
void ocf_lfu_populate(ocf_cache_t cache,
                      ocf_eviction_populate_end_t cmpl, void *priv)
{
    struct ocf_lfu_populate_context *context;
    ocf_parallelize_t parallelize;
    int result;

    result = ocf_parallelize_create(&parallelize, cache, LFU_NUM_SHARDS,
                                    sizeof(*context), ocf_lfu_populate_handle,
                                    ocf_lfu_populate_finish, false);
    if (result)
    {
        cmpl(priv, result);
        return;
    }

    context = ocf_parallelize_get_priv(parallelize);
    context->cache = cache;
    env_atomic_set(&context->curr_size, 0);
    context->cmpl = cmpl;
    context->priv = priv;

    ocf_parallelize_run(parallelize);
}

void ocf_lfu_detach(ocf_cache_t cache, struct ocf_part *part,
                    ocf_cache_line_t cline)
{
    ocf_lfu_repart(cache, cline, part, &cache->free_detached);
}

void ocf_lfu_reattach(ocf_cache_t cache, ocf_cache_line_t cline)
{
    ocf_lfu_repart(cache, cline, &cache->free_detached, &cache->free);
}

/** Functionality copied over from LRU */
// static bool _is_cache_line_acting(struct ocf_cache *cache,
//                                   uint32_t cache_line, ocf_core_id_t core_id,
//                                   uint64_t start_line, uint64_t end_line)
// {
//     ocf_core_id_t tmp_core_id;
//     uint64_t core_line;

//     ocf_metadata_get_core_info(cache, cache_line,
//                                &tmp_core_id, &core_line);

//     if (core_id != OCF_CORE_ID_INVALID)
//     {
//         if (core_id != tmp_core_id)
//             return false;

//         if (core_line < start_line || core_line > end_line)
//             return false;
//     }
//     else if (tmp_core_id == OCF_CORE_ID_INVALID)
//     {
//         return false;
//     }

//     return true;
// }

// /*
//  * Iterates over cache lines that belong to the core device with
//  * core ID = core_id  whose core byte addresses are in the range
//  * [start_byte, end_byte] and applies actor(cache, cache_line) to all
//  * matching cache lines
//  *
//  * set partition_id to PARTITION_UNSPECIFIED to not care about partition_id
//  *
//  * global metadata write lock must be held before calling this function
//  */
int ocf_lfu_metadata_actor(struct ocf_cache *cache,
                           ocf_part_id_t part_id, ocf_core_id_t core_id,
                           uint64_t start_byte, uint64_t end_byte,
                           ocf_metadata_actor_t actor)
{
    uint32_t step = 0;
    uint64_t start_line, end_line;
    int ret = 0;
    struct ocf_alock *c = ocf_cache_line_concurrency(cache);
    int clean;
    struct ocf_lfu_list *list;
    struct ocf_part *part;
    unsigned i, j, cline;
    struct ocf_lfu_meta *node;

    start_line = ocf_bytes_2_lines(cache, start_byte);
    end_line = ocf_bytes_2_lines(cache, end_byte);

    if (part_id == PARTITION_UNSPECIFIED)
    {
        for (cline = 0; cline < cache->device->collision_table_entries;
             ++cline)
        {
            if (_is_cache_line_acting(cache, cline, core_id,
                                      start_line, end_line))
            {
                if (ocf_cache_line_is_used(c, cline))
                    ret = -OCF_ERR_AGAIN;
                else
                    actor(cache, cline);
            }

            OCF_COND_RESCHED_DEFAULT(step);
        }
        return ret;
    }

    ENV_BUG_ON(part_id == PARTITION_FREELIST);
    part = &cache->user_parts[part_id].part;

    for (i = 0; i < LFU_NUM_SHARDS; i++)
    {
        for (j = 0; j < LFU_MAX_FREQ; j++)
        {
            for (clean = 0; clean <= 1; clean++)
            {
                list = ocf_lfu_get_list(part, i, j, clean);

                cline = list->tail;
                while (cline != END_MARKER)
                {
                    node = ocf_metadata_get_lfu(cache, cline);
                    if (!_is_cache_line_acting(cache, cline,
                                               core_id, start_line,
                                               end_line))
                    {
                        cline = node->prev;
                        continue;
                    }
                    if (ocf_cache_line_is_used(c, cline))
                        ret = -OCF_ERR_AGAIN;
                    else
                        actor(cache, cline);
                    cline = node->prev;
                    OCF_COND_RESCHED_DEFAULT(step);
                }
            }
        }
    }

    return ret;
}

/**
 * CLEANING
 */

/** Initialize LFU cleaning iterator */
static inline void lfu_iter_cleaning_init(struct ocf_lfu_iter *iter,
                                          ocf_cache_t cache, struct ocf_part *part, uint32_t start_shard)
{
    /* Lock cachelines for read, non-exclusive access */
    lfu_iter_init(iter, cache, part, start_shard, false, NULL, NULL);
}

/* Get next dirty cacheline from tail of lfu lists. Caller must hold all
 * lfu list locks during entire iteration proces. Returned cacheline
 * is read or write locked, depending on iter->write_lock */
static inline ocf_cache_line_t lfu_iter_cleaning_next(struct ocf_lfu_iter *iter)
{
    ocf_cache_line_t cline, next, start;
    struct ocf_lfu_list *list;

    /* Rotating counters */
    uint32_t scanned_shards = 0;
    uint32_t scanned_freqs;

    do {
        scanned_freqs = 0;

        while (scanned_freqs < LFU_MAX_FREQ) {

            list = ocf_lfu_get_list(iter->part,
                                    iter->current_shard,
                                    iter->current_freq,
                                    false); // dirty

            /* Start from last position OR tail */
            if (iter->last_cline != END_MARKER)
                cline = iter->last_cline;
            else
                cline = list->tail;

            start = cline;

            while (cline != END_MARKER) {
                next = ocf_metadata_get_lfu(iter->cache, cline)->prev;

                if (ocf_cache_line_try_lock_rd(iter->c, cline)) {

                    if (!metadata_test_dirty(iter->cache, cline)) {
                        ocf_cache_line_unlock_rd(iter->c, cline);
                    } else {
                        /* Advance cursor */
                        iter->last_cline = next;
                        return cline;
                    }
                }

                cline = next;

                /* Prevent re-scanning same bucket */
                if (cline == start)
                    break;
            }

            /* Bucket exhausted -> reset cursor */
            iter->last_cline = END_MARKER;

            /* Move to next frequency */
            iter->current_freq++;
            if (iter->current_freq == LFU_MAX_FREQ) {
                iter->current_freq = 0;
                break;
            }

            scanned_freqs++;
        }

        /* Move to next shard */
        iter->current_shard =
            (iter->current_shard + 1) % LFU_NUM_SHARDS;

        iter->last_cline = END_MARKER;

        scanned_shards++;

    } while (scanned_shards < LFU_NUM_SHARDS);

    return END_MARKER;
}

static void ocf_lfu_clean_end(void *private_data, int error)
{
    struct ocf_part_cleaning_ctx *ctx = private_data;
    struct flush_data *entries = ctx->entries;
    unsigned i;

    for (i = 0; i < OCF_EVICTION_CLEAN_SIZE; i++)
    {
        if (entries[i].cache_line == END_MARKER)
            break;
        ocf_cache_line_unlock_rd(
            ctx->cache->device->concurrency.cache_line,
            entries[i].cache_line);
    }

    env_atomic_set(&ctx->cleaner_running, 0);
    env_refcnt_dec(&ctx->counter);
}

/** LFU cleaning */
void ocf_lfu_clean(ocf_cache_t cache, struct ocf_user_part *user_part,
                   ocf_queue_t io_queue, uint32_t count)
{
    struct ocf_part_cleaning_ctx *ctx = &user_part->cleaning;
    struct ocf_cleaner_attribs attribs = {
        .lock_cacheline = false,
        .cmpl_context = ctx,
        .cmpl_fn = ocf_lfu_clean_end,
        .io_queue = io_queue};
    struct flush_data *entries = ctx->entries;
    struct ocf_lfu_iter iter;
    unsigned shard_idx;
    unsigned i;
    unsigned lock_idx;

    if (ocf_mngt_cache_is_locked(cache))
        return;

    if (unlikely(!env_refcnt_inc(&ctx->counter)))
    {
        /* cleaner disabled by management operation */
        return;
    }

    if (env_atomic_cmpxchg(&ctx->cleaner_running, 0, 1) != 0)
    {
        /* cleaning already running for this partition */
        env_refcnt_dec(&ctx->counter);
        return;
    }

    ctx->cache = cache;
    shard_idx = io_queue->lru_idx++ % LFU_NUM_SHARDS;

    lock_idx = ocf_metadata_concurrency_next_idx(io_queue);
    ocf_metadata_start_shared_access(&cache->metadata.lock, lock_idx);

    ocf_metadata_lfu_lock_all(&cache->metadata.lock);

    // Gather candidates by frequency (lowest first)
    lfu_iter_cleaning_init(&iter, cache, &user_part->part, shard_idx);
    count = min(count, OCF_EVICTION_CLEAN_SIZE);
    for (i = 0; i < count; i++)
    {
        entries[i].cache_line = lfu_iter_cleaning_next(&iter);
        if (entries[i].cache_line == END_MARKER)
            break;
        ocf_metadata_get_core_info(cache, entries[i].cache_line,
                                   &entries[i].core_id, &entries[i].core_line);
    }

    ocf_metadata_lfu_unlock_all(&cache->metadata.lock);
    ocf_metadata_end_shared_access(&cache->metadata.lock, lock_idx);

    if (i == 0)
    {
        env_atomic_set(&ctx->cleaner_running, 0);
        env_refcnt_dec(&ctx->counter);
        return;
    }

    ocf_cleaner_sort_flush_data(entries, i);
    ocf_cleaner_do_flush_data_async(cache, entries, i, &attribs);
}

/* Mark a cache line as dirty by moving it to the dirty list */
void ocf_lfu_dirty_cline(ocf_cache_t cache, struct ocf_part *part, ocf_cache_line_t cline)
{
    uint32_t shard_idx = OCF_LFU_GET_SHARD_INDEX(cline);

#if OCF_LFU_DEBUG_PROFILE
    uint64_t t0, t1, t2;
    env_atomic64_inc(&ocf_lfu_prof_stats.dirty_calls);
    t0 = env_get_tick_count();
#endif

    // Assert that cache line is currently not dirty
    // ENV_BUG_ON(metadata_test_dirty(cache, cline));

    // QUESTION: Should we increment its frequency?
    ocf_metadata_lfu_lock(&cache->metadata.lock, shard_idx);

    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);
    
    // bool clean_current = !metadata_test_dirty(cache, cline);
    // ocf_cache_log(cache, log_debug, "[ocf_lfu_dirty_cline] req: cline=%u part=%p clean=%d meta_clean=%d\n",
    //               cline, part, clean_current, meta->clean);
                  
#if OCF_LFU_DEBUG_PROFILE
    t1 = env_get_tick_count();
    env_atomic64_inc(&ocf_lfu_prof_stats.dirty_freq_calls[meta->freq]);
#endif

    remove_from_freq_bucket(meta->freq, cache, cline, meta->clean);
    meta->clean = false;
    add_to_freq_bucket(meta->freq, cache, cline, false);

    ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);

#if OCF_LFU_DEBUG_PROFILE
    t2 = env_get_tick_count();
    env_atomic64_add(t2 - t0, &ocf_lfu_prof_stats.dirty_total_ns);
    env_atomic64_add(t1 - t0, &ocf_lfu_prof_stats.dirty_lock_wait_ns);
    env_atomic64_add(t2 - t1, &ocf_lfu_prof_stats.dirty_body_ns);
#endif
}

/* Mark a cache line as clean by moving it to the clean list */
void ocf_lfu_clean_cline(ocf_cache_t cache, struct ocf_part *part, ocf_cache_line_t cline)
{
    uint32_t shard_idx = OCF_LFU_GET_SHARD_INDEX(cline);

#if OCF_LFU_DEBUG_PROFILE
    uint64_t t0, t1, t2;
    env_atomic64_inc(&ocf_lfu_prof_stats.clean_calls);
    t0 = env_get_tick_count();
#endif

    // Assert that cache line is currently dirty
    // ENV_BUG_ON(!metadata_test_dirty(cache, cline));

    // QUESTION: Should we increment its frequency?
    ocf_metadata_lfu_lock(&cache->metadata.lock, shard_idx);

    struct ocf_lfu_meta *meta = ocf_metadata_get_lfu(cache, cline);

#if OCF_LFU_DEBUG_PROFILE
    t1 = env_get_tick_count();
    env_atomic64_inc(&ocf_lfu_prof_stats.clean_freq_calls[meta->freq]);
#endif

    remove_from_freq_bucket(meta->freq, cache, cline, meta->clean);
    meta->clean = true;
    add_to_freq_bucket(meta->freq, cache, cline, true);
    
    ocf_metadata_lfu_unlock(&cache->metadata.lock, shard_idx);

#if OCF_LFU_DEBUG_PROFILE
    t2 = env_get_tick_count();
    env_atomic64_add(t2 - t0, &ocf_lfu_prof_stats.clean_total_ns);
    env_atomic64_add(t1 - t0, &ocf_lfu_prof_stats.clean_lock_wait_ns);
    env_atomic64_add(t2 - t1, &ocf_lfu_prof_stats.clean_body_ns);
#endif
}

/**
 * Functions to restore metadata from flushed data on clean shutdown
 */

/**
 * @brief Reconstruct metadata for a cline from flushed data
 */
static int ocf_lfu_restore_cline(ocf_cache_t cache, ocf_cache_line_t cline)
{
    struct ocf_lfu_meta *node, *next_node = NULL, *prev_node = NULL;
    struct ocf_lfu_list *list;
    struct ocf_part *part;
    ocf_core_id_t core_id;
    uint64_t core_line;
    ocf_part_id_t part_id;
    bool dirty, valid;
    uint32_t shard_idx;

    ocf_metadata_get_core_info(cache, cline, &core_id, &core_line);

    if (!ocf_metadata_check(cache, cline) || core_id > OCF_CORE_NUM)
    {
        // ocf_cache_log(cache, log_err,
        // 	"[ocf_lru_restore_cline] invalid metadata: cline=%u core_id=%u\n",
        // 	cline, core_id);
        return -OCF_ERR_INVAL;
    }

    valid = metadata_test_valid_any(cache, cline);
    node = ocf_metadata_get_lfu(cache, cline);
    shard_idx = OCF_LFU_GET_SHARD_INDEX(cline);

    if (!valid || core_id == OCF_ERR_MAX)
    { // If cline is free, put into free list
        part = &cache->free;
        list = ocf_lfu_get_list(part, shard_idx, node->freq, true);
        env_atomic_inc(&cache->free.runtime->curr_size);

        // ocf_cache_log(cache, log_debug,
        //               "[ocf_lfu_restore_cline] cline=%u prev=%u next=%u freq=%u clean=%d\n",
        //               cline, node->prev, node->next, node->freq, node->clean);
    }
    else
    {
        part_id = ocf_metadata_get_partition_id(cache, cline);

        if (part_id > OCF_USER_IO_CLASS_MAX)
        {
            // ocf_cache_log(cache, log_err, "[ocf_lru_restore_cline]part_id = %u > %d", part_id, OCF_USER_IO_CLASS_MAX);
            return -OCF_ERR_INVAL;
        }

        if (node->freq != ocf_lfu_hits_to_freq(node->hits))
        {
            node->freq = ocf_lfu_hits_to_freq(node->hits);
        }

        dirty = metadata_test_dirty(cache, cline);

        part = &cache->user_parts[part_id].part;
        list = ocf_lfu_get_list(part, shard_idx, node->freq, !dirty);
        env_atomic_inc(&part->runtime->curr_size);
    }

    if (node->prev != END_MARKER)
        prev_node = ocf_metadata_get_lfu(cache, node->prev);

    if (node->next != END_MARKER)
        next_node = ocf_metadata_get_lfu(cache, node->next);

    // Check neighbor consistency
    if (prev_node && prev_node->next != cline)
    {
        // ocf_cache_log(cache, log_err, "[ocf_lfu_restore_cline]prev neighbor is inconsistent: prev_node->next = %u", prev_node->next);
        return -OCF_ERR_INVAL;
    }

    if (next_node && next_node->prev != cline)
    {
        // ocf_cache_log(cache, log_err, "[ocf_lfu_restore_cline]next neighbor is inconsistent: next_node->prev = %u", next_node->prev);
        return -OCF_ERR_INVAL;
    }

    list->num_nodes++;

    if (node->prev == END_MARKER)
    {
        if (list->head != END_MARKER)
        {
            // struct ocf_lru_meta *head_node = ocf_metadata_get_lru(cache, list->head);
            // ocf_cache_log(cache, log_err, "[ocf_lru_restore_cline]node is not head. head = %u", list->head);

            // ocf_cache_log(cache, log_err,
            // 	"[ocf_lru_restore_cline] existing head meta: cline=%u prev=%u next=%u hot=%u\n",
            // 	list->head, head_node->prev, head_node->next, head_node->hot);
            return -OCF_ERR_INVAL;
        }

        list->head = cline;
    }

    if (node->next == END_MARKER)
    {
        if (list->tail != END_MARKER)
        {
            // ocf_cache_log(cache, log_err, "[ocf_lru_restore_cline]node is not tail. tail = %u", list->tail);
            return -OCF_ERR_INVAL;
        }
        list->tail = cline;
    }

    return 0;
}

/**
 * @brief Restore runtime information from flushed data
 */
int ocf_lfu_restore_runtime(ocf_cache_t cache)
{
    ocf_cache_line_t cline;
    ocf_cache_line_t entries = ocf_metadata_collision_table_entries(cache);
    int ret;

    // Reset runtime
    ocf_part_id_t part_id;

    for (part_id = 0; part_id < OCF_USER_IO_CLASS_MAX; part_id++)
        ocf_lfu_init(cache, &cache->user_parts[part_id].part);

    ocf_lfu_init(cache, &cache->free);
    ocf_lfu_init(cache, &cache->free_detached);

    // Restore clines
    for (cline = 0; cline < entries; cline++)
    {
        ret = ocf_lfu_restore_cline(cache, cline);
        if (ret)
            return ret;
    }

    return ret;
}