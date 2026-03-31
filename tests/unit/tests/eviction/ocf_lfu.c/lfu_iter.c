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

#define META_COUNT 2500

static struct ocf_lfu_meta meta[META_COUNT];
static struct ocf_lfu_list freq_bucket_lists[LFU_MAX_FREQ];

static const unsigned END_MARKER = (uint32_t)-1;

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

struct ocf_lfu_list *__wrap_ocf_lfu_get_list(struct ocf_part *part, uint32_t freq, bool clean) 
{
	assert(freq < LFU_MAX_FREQ);
	return &freq_bucket_lists[freq];
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

/** EVICTION ITERATOR */

// case 0 - all lists empty
static void lfu_iter_eviction_next_test01(void **state)
{
	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	expected_cache_line = END_MARKER;

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		// Assert that you cannot find a cache line
		assert_int_equal(cache_line, expected_cache_line);
	} while (cache_line != END_MARKER);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ);
}

// case 1 - all lists with single element
static void lfu_iter_eviction_next_test02(void **state)
{

	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Insert a single element into each frequency bucket
		ocf_cache_line_t cline = i;
		meta[cline].freq = i;
		add_to_freq_bucket(i, NULL, cline, true);
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	unsigned i = 0;

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);

		expected_cache_line = i;
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, i, true);

		i++;
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 2 - all lists have between 1 and 5 elements, increasingly
static void lfu_iter_eviction_next_test03(void **state)
{

	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = 1 + i / (LFU_MAX_FREQ / 4);

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = i * num_elements + j;
			meta[cline].freq = i;
			add_to_freq_bucket(i, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	unsigned i = 0;
	unsigned j = 0;
	unsigned num_elements = 1 + i / (LFU_MAX_FREQ / 4);

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		expected_cache_line = i * num_elements + j;
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		j++;

		if (j == num_elements) {
			j = 0;
			i++;
			num_elements = 1 + i / (LFU_MAX_FREQ / 4);
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 3 - all lists have between 1 and 5 elements, modulo index
static void lfu_iter_eviction_next_test04(void **state)
{

	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	ocf_cache_line_t cline_counter = 0;
	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = 1 + (i % 5);

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = cline_counter++;
			meta[cline].freq = i;
			add_to_freq_bucket(i, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line = 0;
	unsigned i = 0;
	unsigned j = 0;
	unsigned num_elements = 1 + (i % 5);

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		expected_cache_line++;

		j++;

		if (j == num_elements) {
			j = 0;
			i++;
			num_elements = 1 + (i % 5);
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 4 - all lists have between 0 and 4 elements, increasingly
static void lfu_iter_eviction_next_test05(void **state)
{
	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = i / (LFU_MAX_FREQ / 4);

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = i * num_elements + j;
			meta[cline].freq = i;
			add_to_freq_bucket(i, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	unsigned i = 0;
	unsigned j = 0;
	unsigned num_elements = i / (LFU_MAX_FREQ / 4);

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		if(num_elements == 0) {
			j = 0;
			i++;
			num_elements = i / (LFU_MAX_FREQ / 4);
			continue;
		} 
			
		expected_cache_line = i * num_elements + j;	
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		j++;

		if (j == num_elements) {
			j = 0;
			i++;
			num_elements = i / (LFU_MAX_FREQ / 4);
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 5 - all lists have between 0 and 4 elements, modulo index
static void lfu_iter_eviction_next_test06(void **state)
{
	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup lists
	ocf_cache_line_t cline_counter = 0;
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = i % 5;

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = cline_counter++;
			meta[cline].freq = i;
			add_to_freq_bucket(i, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line = 0;
	unsigned i = 0;
	unsigned j = 0;
	unsigned num_elements = i % 5;

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		if(num_elements == 0) {
			j = 0;
			i++;
			num_elements = i % 5;
			continue;
		} 
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		expected_cache_line++;

		j++;

		if (j == num_elements) {
			j = 0;
			i++;
			num_elements = i % 5;
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 6 - list length increasing by 1 from 0
static void lfu_iter_eviction_next_test07(void **state)
{
	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup lists
	ocf_cache_line_t cline_counter = 0;
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = i;

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = cline_counter++;
			meta[cline].freq = i;
			add_to_freq_bucket(i, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line = 0;
	unsigned i = 0;
	unsigned j = 0;

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		if(i == 0) {
			j = 0;
			i++;
			continue;
		} 
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		expected_cache_line++;
		j++;

		if (j == i) {
			j = 0;
			i++;
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 7 - list length increasing by 1 from 1
static void lfu_iter_eviction_next_test08(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Setup lists
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i + 1;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = i;
            add_to_freq_bucket(i, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line = 0;
    unsigned i = 0;
    unsigned j = 0;
    unsigned num_elements = i + 1;

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            num_elements = i + 1;
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 8 - list length increasing by 4 from 0
static void lfu_iter_eviction_next_test09(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Setup lists
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i * 4;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = i;
            add_to_freq_bucket(i, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line = 0;
    unsigned i = 0;
    unsigned j = 0;
    unsigned num_elements = i * 4;

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

		if (num_elements == 0) {
            j = 0;
            i++;
            num_elements = i * 4;
			continue;
        }

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            num_elements = i * 4;
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 9 - list length increasing by 4 from 1
static void lfu_iter_eviction_next_test10(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Setup lists
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i * 4 + 1;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = i;
            add_to_freq_bucket(i, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line = 0;
    unsigned i = 0;
    unsigned j = 0;
    unsigned num_elements = i * 4 + 1;

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

		if (num_elements == 0) {
            j = 0;
            i++;
            num_elements = i * 4 + 1;
            continue;
        }

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            num_elements = i * 4 + 1;
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 10: case 0 rotated right by 4
static void lfu_iter_eviction_next_test11(void **state)
{
	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	expected_cache_line = END_MARKER;

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		// Assert that you cannot find a cache line
		assert_int_equal(cache_line, expected_cache_line);
	} while (cache_line != END_MARKER);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ);
}

// case 11: case 1 rotated right by 4
static void lfu_iter_eviction_next_test12(void **state)
{
	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Insert a single element into each frequency bucket
		unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;
        ocf_cache_line_t cline = i;
        meta[cline].freq = rotated_bucket;
        add_to_freq_bucket(rotated_bucket, NULL, cline, true);
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	unsigned i = 0;

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);

		// The cache line we expect to find in bucket 'i' is the one
        // that was originally destined for it before rotation.
		expected_cache_line = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, expected_cache_line, true);

		i++;
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}


// case 12: case 2 rotated right by 4
static void lfu_iter_eviction_next_test13(void **state)
{

	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = 1 + i / (LFU_MAX_FREQ / 4);
        cline_counter += num_elements;
    }

	// Setup lists
	cline_counter = 0;
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = 1 + i / (LFU_MAX_FREQ / 4);
		unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line;
	unsigned i = 0;
	unsigned j = 0;
	unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = 1 + original_bucket / (LFU_MAX_FREQ / 4);
	expected_cache_line = start_clines[original_bucket];

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		expected_cache_line++;
		j++;

		if (j == num_elements) {
			j = 0;
			i++;
			original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = 1 + original_bucket / (LFU_MAX_FREQ / 4);
            expected_cache_line = start_clines[original_bucket];
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}


// case 13: case 3 rotated right by 4
static void lfu_iter_eviction_next_test14(void **state)
{

	// Reset
	memset(meta, 0, sizeof(meta)); // Metadata
	// Lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) { 
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

	// Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = 1 + (i % 5);
        cline_counter += num_elements;
    }

	cline_counter = 0;
	// Setup lists
	for (int i = 0; i < LFU_MAX_FREQ; i++) {
		// Determine number of elements for the current bucket
		unsigned num_elements = 1 + (i % 5);
		unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

		// Insert elements into the bucket
		for (int j = 0; j < num_elements; j++) {
			ocf_cache_line_t cline = cline_counter++;
			meta[cline].freq = rotated_bucket;
			add_to_freq_bucket(rotated_bucket, NULL, cline, true);
		}
	}

	// Setup variables
	struct ocf_lfu_iter iter;
	ocf_cache_line_t cache_line, expected_cache_line = 0;
	unsigned i = 0;
	unsigned j = 0;
	unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = 1 + (original_bucket % 5);
    expected_cache_line = start_clines[original_bucket];

	// Initialize eviction iterator
	lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

	// Try to iterate
	do {
		cache_line = lfu_iter_eviction_next(&iter);
		
		// Assert that you find the correct cache line for each frequency bucket
		assert_int_equal(cache_line, expected_cache_line);
		assert_int_equal(iter.current_freq, i);

		remove_from_freq_bucket(i, NULL, cache_line, true);

		expected_cache_line++;

		j++;

		if (j == num_elements) {
			j = 0;
			i++;
			original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = 1 + (original_bucket % 5);
            expected_cache_line = start_clines[original_bucket];
		}
	} while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

	// Ensure all freq buckets has been visited
	assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 14: case 4 rotated right by 4
static void lfu_iter_eviction_next_test15(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = i / (LFU_MAX_FREQ / 4);
        cline_counter += num_elements;
    }

    // Setup lists
    cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i / (LFU_MAX_FREQ / 4);
        unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line;
    unsigned i = 0;
    unsigned j = 0;
    unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = original_bucket / (LFU_MAX_FREQ / 4);
    expected_cache_line = start_clines[original_bucket];

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        if (num_elements == 0) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket / (LFU_MAX_FREQ / 4);
            expected_cache_line = start_clines[original_bucket];
            continue;
        }

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket / (LFU_MAX_FREQ / 4);
            expected_cache_line = start_clines[original_bucket];
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 15: case 5 rotated right by 4
static void lfu_iter_eviction_next_test16(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = i % 5;
        cline_counter += num_elements;
    }

    // Setup lists
    cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i % 5;
        unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line;
    unsigned i = 0;
    unsigned j = 0;
    unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = original_bucket % 5;
    expected_cache_line = start_clines[original_bucket];

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        if (num_elements == 0) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket % 5;
            expected_cache_line = start_clines[original_bucket];
            continue;
        }

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket % 5;
            expected_cache_line = start_clines[original_bucket];
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 16: case 6 rotated right by 4
static void lfu_iter_eviction_next_test17(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = i;
        cline_counter += num_elements;
    }

    // Setup lists
    cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i;
        unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line;
    unsigned i = 0;
    unsigned j = 0;
    unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = original_bucket;
    expected_cache_line = start_clines[original_bucket];

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        if (num_elements == 0) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket;
            expected_cache_line = start_clines[original_bucket];
            continue;
        }

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket;
            expected_cache_line = start_clines[original_bucket];
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 17: case 7 rotated right by 4
static void lfu_iter_eviction_next_test18(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = i + 1;
        cline_counter += num_elements;
    }

    // Setup lists
    cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i + 1;
        unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line;
    unsigned i = 0;
    unsigned j = 0;
    unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = original_bucket + 1;
    expected_cache_line = start_clines[original_bucket];

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket + 1;
            expected_cache_line = start_clines[original_bucket];
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 18: case 8 rotated right by 4
static void lfu_iter_eviction_next_test19(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = i * 4;
        cline_counter += num_elements;
    }

    // Setup lists
    cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i * 4;
        unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line;
    unsigned i = 0;
    unsigned j = 0;
    unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = original_bucket * 4;
    expected_cache_line = start_clines[original_bucket];

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        if (num_elements == 0) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket * 4;
            expected_cache_line = start_clines[original_bucket];
            continue;
        }

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket * 4;
            expected_cache_line = start_clines[original_bucket];
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
}

// case 19: case 9 rotated right by 4
static void lfu_iter_eviction_next_test20(void **state)
{
    // Reset
    memset(meta, 0, sizeof(meta)); // Metadata
    // Lists
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        freq_bucket_lists[i].head = END_MARKER;
        freq_bucket_lists[i].tail = END_MARKER;
        freq_bucket_lists[i].num_nodes = 0;
    }

    // Pre-calculate the starting cache line for each original bucket
    ocf_cache_line_t start_clines[LFU_MAX_FREQ];
    ocf_cache_line_t cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        start_clines[i] = cline_counter;
        unsigned num_elements = i * 4 + 1;
        cline_counter += num_elements;
    }

    // Setup lists
    cline_counter = 0;
    for (int i = 0; i < LFU_MAX_FREQ; i++) {
        // Determine number of elements for the current bucket
        unsigned num_elements = i * 4 + 1;
        unsigned rotated_bucket = (i + 4) % LFU_MAX_FREQ;

        // Insert elements into the bucket
        for (int j = 0; j < num_elements; j++) {
            ocf_cache_line_t cline = cline_counter++;
            meta[cline].freq = rotated_bucket;
            add_to_freq_bucket(rotated_bucket, NULL, cline, true);
        }
    }

    // Setup variables
    struct ocf_lfu_iter iter;
    ocf_cache_line_t cache_line, expected_cache_line;
    unsigned i = 0;
    unsigned j = 0;
    unsigned original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
    unsigned num_elements = original_bucket * 4 + 1;
    expected_cache_line = start_clines[original_bucket];

    // Initialize eviction iterator
    lfu_iter_eviction_init(&iter, NULL, NULL, 0, NULL);

    // Try to iterate
    do {
        cache_line = lfu_iter_eviction_next(&iter);

        // Assert that you find the correct cache line for each frequency bucket
        assert_int_equal(cache_line, expected_cache_line);
        assert_int_equal(iter.current_freq, i);

        remove_from_freq_bucket(i, NULL, cache_line, true);

        expected_cache_line++;
        j++;

        if (j == num_elements) {
            j = 0;
            i++;
            original_bucket = (i - 4 + LFU_MAX_FREQ) % LFU_MAX_FREQ;
            num_elements = original_bucket * 4 + 1;
            expected_cache_line = start_clines[original_bucket];
        }
    } while (cache_line != END_MARKER && i < LFU_MAX_FREQ);

    // Ensure all freq buckets has been visited
    assert_int_equal(iter.current_freq, LFU_MAX_FREQ - 1);
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
		cmocka_unit_test(lfu_iter_eviction_next_test10),
		cmocka_unit_test(lfu_iter_eviction_next_test11),
		cmocka_unit_test(lfu_iter_eviction_next_test12),
		cmocka_unit_test(lfu_iter_eviction_next_test13),
		cmocka_unit_test(lfu_iter_eviction_next_test14),
		cmocka_unit_test(lfu_iter_eviction_next_test15),
		cmocka_unit_test(lfu_iter_eviction_next_test16),
		cmocka_unit_test(lfu_iter_eviction_next_test17),
		cmocka_unit_test(lfu_iter_eviction_next_test18),
		cmocka_unit_test(lfu_iter_eviction_next_test19),
		cmocka_unit_test(lfu_iter_eviction_next_test20)
	};

	print_message("Unit test for lfu iterators\n");

	return cmocka_run_group_tests(tests, NULL, NULL);
}