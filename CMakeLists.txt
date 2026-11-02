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

# 可选 libFuzzer 目标：-DDCACHE_FUZZ=ON。只支持 clang —— libFuzzer 随 clang 分发，
# gcc 侧没有等价物（AFL++ 是另一套工具链，先不引入）。
option(DCACHE_FUZZ "构建 libFuzzer 目标（需要 clang）" OFF)
if(DCACHE_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DCACHE_FUZZ 需要 clang：libFuzzer 只随 clang 提供")
    endif()
    # fuzzer-no-link = 只插覆盖率探针、不引入 main。库必须一起插桩，否则 fuzzer
    # 走的是「纯随机」模式：它看不到任何「哪些分支被走到过」的信息，
    # 几分钟的随机测试未必比手写用例强。main 由各 fuzz 目标自己链接 -fsanitize=fuzzer。
    # -O1：比 -O0 快好几倍，又不至于像 -O2 那样把变量优化掉、让崩溃栈难以对应源码。
    add_compile_options(-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address)
endif()

# 节点间 TLS：-DDCACHE_TLS=OFF 时编出桩实现（include/tls.hpp 接口不变，
# 运行时明确报「不可用」）。刻意选择「找不到 OpenSSL 就 configure 失败」而不是
# 静默降级成明文：静默降级会让「本节点已启用 TLS」这个结论在别人的机器上不成立，
# 而这种事只有在出事之后才会被发现。
option(DCACHE_TLS "节点间 TLS（需要 OpenSSL）" ON)

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
    src/http_parser.cpp
    src/http_server.cpp
    src/group.cpp
    src/peer_pool.cpp
    src/url.cpp
    src/node.cpp)
target_include_directories(dcache_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(dcache_core PUBLIC cxx_std_17)
target_link_libraries(dcache_core PUBLIC Threads::Threads)

# TLS 实现二选一。用 target_sources 而不是把两个文件都写进 add_library：
# 同时编进来会得到重复定义的 TLS 符号，而链接期的报错信息通常指着不相干的地方。
if(DCACHE_TLS)
    find_package(OpenSSL REQUIRED)
    target_sources(dcache_core PRIVATE src/tls_openssl.cpp)
    target_compile_definitions(dcache_core PUBLIC DCACHE_TLS=1)
    target_link_libraries(dcache_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    target_sources(dcache_core PRIVATE src/tls_stub.cpp)
    message(STATUS "DCACHE_TLS=OFF：节点间 TLS 不可用（桩实现）")
endif()

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
    test_http_parser
    test_node
    test_worker_isolation
    test_latency_stats)

# TLS 测试要自己签发证书（用 OpenSSL API，不依赖 openssl 命令行），
# 因此只在 DCACHE_TLS=ON 时存在。
if(DCACHE_TLS)
    list(APPEND DCACHE_TESTS test_tls)
endif()

foreach(t IN LISTS DCACHE_TESTS)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE dcache_core)
    dcache_add_warnings(${t})
endforeach()

enable_testing()
foreach(t IN LISTS DCACHE_TESTS)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# libFuzzer 目标：只在 DCACHE_FUZZ=ON 时存在。普通构建里它们连编译都不该发生——
# 没有 -fsanitize=fuzzer 就找不到 LLVMFuzzerTestOneInput 的运行时，链接必然失败。
if(DCACHE_FUZZ)
    set(DCACHE_FUZZERS
        fuzz_json
        fuzz_url
        fuzz_http_request)
    foreach(f IN LISTS DCACHE_FUZZERS)
        add_executable(${f} fuzz/${f}.cpp)
        target_link_libraries(${f} PRIVATE dcache_core)
        # 这里才引入 libFuzzer 的 main 与驱动逻辑。
        target_link_options(${f} PRIVATE -fsanitize=fuzzer,address)
        dcache_add_warnings(${f})
    endforeach()
endif()