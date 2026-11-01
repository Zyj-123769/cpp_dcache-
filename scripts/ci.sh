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
#   scripts/ci.sh all     依次执行上面四项
#
# 环境变量：
#   JOBS   并行度，默认取 CPU 核数

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

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
    done < <(find include src apps tests \
                  \( -name '*.hpp' -o -name '*.cpp' -o -name '*.tpp' \) -print0)

    log "[lint] 静态分析"
    cmake -S . -B build-lint -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
    while IFS= read -r -d '' f; do
        "$tidy" -p build-lint "$f"
    done < <(find src apps -name '*.cpp' -print0)
}

case "${1:-}" in
    build) do_build ;;
    asan)  do_asan ;;
    tsan)  do_tsan ;;
    lint)  do_lint ;;
    all)
        do_build
        do_asan
        do_tsan
        do_lint
        ;;
    *)
        echo "用法: $0 <build|asan|tsan|lint|all>" >&2
        exit 2
        ;;
esac

log "完成：${1}"
