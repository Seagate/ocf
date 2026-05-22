cmake_minimum_required(VERSION 2.6.0)

project(OCF_unit_tests C)

enable_testing()

include_directories(
	/mnt/host/open-cas-linux/ocf/src/
	/mnt/host/open-cas-linux/ocf/src/cleaning/
	/mnt/host/open-cas-linux/ocf/src/engine/
	/mnt/host/open-cas-linux/ocf/src/metadata/
	/mnt/host/open-cas-linux/ocf/src/eviction/
	/mnt/host/open-cas-linux/ocf/src/mngt/
	/mnt/host/open-cas-linux/ocf/src/concurrency/
	/mnt/host/open-cas-linux/ocf/src/utils/
	/mnt/host/open-cas-linux/ocf/inc/
	/mnt/host/open-cas-linux/ocf/src/promotion/
	/mnt/host/open-cas-linux/ocf/src/promotion/nhit/
	${CMOCKA_PUBLIC_INCLUDE_DIRS}
	${CMAKE_BINARY_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	/mnt/host/open-cas-linux/ocf/tests/unit/tests/ocf_env/
)


add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/ocf_env/)


add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/eviction/ocf_lru.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/engine/engine_common.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/metadata/metadata_collision.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/mngt/ocf_mngt_io_class.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/cleaning/cleaning.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/utils/utils_rbtree.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/concurrency/ocf_cache_line_concurrency.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/utils/utils_generator.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/ocf_space.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/concurrency/ocf_metadata_concurrency.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/eviction/ocf_lfu.c)
add_subdirectory(/mnt/host/open-cas-linux/ocf/tests/unit/tests/sources_to_test_repository/mngt/ocf_mngt_cache.c)