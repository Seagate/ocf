/*
 * Copyright(c) 2026 Seagate Technology LLC and/or its affiliates
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * <tested_file_path>src/eviction/ocf_lfu.c</tested_file_path>
 * <tested_function>_lfu_init</tested_function>
 * <functions_to_leave>
 * ocf_lfu_increment_hits
 * ocf_lfu_hits_to_freq
 * ocf_lfu_add
 * ocf_lfu_remove
 * add_to_list
 * remove_from_list
 * add_to_freq_bucket
 * remove_from_freq_bucket
 * ocf_lfu_increment
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

#include "eviction/ocf_lfu.c/lfu_generated_wraps.c"

#define META_COUNT 128

static struct ocf_lfu_meta meta[META_COUNT];
static struct ocf_lfu_list freq_bucket_lists[LFU_NUM_SHARDS][LFU_MAX_FREQ];

/** Wrappers */

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
	assert(shard_idx < LFU_NUM_SHARDS);
	return &freq_bucket_lists[shard_idx][freq];
}

bool __wrap_metadata_test_dirty(struct ocf_cache *cache,
		ocf_cache_line_t line)
{
	return 1 - (&meta[line])->clean;
}

static const ocf_cache_line_t END_MARKER = OCF_CACHE_LINE_INVALID;

/** Setup */
static int setup_freq_bucket_lists(void **state) {
	for (int i = 0; i < LFU_NUM_SHARDS; i++) {
			for (int j = 0; j < LFU_MAX_FREQ; j++) {
			freq_bucket_lists[i][j].head = END_MARKER;
			freq_bucket_lists[i][j].tail = END_MARKER;
			freq_bucket_lists[i][j].num_nodes = 0;
		}
	}
    return 0; 
}

/** Test Cases */

/** Test Init */
static void _lfu_init_test01(void **state)
{
    struct ocf_lfu_list l;

    print_test_description("lfu: test init\n");

    _lfu_init(&l);

    // Validate LFU structure is initialized
    assert_int_equal(l.num_nodes, 0);
    assert_int_equal(l.head, END_MARKER);
    assert_int_equal(l.tail, END_MARKER);
}

/** Test insert */
static void _lfu_init_test02(void **state)
{
	unsigned i;

	memset(meta, 0, sizeof(meta));

	print_test_description("lfu: test add\n");

	for (i = 1; i <= 8; i++)
	{
		ocf_lfu_add(NULL, i);

		// Check if added with frequency 0
		struct ocf_lfu_meta *meta_item = __wrap_ocf_metadata_get_lfu(NULL, i);
		assert_int_equal(meta_item->freq, 0);
		
		// Check list structure
		assert_int_equal(freq_bucket_lists[0][0].num_nodes, i);
		assert_int_equal(freq_bucket_lists[0][0].head, i);
		assert_int_equal(freq_bucket_lists[0][0].tail, 1);
	}
}

/** Test remove */
static void _lfu_init_test03(void **state)
{
	memset(meta, 0, sizeof(meta));

	print_test_description("lfu: remove from bucket 0\n");

	// Fill in the cache
	for (int i = 1; i <= 8; i++) {
		ocf_lfu_add(NULL, i);
	}

	// Start popping things out of the cache
	for (int i = 8; i >= 1; i--) {
		// Since we're not increasing frequency, check the 0 bucket only
		assert_int_equal(freq_bucket_lists[0][0].num_nodes, i);
		assert_int_equal(freq_bucket_lists[0][0].head, i);
		assert_int_equal(freq_bucket_lists[0][0].tail, 1);

		ocf_lfu_remove(NULL, i);
	}

	// Check that Freq 0 list is completely empty
	assert_int_equal(freq_bucket_lists[0][0].num_nodes, 0);
	assert_int_equal(freq_bucket_lists[0][0].head, END_MARKER);
	assert_int_equal(freq_bucket_lists[0][0].tail, END_MARKER);
}

/** Test increment frequency */
static void _lfu_init_test04(void **state)
{
	ocf_cache_line_t cline = 7;

    // Setup initial metadata
    memset(meta, 0, sizeof(meta));

	print_test_description("lfu: test increment frequency\n");

	// Add to cache
	ocf_lfu_add(NULL, cline);

	// Increment frequency by 1
	ocf_lfu_increment(NULL, cline);

	// Check if buckets were updated correctly

	// 1st bucket should have things
	assert_int_equal(freq_bucket_lists[0][1].num_nodes, 1);
	assert_int_equal(freq_bucket_lists[0][1].head, cline);

	// 0th bucket should be empty
	assert_int_equal(freq_bucket_lists[0][0].num_nodes, 0);
	assert_int_equal(freq_bucket_lists[0][0].head, END_MARKER);
	assert_int_equal(freq_bucket_lists[0][0].tail, END_MARKER);
}

/** Test increment frequency max */
static void _lfu_init_test05(void **state)
{
	ocf_cache_line_t cline = 7;

    // Setup initial metadata
    memset(meta, 0, sizeof(meta));

	print_test_description("lfu: test increment frequency when maxed\n");

	// Add to cache
	meta[cline].hits = (ocf_lfu_hits_t)-1;
	meta[cline].freq = ocf_lfu_hits_to_freq(meta[cline].hits);

	assert_int_equal(meta[cline].freq, LFU_MAX_FREQ - 1);

	add_to_freq_bucket(LFU_MAX_FREQ - 1, NULL, cline, true);

    // Try increment 1
    ocf_lfu_increment(NULL, cline);
	
	// Should not increase
	assert_int_equal(meta[cline].hits, (ocf_lfu_hits_t)-1);
	assert_int_equal(meta[cline].freq, LFU_MAX_FREQ - 1);
	
	// Check if buckets were updated correctly
	assert_int_equal(freq_bucket_lists[0][LFU_MAX_FREQ - 1].num_nodes, 1);
	// Due to optimization, it doesn't need to be moved to head:
	// assert_int_equal(freq_bucket_lists[LFU_MAX_FREQ - 1].head, cline);
}

static void _lfu_init_test06(void **state)
{
    unsigned i, j;
    unsigned count;

    memset(meta, 0, sizeof(meta));

    print_test_description("lfu: test remove from various buckets\n");

    // Setup: Add 8 elements, then increment their frequencies
    // to distribute them across different buckets.
    // cline 8 -> hits 4 -> freq 2
    // cline 7, 6 -> hits 2 -> freq 1
    // cline 5, 4, 3, 2, 1 -> freq 0
    for (i = 1; i <= 8; i++) {
        ocf_lfu_add(NULL, i);
    }

	for(j = 0; j < 4; j++) {
		ocf_lfu_increment(NULL, 8);
	}

	ocf_lfu_increment(NULL, 7);
	ocf_lfu_increment(NULL, 6);
	
    // Initial state verification
	assert_int_equal(meta[8].freq, 2);
	assert_int_equal(meta[7].freq, 1);
	assert_int_equal(meta[6].freq, 1);

    assert_int_equal(freq_bucket_lists[0][2].num_nodes, 1);
    assert_int_equal(freq_bucket_lists[0][2].head, 8);
    assert_int_equal(freq_bucket_lists[0][1].num_nodes, 2);
    assert_int_equal(freq_bucket_lists[0][1].head, 6);
    assert_int_equal(freq_bucket_lists[0][0].num_nodes, 5);
    assert_int_equal(freq_bucket_lists[0][0].head, 5);

    // Remove from a bucket with multiple items (cline 7 from freq 1)
    ocf_lfu_remove(NULL, 7);
    assert_int_equal(freq_bucket_lists[0][1].num_nodes, 1);
    assert_int_equal(freq_bucket_lists[0][1].head, 6);
    assert_int_equal(freq_bucket_lists[0][1].tail, 6);

    // Remove the last item from a bucket (cline 8 from freq 2)
    ocf_lfu_remove(NULL, 8);
    assert_int_equal(freq_bucket_lists[0][2].num_nodes, 0);
    assert_int_equal(freq_bucket_lists[0][2].head, END_MARKER);

    // Remove from the head of bucket 0
    ocf_lfu_remove(NULL, 5);
    assert_int_equal(freq_bucket_lists[0][0].num_nodes, 4);
    assert_int_equal(freq_bucket_lists[0][0].head, 4);

    // Remove from the tail of bucket 0
    ocf_lfu_remove(NULL, 1);
    assert_int_equal(freq_bucket_lists[0][0].num_nodes, 3);
    assert_int_equal(freq_bucket_lists[0][0].tail, 2);

    // Remove from the middle of bucket 0
    ocf_lfu_remove(NULL, 3);
    assert_int_equal(freq_bucket_lists[0][0].num_nodes, 2);
    assert_int_equal(meta[4].next, 2);
    assert_int_equal(meta[2].prev, 4);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(_lfu_init_test01, setup_freq_bucket_lists),
		cmocka_unit_test_setup(_lfu_init_test02, setup_freq_bucket_lists),
		cmocka_unit_test_setup(_lfu_init_test03, setup_freq_bucket_lists),
		cmocka_unit_test_setup(_lfu_init_test04, setup_freq_bucket_lists),
		cmocka_unit_test_setup(_lfu_init_test05, setup_freq_bucket_lists),
		cmocka_unit_test_setup(_lfu_init_test06, setup_freq_bucket_lists)
	};

	print_message("Unit test for lfu\n");

	return cmocka_run_group_tests(tests, NULL, NULL);
}