add_library(ocf_env ocf_env.c)

add_definitions(-Dstatic= -Dinline=)

target_include_directories(ocf_env PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    /usr/include           # Optional, only needed if custom includes
)

target_link_libraries(ocf_env pthread z)