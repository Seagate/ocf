/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * <tested_file_path>src/ocf_lfu.c</tested_file_path>
 * <tested_function>_lfu_init</tested_function>
 * <functions_to_leave>
 * ocf_lfu_add
 * ocf_lfu_remove
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

#include "ocf_lfu.c/lfu_generated_wraps.c"

#define META_COUNT 128

static struct ocf_lfu_meta meta[META_COUNT];
static struct ocf_lfu_list freq_buckets[MAX_FREQ];

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

struct ocf_lfu_list *__wrap_ocf_lfu_get_list(struct ocf_part *part, uint32_t freq) 
{
	assert(freq < MAX_FREQ);
	return &freq_buckets[freq];
}

struct ocf_lfu_list *__wrap_lfu_get_cline_list(ocf_cache_t cache,
		ocf_cache_line_t cline)
{
	struct ocf_lfu_meta *meta_item = &meta[cline];
	return &freq_buckets[meta_item->freq];
}	

static const unsigned END_MARKER = -1;

/** Setup */
static int setup_freq_buckets(void **state) {
    for (int i = 0; i < MAX_FREQ; i++) {
        freq_buckets[i].head = END_MARKER;
        freq_buckets[i].tail = END_MARKER;
        freq_buckets[i].num_nodes = 0;
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
		assert_int_equal(freq_buckets[0].num_nodes, i);
		assert_int_equal(freq_buckets[0].head, i);
		assert_int_equal(freq_buckets[0].tail, 1);
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
		assert_int_equal(freq_buckets[0].num_nodes, i);
		assert_int_equal(freq_buckets[0].head, i);
		assert_int_equal(freq_buckets[0].tail, 1);

		ocf_lfu_remove(NULL, i);
	}

	// Check that Freq 0 list is completely empty
	assert_int_equal(freq_buckets[0].num_nodes, 0);
	assert_int_equal(freq_buckets[0].head, END_MARKER);
	assert_int_equal(freq_buckets[0].tail, END_MARKER);
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

    // Try increment 1
    ocf_lfu_increment(NULL, cline);
    assert_int_equal(meta[cline].freq, 1);
	
	// Check if buckets were updated correctly

	// 1st bucket should have things
	assert_int_equal(freq_buckets[1].num_nodes, 1);
	assert_int_equal(freq_buckets[1].head, cline);

	// 0th bucket should be empty
	assert_int_equal(freq_buckets[0].num_nodes, 0);
	assert_int_equal(freq_buckets[0].head, END_MARKER);
	assert_int_equal(freq_buckets[0].tail, END_MARKER);
}

/** Test increment frequency max */
static void _lfu_init_test05(void **state)
{
	ocf_cache_line_t cline = 7;

    // Setup initial metadata
    memset(meta, 0, sizeof(meta));

	print_test_description("lfu: test increment frequency when maxed\n");

	// Add to cache
	meta[cline].freq = MAX_FREQ - 1;
	add_to_freq_bucket(MAX_FREQ - 1, NULL, cline);

    // Try increment 1
    ocf_lfu_increment(NULL, cline);
	
	// Should not increase
	assert_int_equal(meta[cline].freq, MAX_FREQ - 1);
	
	// Check if buckets were updated correctly
	assert_int_equal(freq_buckets[MAX_FREQ - 1].num_nodes, 1);
	assert_int_equal(freq_buckets[MAX_FREQ - 1].head, cline);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(_lfu_init_test01, setup_freq_buckets),
		cmocka_unit_test_setup(_lfu_init_test02, setup_freq_buckets),
		cmocka_unit_test_setup(_lfu_init_test03, setup_freq_buckets),
		cmocka_unit_test_setup(_lfu_init_test04, setup_freq_buckets),
		cmocka_unit_test_setup(_lfu_init_test05, setup_freq_buckets)
	};

	print_message("Unit test for lfu\n");

	return cmocka_run_group_tests(tests, NULL, NULL);
}