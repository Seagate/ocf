/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * <tested_file_path>src/eviction/ocf_lfu.c</tested_file_path>
 * <tested_function>lfu_iter_eviction_next</tested_function>
 * <functions_to_leave>
 * lfu_iter_init
 * lfu_iter_cleaning_init
 * lfu_iter_eviction_init
 * lfu_iter_eviction_next
 * lfu_iter_cleaning_next
 * add_to_list
 * add_to_freq_bucket
 * remove_from_list
 * remove_from_freq_bucket
 * </functions_to_leave>
 */

#undef static

#undef inline


#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "print_desc.h"

#include "../ocf_space.h"
#include "ocf_lfu.h"
#include "../utils/utils_cleaner.h"
#include "../utils/utils_cache_line.h"
#include "../utils/utils_generator.h"
#include "../utils/utils_parallelize.h"
#include "../concurrency/ocf_concurrency.h"
#include "../mngt/ocf_mngt_common.h"
#include "../engine/engine_zero.h"
#include "ocf_cache_priv.h"
#include "ocf_request.h"
#include "../engine/engine_common.h"

#include "eviction/ocf_lfu.c/lfu_iter_generated_wraps.c"

/** DATA STRUCTURES & CONSTANTS */

// Container for LFU metadata
#define META_COUNT OCF_LFU_STRIPE_SIZE * 2

static struct ocf_lfu_meta meta[META_COUNT];

// Frequency bucket lists
static struct ocf_lfu_list freq_bucket_lists[LFU_NUM_SHARDS][LFU_MAX_FREQ];

// Expected results
#define MAX_EXPECTED META_COUNT
struct lfu_expected_entry {
    ocf_cache_line_t cline;
    uint32_t shard;
    uint32_t freq;
};

static struct lfu_expected_entry expected[MAX_EXPECTED];
static size_t expected_count;

// Sentinel / end marker
static const ocf_cache_line_t END_MARKER = OCF_CACHE_LINE_INVALID;

/** WRAPPERS */

struct ocf_cache_line_concurrency *__wrap_ocf_cache_line_concurrency(ocf_cache_t cache)
{
	return NULL;
}

struct ocf_lfu_meta *__wrap_ocf_metadata_get_lfu(ocf_cache_t cache, ocf_cache_line_t line)
{
	assert (line < META_COUNT);
	return &meta[line];
}

struct ocf_lfu_list *__wrap_ocf_lfu_get_list(struct ocf_part *part, uint32_t shard_idx, uint32_t freq, bool clean) 
{
	assert(freq < LFU_MAX_FREQ);
	return &freq_bucket_lists[shard_idx][freq];
}

struct ocf_part *__wrap_lfu_get_cline_part(ocf_cache_t cache,
                                                  ocf_cache_line_t cline)
{ 
    return NULL;
}

bool __wrap_ocf_cache_line_try_lock_rd(struct ocf_cache_line_concurrency *c,
		ocf_cache_line_t line)
{
	return true;
}

bool __wrap_ocf_cache_line_try_lock_wr(struct ocf_cache_line_concurrency *c,
		ocf_cache_line_t line)
{
	return false;
}

bool __wrap__lfu_iter_eviction_lock(struct ocf_lfu_iter *iter,
                                           ocf_cache_line_t cache_line,
                                           ocf_core_id_t *core_id,
                                           uint64_t *core_line)
{
	return true;
}

bool __wrap_metadata_test_dirty(struct ocf_cache *cache,
		ocf_cache_line_t line)
{
	return !(&meta[line])->clean;
}

/** Helpers */
static void expected_reset()
{
    expected_count = 0;
}

static void expected_push(ocf_cache_line_t cline, uint32_t shard, uint32_t freq)
{
    assert_true(expected_count < MAX_EXPECTED);
    expected[expected_count].cline = cline;
    expected[expected_count].shard = shard;
    expected[expected_count].freq = freq;
    expected_count++;
}

static inline ocf_cache_line_t test_cline(uint32_t shard, uint32_t ordinal)
{
	uint32_t stripe = ordinal / LFU_CHUNK_SIZE;
    uint32_t pos = ordinal % LFU_CHUNK_SIZE;

    return ((stripe * LFU_NUM_SHARDS) + shard) * LFU_CHUNK_SIZE + pos;
}

static inline uint32_t test_shard(ocf_cache_line_t cline)
{
	return OCF_LFU_GET_SHARD_INDEX(cline);
}

static void insert_line(uint32_t shard, uint32_t freq, uint32_t pos, bool clean)
{
	ocf_cache_line_t cline = test_cline(shard, pos);

	assert_true(cline < META_COUNT);
	meta[cline].freq = freq;
	meta[cline].clean = clean;

	add_to_freq_bucket(freq, NULL, cline, clean);
}

static void remove_line(uint32_t shard, uint32_t freq, uint32_t pos, bool clean)
{
	ocf_cache_line_t cline = test_cline(shard, pos);
	remove_from_freq_bucket(freq, NULL, cline, clean);
}

static int reset_test_state() {
    // Reset expected array
    expected_reset();

    // Reset metadata
    memset(meta, 0, sizeof(meta));

    // Initialize clean bucket lists
	for (int i = 0; i < LFU_NUM_SHARDS; i++) {
			for (int j = 0; j < LFU_MAX_FREQ; j++) {
			freq_bucket_lists[i][j].head = END_MARKER;
			freq_bucket_lists[i][j].tail = END_MARKER;
			freq_bucket_lists[i][j].num_nodes = 0;
		}
	}
    return 0; 
}

static void assert_expected_sequence(struct ocf_lfu_iter *iter)
{
    ocf_cache_line_t cline;
    size_t i;

    for (i = 0; i < expected_count; i++) {
        cline = lfu_iter_eviction_next(iter, NULL, NULL, NULL);

        assert_int_equal(cline, expected[i].cline);
        assert_int_equal(iter->current_shard, expected[i].shard);
        assert_int_equal(iter->current_freq, expected[i].freq);

        if(cline != END_MARKER) {
            remove_from_freq_bucket(expected[i].freq, NULL, cline, (&meta[cline])->clean);
        }
    }

    cline = lfu_iter_eviction_next(iter, NULL, NULL, NULL);
    assert_int_equal(cline, END_MARKER);
}

/** WRAPPER FOR RUNNING TESTS */
typedef uint32_t (*lfu_num_elements_fn)(uint32_t freq);

static void _run_lfu_eviction_test(
        const char *desc,
        lfu_num_elements_fn num_elements_fn)
{
    struct ocf_lfu_iter iter;
    uint32_t start_shard, shard, freq, s, j;
    uint32_t base_ordinal[LFU_MAX_FREQ];
    uint32_t next_ordinal;

    print_test_description(desc);

    for (start_shard = 0; start_shard < LFU_NUM_SHARDS; start_shard++) {
        reset_test_state();

        /* Populate all buckets */
        for (shard = 0; shard < LFU_NUM_SHARDS; shard++) {
            next_ordinal = 0;

            for (freq = 0; freq < LFU_MAX_FREQ; freq++) {
                uint32_t num_elements = num_elements_fn(freq);
                base_ordinal[freq] = next_ordinal;
                next_ordinal += num_elements;
                
                for (j = 0; j < num_elements; j++) {
                    ocf_cache_line_t cline =
                        test_cline(shard, base_ordinal[freq] + j);

                    meta[cline].freq = freq;
                    meta[cline].clean = true;
                    add_to_freq_bucket(freq, NULL, cline, true);
                }
            }
        }

        /*
         * Expected order across repeated calls:
         * for each freq from low to high,
         *   for each bucket position j,
         *     visit shards in rotated order
         */
        for (freq = 0; freq < LFU_MAX_FREQ; freq++) {
            uint32_t num_elements = num_elements_fn(freq);

            for (j = 0; j < num_elements; j++) {
                for (s = 0; s < LFU_NUM_SHARDS; s++) {
                    shard = (start_shard + 1 + s) % LFU_NUM_SHARDS;

                    /* recompute base for this shard */
                    uint32_t base = 0;
                    for (uint32_t f2 = 0; f2 < freq; f2++)
                        base += num_elements_fn(f2);

                    expected_push(
                        test_cline(shard, base + j),
                        shard, freq);
                }
            }
        }

        lfu_iter_eviction_init(&iter, NULL, NULL, start_shard, NULL);
        assert_expected_sequence(&iter);
    }
}


/** EVICTION ITERATOR */

// case 1 - all lists empty
static uint32_t num_elements_case01(uint32_t freq)
{
    return 0;
}

static void lfu_iter_eviction_next_test01(void **state)
{
    _run_lfu_eviction_test("case 1 - all lists empty", num_elements_case01);
}

// case 2 - all shards, all lists with single element
static uint32_t num_elements_case02(uint32_t freq)
{
    return 1;
}

static void lfu_iter_eviction_next_test02(void **state)
{
    _run_lfu_eviction_test("case 2 - all shards, all lists with single element", num_elements_case02);
    
}

// case 3 - all shards, all lists have between 1 and 5 elements, increasingly
static uint32_t num_elements_case03(uint32_t freq)
{
    return 1 + freq / (LFU_MAX_FREQ / 4);
}

static void lfu_iter_eviction_next_test03(void **state)
{
    _run_lfu_eviction_test("case 3 - all shards, all lists have between 1 and 5 elements, increasingly", num_elements_case03);
}

// case 4 - all shards, all lists have between 1 and 5 elements, modulo index
static uint32_t num_elements_case04(uint32_t freq)
{
    return 1 + (freq % 5);
}

static void lfu_iter_eviction_next_test04(void **state)
{
    _run_lfu_eviction_test("case 4 - all shards, all lists have between 1 and 5 elements, modulo index", num_elements_case04);
}

// case 5 - all shards, all lists have between 0 and 4 elements, increasingly
static uint32_t num_elements_case05(uint32_t freq)
{
    return  freq / (LFU_MAX_FREQ / 4);
}

static void lfu_iter_eviction_next_test05(void **state)
{
	_run_lfu_eviction_test("case 5 - all shards, all lists have between 0 and 4 elements, increasingly", num_elements_case05);
}

// case 6 - all shards, all lists have between 0 and 4 elements, modulo index
static uint32_t num_elements_case06(uint32_t freq)
{
    return freq % 5;
}

static void lfu_iter_eviction_next_test06(void **state)
{
	_run_lfu_eviction_test("case 6 - all shards, all lists have between 0 and 4 elements, modulo index", num_elements_case06);
}

// case 7 - list length increasing by 1 from 0
static uint32_t num_elements_case07(uint32_t freq)
{
    return freq;
}

static void lfu_iter_eviction_next_test07(void **state)
{
	_run_lfu_eviction_test("case 7 - list length increasing by 1 from 0", num_elements_case07);
}

// case 8 - list length increasing by 4 from 0
static uint32_t num_elements_case08(uint32_t freq)
{
    return freq * 4;
}

static void lfu_iter_eviction_next_test08(void **state)
{
    _run_lfu_eviction_test("case 8 - list length increasing by 4 from 0", num_elements_case08);
}

// case 9 - list length increasing by 4 from 1
static uint32_t num_elements_case09(uint32_t freq)
{
    return freq * 4 + 1;
}

static void lfu_iter_eviction_next_test09(void **state)
{
    _run_lfu_eviction_test("case 9 - list length increasing by 4 from 1", num_elements_case09);
}


int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(lfu_iter_eviction_next_test01),
		cmocka_unit_test(lfu_iter_eviction_next_test02),
		cmocka_unit_test(lfu_iter_eviction_next_test03),
		cmocka_unit_test(lfu_iter_eviction_next_test04),
		cmocka_unit_test(lfu_iter_eviction_next_test05),
		cmocka_unit_test(lfu_iter_eviction_next_test06),
		cmocka_unit_test(lfu_iter_eviction_next_test07),
		cmocka_unit_test(lfu_iter_eviction_next_test08),
		cmocka_unit_test(lfu_iter_eviction_next_test09),
	};

	print_message("Unit test for lfu iterators\n");

	return cmocka_run_group_tests(tests, NULL, NULL);
}