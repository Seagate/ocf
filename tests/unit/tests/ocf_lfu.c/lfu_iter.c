/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * <tested_file_path>src/ocf_lfu.c</tested_file_path>
 * <tested_function>lfu_iter_eviction_next</tested_function>
 * <functions_to_leave>
 *	lfu_iter_init
 *	lfu_iter_eviction_init
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

#include "ocf_lfu.c/lfu_iter_generated_wraps.c"

/** Wrappers */

struct ocf_cache_line_concurrency *__wrap_ocf_cache_line_concurrency(ocf_cache_t cache)
{
	return NULL;
}



/** Test cases */

static void lfu_iter_eviction_next_test01(void **state)
{
	print_test_description("Put test description here\n");
	assert_int_equal(1,1);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(lfu_iter_eviction_next_test01)
	};

	print_message("Unit test for lfu_iter_eviction_next\n");

	return cmocka_run_group_tests(tests, NULL, NULL);
}