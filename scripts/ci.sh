#!/usr/bin/env bash
#
# 统一构建与测试入口。
#
# 为什么要有这个脚本：构建逻辑如果写在 CI 的 YAML 里，那么换一个 CI 平台
# （GitHub Actions -> Gitee Go / Jenkins / 云效）就要把同一套命令再抄一遍，
# 抄完之后两边会慢慢跑偏——一边加了 -DDCACHE_SANITIZE，另一边忘了。
# 把命令收在脚本里，CI 只负责「在哪台机器上调用」，这是唯一需要按平台变的部分。
#
# 用法：
#   scripts/ci.sh build   普通 Release 构建 + 全部单测
#   scripts/ci.sh asan    ASan + UBSan（查内存错误与未定义行为）
#   scripts/ci.sh tsan    TSan（查数据竞争，ASan 查不出这类问题）
#   scripts/ci.sh lint    clang-format 格式检查 + clang-tidy 静态分析
#   scripts/ci.sh coverage  --coverage 插桩 + gcovr 统计，行覆盖率低于门槛即失败
#   scripts/ci.sh fuzz      libFuzzer 跑 json / url / HTTP 解析器（需 clang）
#   scripts/ci.sh all     依次执行上面六项
#
# 环境变量：
#   JOBS               并行度，默认取 CPU 核数
#   COVERAGE_MIN_LINE  行覆盖率门槛（百分比），默认 75
#   FUZZ_SECONDS       每个 fuzz 目标的运行预算（秒），默认 30

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

# 覆盖率门槛（行覆盖率百分比）。75 是「不许倒退」的地板：首跑实测 79%，
# 留 3 个点给工具版本差异（gcovr/gcc 的计数口径会小幅漂移），
# 超过这个余量的下跌只可能是真的少测了。收紧要单独提交并写明原因；
# 放宽必须在 PR 里解释为什么这次的低覆盖是合理的。
COVERAGE_MIN_LINE="${COVERAGE_MIN_LINE:-75}"

# 每个 fuzz 目标的运行预算（秒）。CI 里不能「跑到崩为止」——那意味着一次红的
# 耗时不确定；固定预算才能预判时长。深度 fuzz 属于本地/夜间任务，CI 这道关
# 只负责挡住「几秒钟就能撞出来」的崩。
FUZZ_SECONDS="${FUZZ_SECONDS:-30}"

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# configure_build_test <名称> <构建目录> [cmake 额外参数...]
configure_build_test() {
    local name="$1" dir="$2"
    shift 2

    log "[$name] 配置"
    cmake -S . -B "$dir" "$@"

    log "[$name] 构建（-j $JOBS）"
    cmake --build "$dir" -j "$JOBS"

    log "[$name] 测试"
    # --output-on-failure 是必须的：CI 里失败用例的输出默认被吞掉，
    # 只留一行 "1 test failed"，排查时等于没有信息。
    ctest --test-dir "$dir" --output-on-failure
}

do_build() {
    configure_build_test build build -DCMAKE_BUILD_TYPE=Release
}

do_asan() {
    configure_build_test asan build-san \
        -DCMAKE_BUILD_TYPE=Debug \
        -DDCACHE_SANITIZE=address,undefined
}

do_tsan() {
    configure_build_test tsan build-tsan \
        -DCMAKE_BUILD_TYPE=Debug \
        -DDCACHE_SANITIZE=thread
}

do_lint() {
    local fmt="${CLANG_FORMAT:-clang-format}"
    local tidy="${CLANG_TIDY:-clang-tidy}"

    if ! command -v "$fmt" >/dev/null 2>&1 || ! command -v "$tidy" >/dev/null 2>&1; then
        echo "跳过 lint：未找到 clang-format 或 clang-tidy" >&2
        echo "  Ubuntu/Debian: sudo apt-get install -y clang-format clang-tidy" >&2
        echo "  macOS:         brew install clang-format llvm" >&2
        # 工具缺失不算失败，但要在日志里留下痕迹，避免「以为跑过了」。
        return 0
    fi

    log "[lint] 格式检查"
    while IFS= read -r -d '' f; do
        "$fmt" --dry-run --Werror "$f"
    done < <(find include src apps tests fuzz \
                  \( -name '*.hpp' -o -name '*.cpp' -o -name '*.tpp' \) -print0)

    log "[lint] 静态分析"
    cmake -S . -B build-lint -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
    while IFS= read -r -d '' f; do
        "$tidy" -p build-lint "$f"
    done < <(find src apps -name '*.cpp' -print0)
}

do_coverage() {
    local dir=build-cov

    if ! command -v gcovr >/dev/null 2>&1; then
        # 和 lint 同样的处理：缺工具就明确跳过，不静默通过、也不算失败。
        echo "跳过覆盖率：未找到 gcovr" >&2
        echo "  pip install --user gcovr   （或 apt-get install -y gcovr）" >&2
        return 0
    fi

    configure_build_test coverage "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DDCACHE_COVERAGE=ON

    log "[coverage] 统计（门槛：行覆盖率 ${COVERAGE_MIN_LINE}%）"
    local report="$dir/coverage.txt"
    local rc=0
    # --root 让报告里的路径相对仓库根；--filter 只统计库本体。
    # 用 apps/ 的入口和 tests/ 的用例给自己打分没有意义，所以排除。
    # 注意 gcovr 在「一个文件都没匹配到」时不报错，会给一份空表（0/0）——
    # 那看起来和「100% 覆盖」一样漂亮，所以下面必须单独判一次 TOTAL 行。
    gcovr --root . "$dir" \
        --filter 'src/' --filter 'include/' \
        --exclude 'tests/' --exclude 'apps/' \
        --txt "$report" --print-summary \
        --fail-under-line "$COVERAGE_MIN_LINE" || rc=$?

    if ! grep -q '^TOTAL' "$report" 2>/dev/null; then
        echo "覆盖率报告里没有 TOTAL 行：--filter 没匹配到任何被插桩的源文件。" >&2
        echo "常见原因：没加 --coverage（.gcda 不存在）、测试没跑、或构建目录传错。" >&2
        echo "这种情况一律按失败处理，不要当成通过。" >&2
        return 1
    fi

    if [ "$rc" -ne 0 ]; then
        echo "覆盖率未达门槛：行覆盖率低于 ${COVERAGE_MIN_LINE}%（明细：${report}）。" >&2
        echo "两条路：补测试把数字抬上去，或确属合理下调时显式改 COVERAGE_MIN_LINE 并写明理由。" >&2
        return "$rc"
    fi

    log "[coverage] 通过（门槛 ${COVERAGE_MIN_LINE}%，明细：${report}）"
}

do_fuzz() {
    local dir=build-fuzz

    if ! command -v clang++ >/dev/null 2>&1; then
        # 和 lint 一样：缺工具就明确跳过，不静默通过、也不算失败。
        echo "跳过 fuzz：未找到 clang++（libFuzzer 只随 clang 提供）" >&2
        echo "  Ubuntu/Debian: sudo apt-get install -y clang" >&2
        echo "  macOS:         xcode-select --install" >&2
        return 0
    fi

    log "[fuzz] 配置"
    cmake -S . -B "$dir" \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DDCACHE_FUZZ=ON

    log "[fuzz] 构建 fuzz 目标（-j $JOBS）"
    # 只构建 fuzz 目标：这个构建里的单元测试没有意义（它们同样带着 fuzzer 覆盖率插桩，
    # 跑起来只是更慢），少编 7 个可执行文件能省掉一半时间。
    cmake --build "$dir" -j "$JOBS" --target fuzz_json fuzz_url fuzz_http_request

    local rc=0
    local name
    for name in json url http_request; do
        local exe="$dir/fuzz_${name}"
        local corpus="$dir/corpus/$name"
        # 语料在构建目录里跑，不直接写 fuzz/corpus/：fuzzer 会不断往「第一个语料目录」
        # 里塞新样本，写进源码树的话，每次本地跑完都会多出一堆未跟踪文件。
        # 种子先拷进去，让它从「已知能被解析」的样本起步，而不是纯随机字节。
        mkdir -p "$corpus"
        if [ -d "fuzz/corpus/$name" ]; then
            cp -n fuzz/corpus/"$name"/* "$corpus"/ 2>/dev/null || true
        fi

        log "[fuzz] ${name}（预算 ${FUZZ_SECONDS}s）"
        if ! "$exe" -max_total_time="$FUZZ_SECONDS" -print_final_stats=1 "$corpus"; then
            echo "" >&2
            echo "fuzz 目标 fuzz_${name} 报告了崩溃或不变式失败。" >&2
            echo "崩溃样本已写到仓库根目录的 crash-*，复现方式：$exe crash-xxx" >&2
            rc=1
        fi
    done
    return "$rc"
}

case "${1:-}" in
    build) do_build ;;
    asan)  do_asan ;;
    tsan)  do_tsan ;;
    lint)  do_lint ;;
    coverage) do_coverage ;;
    fuzz) do_fuzz ;;
    all)
        do_build
        do_asan
        do_tsan
        do_lint
        do_coverage
        do_fuzz
        ;;
    *)
        echo "用法: $0 <build|asan|tsan|lint|coverage|fuzz|all>" >&2
        exit 2
        ;;
esac

log "完成：${1}"
