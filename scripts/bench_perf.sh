#!/usr/bin/env bash
# 压测编排：起节点 -> 跑 bench -> 收尸 -> 打表。
#
# 为什么要有这个脚本，而不是让人照着 README 手敲：
#
#   节点的 --capacity 默认是 100。手动起节点时忘了给 --capacity，压出来的是一个
#   「命中率 10%」的数字——它其实在测 LRU 淘汰。这份脚本把 capacity / keys /
#   workers / threads 全部显式铺开，让「参数是什么」和「数字是多少」绑在一起进报告。
#
#   四种配置的差别只在「有没有远端转发」「会不会淘汰」「走不走 TLS」，别的都一样：
#     A 单节点 · key 全装得下   -> 本地命中路径的纯成本
#     B 三节点 · key 全装得下   -> 只多了一层一致性哈希判定（稳态下仍是本地命中）
#     C 三节点 · key 装不下     -> 淘汰 + 转发 + 回填，最接近真实负载
#     D 单节点 · TLS            -> 与 A 同一份负载，只多一层握手与记录层
#
#   D 存在的理由：TLS 是这个项目的主打功能，而「TLS 慢多少」这种问题
#   **必须在自己的机器上、同一份负载下、用同一个客户端量**才有意义。
#   拿别处的数字对比毫无用处 —— 那两个数根本不是同一个实验。
#
# 用法：
#   bash scripts/bench_perf.sh                # 默认 3 轮取中位数
#   REPS=1 REQUESTS=5000 bash scripts/bench_perf.sh   # 快速验证
#
# 所有参数都可以用环境变量覆盖，CI 与本地跑的是同一份逻辑。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

REPS=${REPS:-3}
BASE_PORT=${BASE_PORT:-18001}
KEYS=${KEYS:-1000}
CAPACITY=${CAPACITY:-10000}
WORKERS=${WORKERS:-2}
THREADS=${THREADS:-2}
REQUESTS=${REQUESTS:-20000}
WARMUP=${WARMUP:-2000}
# C 配置的 key 空间：要明显大于「节点数 × 容量」，否则压不到淘汰与回填。
COLD_KEYS=${COLD_KEYS:-100000}

# transport=tcp 时传空数组；transport=tls 时填证书路径。
# 用数组而不是字符串拼接：路径里一旦有空格，拼接版会把参数拆成两个。
TLS_ARGS=()
BENCH_TLS_ARGS=()
CERT_PEM=""
KEY_PEM=""

NODE=./node
BENCH=./bench
# 二进制放在哪里：make 直接放仓库根目录，CMake 放在构建目录里。
# 做成可覆盖的，是因为 CI 用 CMake（Release）而本地常用 make——
# 一份脚本两边都能跑，才谈得上「本地复现 CI 的数字」。
BINDIR=${BINDIR:-.}
NODE="$BINDIR/node"
BENCH="$BINDIR/bench"
TMP=$(mktemp -d)
NODE_PIDS=()

cleanup() {
    for pid in "${NODE_PIDS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    # 等它们真的退出，否则下一轮起在同一组端口上会 bind 失败。
    for pid in "${NODE_PIDS[@]:-}"; do
        [ -n "$pid" ] && wait "$pid" 2>/dev/null
    done
    NODE_PIDS=()
}
trap 'cleanup; rm -rf "$TMP"' EXIT

if [ ! -x "$NODE" ] || [ ! -x "$BENCH" ]; then
    echo "找不到 $NODE 或 $BENCH：先在仓库根目录构建（bash scripts/ci.sh build）" >&2
    exit 1
fi
# 生成一份自签证书，供 TLS 配置使用。
#
# 两个细节不能错：
#   1. SAN 必须写 IP:127.0.0.1。客户端连的是 IP 字面量，走的是
#      X509_VERIFY_PARAM_set1_ip_asc —— 它只认 IP 形式的 SAN。
#      实测：同一份证书把 SAN 换成 DNS:127.0.0.1 之后，
#      `openssl verify -verify_ip 127.0.0.1` 直接报 "IP address mismatch"。
#   2. CA:TRUE。节点用同一份证书校验对端，而这份证书是自签的；
#      basicConstraints 不是 CA:TRUE 的话，OpenSSL 会拒绝把它当信任锚。
#      （`req -x509` 默认就带 CA:TRUE，这里再写一遍是显式声明，不是叠加。）
make_cert() {
    CERT_PEM="$TMP/cert.pem"
    KEY_PEM="$TMP/key.pem"
    if ! openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$KEY_PEM" -out "$CERT_PEM" -days 1 \
        -subj "/CN=127.0.0.1" \
        -addext "subjectAltName=IP:127.0.0.1" \
        -addext "basicConstraints=critical,CA:TRUE" >/dev/null 2>&1; then
        echo "生成自签证书失败（openssl 不可用？）" >&2
        return 1
    fi
    TLS_ARGS=(--tls-cert "$CERT_PEM" --tls-key "$KEY_PEM" --tls-ca "$CERT_PEM")
    BENCH_TLS_ARGS=(--tls-ca "$CERT_PEM")
    return 0
}

start_cluster() {
    local count=$1
    local peers=""
    local i port
    for i in $(seq 1 "$count"); do
        port=$((BASE_PORT + i - 1))
        peers="${peers:+$peers,}127.0.0.1:$port"
    done

    for i in $(seq 1 "$count"); do
        port=$((BASE_PORT + i - 1))
        # --ttl-ms 0 = 不过期：这一批压的是容量驱动的淘汰，不是 TTL 到期，
        # 免得把「过期重建」的成本算进命中路径。
        #
        # ${TLS_ARGS[@]+...} 这种写法是为了在数组为空时也不触发 unbound variable
        # （bash 3.2 在 set -u 下会直接报错，bash 4.3+ 才不报）。
        "$NODE" --self "127.0.0.1:$port" --peers "$peers" \
            --capacity "$CAPACITY" --workers "$WORKERS" --ttl-ms 0 \
            --report-interval 3600 \
            ${TLS_ARGS[@]+"${TLS_ARGS[@]}"} >"$TMP/node-$port.log" 2>&1 &
        NODE_PIDS+=("$!")
    done

    # 等端口真的能应答。sleep 固定秒数在慢 runner 上会假失败，快机器上又白等。
    local probe="127.0.0.1:$BASE_PORT"
    for _ in $(seq 1 100); do
        if curl -s -m 1 -o /dev/null "http://$probe/stats"; then
            return 0
        fi
        sleep 0.1
    done
    echo "节点在 10 秒内没起来，日志：" >&2
    cat "$TMP"/node-*.log >&2
    return 1
}

# 中位数。样本量是 REPS（默认 3），偶数取中间两个的均值。
median_of() {  # $1 = 列号，stdin = TSV
    cut -f"$1" | sort -n | awk '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "NA"; exit }
            if (NR % 2 == 1) { printf "%.0f\n", a[(NR + 1) / 2] }
            else { printf "%.0f\n", (a[NR / 2] + a[NR / 2 + 1]) / 2 }
        }'
}

run_config() {
    local label=$1 nodes=$2 keys=$3 transport=${4:-tcp}
    local out="$TMP/result.tsv"
    : >"$out"

    # 证书在整轮里只生成一次：换证书会让连接池里的旧会话全部失效，
    # 那样测出来的就是「重新握手」而不是「TLS 的稳态开销」。
    if [ "$transport" = "tls" ]; then
        if ! make_cert; then
            echo "BENCH_FAILED [$label] 证书生成失败"
            return 1
        fi
    else
        TLS_ARGS=()
        BENCH_TLS_ARGS=()
    fi

    cleanup
    if ! start_cluster "$nodes"; then
        echo "BENCH_FAILED [$label] 集群未启动"
        return 1
    fi

    local rep
    for rep in $(seq 1 "$REPS"); do
        "$BENCH" --target "127.0.0.1:$BASE_PORT" --group scores \
            --threads "$THREADS" --requests "$REQUESTS" --warmup "$WARMUP" \
            --keys "$keys" \
            ${BENCH_TLS_ARGS[@]+"${BENCH_TLS_ARGS[@]}"} >"$TMP/bench-$label-$rep.log" 2>&1
        local rc=$?
        if [ $rc -ne 0 ]; then
            echo "BENCH_FAILED [$label] bench 退出码 $rc，日志尾部：" >&2
            tail -5 "$TMP/bench-$label-$rep.log" >&2
            return 1
        fi
        # 把 RESULT 行转成 TSV。字段顺序由 bench 决定，这里按位置取——
        # 所以 bench 里那行的字段顺序是契约，改它就得同步改这里。
        awk '/^RESULT/ { for (i = 2; i <= NF; i++) { split($i, kv, "="); printf "%s%s", (i > 2 ? "\t" : ""), kv[2] } print "" }' \
            "$TMP/bench-$label-$rep.log" >>"$out"
    done

    # 列顺序：qps ok failed found n p50 p90 p99 p999 max
    local qps p50 p99 p999
    qps=$(median_of 1 <"$out")
    p50=$(median_of 6 <"$out")
    p99=$(median_of 8 <"$out")
    p999=$(median_of 9 <"$out")

    # 节点侧计数器只取最后一轮。它是**节点自启动以来的累计值**，包含预热和
    # 之前几轮 —— 所以只能当「这份配置下的命中率量级」看，不能当成某一轮的值。
    local node_note=""
    if grep -q '^STATS' "$TMP/bench-$label-$REPS.log" 2>/dev/null; then
        node_note=" $(grep '^STATS' "$TMP/bench-$label-$REPS.log" | tail -1 | sed 's/^STATS //')"
    fi

    cleanup

    echo ""
    echo "---- $label（nodes=$nodes capacity=$CAPACITY keys=$keys workers=$WORKERS threads=$THREADS transport=$transport reps=$REPS）"
    echo "     qps=$qps  p50=${p50}us  p99=${p99}us  p999=${p999}us   （${REPS} 轮中位数）"
    [ -n "$node_note" ] && echo "     节点累计：${node_note# }"
    # 给 CI 的一行：注解用它，人看上面那张表。
    echo "BENCH_SUMMARY [$label] nodes=$nodes capacity=$CAPACITY keys=$keys transport=$transport qps=$qps p50=${p50}us p99=${p99}us p999=${p999}us${node_note}"
    return 0
}

echo "====== 环境 ======"
echo "os      : $(uname -srm)"
# 这些数字只在同一台机器上互相比较才有意义。共享 runner 的 vCPU 会被邻居抢，
# 绝对值和「某台服务器上的 QPS」不是一回事——报告里必须连着这条一起写。
echo "cpu     : $( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?') ) 逻辑核"
echo "reps    : $REPS"
echo ""

rc=0
run_config "单节点·全命中" 1 "$KEYS" tcp || rc=1
run_config "三节点·全命中" 3 "$KEYS" tcp || rc=1
run_config "三节点·会淘汰" 3 "$COLD_KEYS" tcp || rc=1
# TLS 与 A 是同一份负载、同一台机器、同一时刻，唯一差别是传输层。
# 它的价值不在绝对值（「TLS 每请求贵多少微秒」），而在**这个比值**。
run_config "单节点·TLS" 1 "$KEYS" tls || rc=1

echo ""
echo "====== 怎么读这些数字 ======"
echo "· qps 的绝对值依赖机器：同一台机器上的横向对比有意义，跨机器没有。"
echo "· p999 比 p99 更值得看：它先暴露事件循环被一条大响应推迟这类抖动。"
echo "· 「全命中」与「会淘汰」的差值就是淘汰 + 转发 + 回填的代价；"
echo "  想单独看转发的代价，把 C 的 keys 调到 capacity 以下再跑一次。"
echo "· 「单节点·全命中」与「单节点·TLS」是同一份负载，差值就是 TLS 的代价；"
echo "  两者都在同一台机器上跑，这个差值比各自的绝对值可靠。"
echo "· 三节点配置会超售：每个节点是 1 个事件循环 + $WORKERS 个 worker，"
echo "  三个节点加上 $THREADS 个压测线程 = $((3 * WORKERS + 3 + THREADS)) 个可运行线程。"
echo "  在 $( (nproc 2>/dev/null || echo '?') ) 个逻辑核上跑不开是预期的——"
echo "  这时比的仍然是同环境下的两组配置，别把它当成「三节点比单节点慢」。"

exit $rc
