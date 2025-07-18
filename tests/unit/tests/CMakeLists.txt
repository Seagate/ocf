cmake_minimum_required(VERSION 2.6.0)

project(OCF_unit_tests C)

enable_testing()

include_directories(
	/opt/spdk/ocf/src/
	/opt/spdk/ocf/src/cleaning/
	/opt/spdk/ocf/src/engine/
	/opt/spdk/ocf/src/metadata/
	/opt/spdk/ocf/src/eviction/
	/opt/spdk/ocf/src/mngt/
	/opt/spdk/ocf/src/concurrency/
	/opt/spdk/ocf/src/utils/
	/opt/spdk/ocf/inc/
	/opt/spdk/ocf/src/promotion/
	/opt/spdk/ocf/src/promotion/nhit/
	${CMOCKA_PUBLIC_INCLUDE_DIRS}
	${CMAKE_BINARY_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	/opt/spdk/ocf/tests/unit/tests/ocf_env/
)


add_subdirectory(/opt/spdk/ocf/tests/unit/tests/ocf_env/)


add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/ocf_lru.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/cleaning/cleaning.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/utils/utils_generator.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/utils/utils_refcnt.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/ocf_lfu.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/concurrency/ocf_cache_line_concurrency.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/mngt/ocf_mngt_io_class.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/metadata/metadata_collision.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/engine/engine_common.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/ocf_space.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/concurrency/ocf_metadata_concurrency.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/mngt/ocf_mngt_cache.c)
add_subdirectory(/opt/spdk/ocf/tests/unit/tests/sources_to_test_repository/utils/utils_rbtree.c)