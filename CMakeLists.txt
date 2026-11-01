cmake_minimum_required(VERSION 3.16)

project(dcache
    VERSION 1.0.0
    DESCRIPTION "分布式缓存（一致性哈希 + LRU + HTTP 网关 + singleflight）"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 未指定构建类型时默认 Release（基准测试需要优化）。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# 可选 Sanitizer：-DDCACHE_SANITIZE=address,undefined（或 thread）。
# 必须放在所有 target 之前：add_compile_options 只对之后创建的 target 生效。
# 早期版本挂在 dcache_core 的 INTERFACE 上，导致库自身源码不被插桩（只有测试被插桩）。
set(DCACHE_SANITIZE "" CACHE STRING "逗号分隔的 Sanitizer 列表：address,undefined,thread；留空关闭")
if(DCACHE_SANITIZE)
    if(MSVC)
        message(WARNING "MSVC 不支持 -fsanitize，DCACHE_SANITIZE 已忽略")
    else()
        if(DCACHE_SANITIZE MATCHES "address" AND DCACHE_SANITIZE MATCHES "thread")
            message(FATAL_ERROR "ASan 与 TSan 不能同时启用，请分两次构建")
        endif()
        string(REPLACE "," ";" _dcache_sans "${DCACHE_SANITIZE}")
        foreach(_s IN LISTS _dcache_sans)
            add_compile_options(-fsanitize=${_s} -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=${_s})
        endforeach()
    endif()
endif()

# 可选覆盖率插桩：-DDCACHE_COVERAGE=ON（gcc/clang 的 --coverage，供 gcovr/lcov 读取）。
# 同样必须放在所有 target 之前，否则只有部分 TU 被插桩，算出来的百分比没有意义。
# 不要和 DCACHE_SANITIZE 一起用：ASan 下进程异常退出会丢掉 .gcda，覆盖率会莫名偏低。
option(DCACHE_COVERAGE "启用 --coverage 插桩（gcov/gcovr/lcov 用）" OFF)
if(DCACHE_COVERAGE)
    if(MSVC)
        message(WARNING "MSVC 不支持 --coverage，DCACHE_COVERAGE 已忽略")
    else()
        # -O0：优化会把多行合并/删除，行号统计随之失真，所以关掉优化再测。
        add_compile_options(--coverage -O0 -g)
        add_link_options(--coverage)
    endif()
endif()

# 统一告警开关。必须在 target 定义前声明，才能作用于 dcache_core 本身。
function(dcache_add_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endfunction()

add_library(dcache_core
    src/hash.cpp
    src/consistent_hash.cpp
    src/json.cpp
    src/http_client.cpp
    src/http_server.cpp
    src/group.cpp
    src/peer_pool.cpp
    src/url.cpp
    src/node.cpp)
target_include_directories(dcache_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(dcache_core PUBLIC cxx_std_17)
target_link_libraries(dcache_core PUBLIC Threads::Threads)
# 库本体占全项目约 90% 的代码量，此前漏加了告警开关（只对 app/test 生效）。
dcache_add_warnings(dcache_core)

# 可执行程序
add_executable(node apps/node_main.cpp)
target_link_libraries(node PRIVATE dcache_core)
dcache_add_warnings(node)

add_executable(bench apps/bench.cpp)
target_link_libraries(bench PRIVATE dcache_core)
dcache_add_warnings(bench)

# 单元测试
set(DCACHE_TESTS
    test_lru_cache
    test_consistent_hash
    test_singleflight
    test_json
    test_url
    test_node)
foreach(t IN LISTS DCACHE_TESTS)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE dcache_core)
    dcache_add_warnings(${t})
endforeach()

enable_testing()
foreach(t IN LISTS DCACHE_TESTS)
    add_test(NAME ${t} COMMAND ${t})
endforeach()