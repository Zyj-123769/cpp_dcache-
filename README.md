# 04. 分布式缓存（C++）

[![CI](https://github.com/Zyj-123769/cpp_dcache-/actions/workflows/ci.yml/badge.svg)](https://github.com/Zyj-123769/cpp_dcache-/actions/workflows/ci.yml)

对标 Go 版 groupcache / GeeCache 的 C++ 实现：**线程安全 LRU 本地缓存 + 一致性哈希路由 + HTTP 网关 + singleflight 防击穿**。

在单机 LRU 缓存之上叠加一致性哈希节点路由与 HTTP 网关，实现「本地命中即返回、未命中按哈希转发到负责节点并回填」的多节点分布式缓存。

## 架构

```
                ┌──────────────────────────────────────┐
                │              HTTP 客户端 (bench)       │
                └───────────────────┬──────────────────┘
                                    │ GET /api/{group}/{key}
        ┌───────────────────────────┼───────────────────────────┐
        │                           │                           │
   ┌────▼────┐                 ┌────▼────┐                 ┌────▼────┐
   │ Node A  │   HTTP 转发      │ Node B  │                 │ Node C  │
   │ 8001    │◄───────────────►│ 8002    │◄───────────────►│ 8003    │
   └────┬────┘                 └────┬────┘                 └────┬────┘
        │                           │                           │
   ┌────▼───────────────────────────▼───────────────────────────▼────┐
   │            一致性哈希环（每物理节点 150 个虚拟节点）               │
   └──────────────────────────────────────────────────────────────────┘

单个 Node 内部读取链路：

  Get(group, key)
    └─ Group.Get(key)
         ├─ 本地 LRU 命中 → 返回（命中率在 /stats 可见）
         └─ 未命中 → singleflight 合并并发（防击穿）
              ├─ 一致性哈希判断负责节点
              │    ├─ 自身负责 → 回源 getter 并写本地（负结果短 TTL 防穿透）
              │    └─ 远端负责 → HTTP GET 到负责节点取数 →
              │                   回填本地（负结果短 TTL 防穿透）
```

### 核心机制与 Phase 对应

| Phase | 模块 | 文件 |
|---|---|---|
| 1 线程安全 LRU + TTL | `LruCache<K,V>` | `include/lru_cache.hpp` / `src/lru_cache.tpp` |
| 2 HTTP 网关 | `HttpServer`（epoll + 工作线程池）、`HttpClient`、JSON 报文 | `include/http_server.hpp` `include/http_client.hpp` `include/json.hpp` |
| 3 一致性哈希路由 | `ConsistentHash`（murmur3 + 虚拟节点）、`HttpPeerPool` | `include/consistent_hash.hpp` `include/peer_pool.hpp` |
| 4 防击穿 / 治理 | `SingleFlight<R>`、空值短 TTL、过期抖动、统计 | `include/singleflight.hpp` `include/group.hpp` |

## 目录结构

```
include/              头文件（模板实现见 src/*.tpp）
  common.hpp           GetResult / Getter 回源接口
  hash.hpp             murmur3_32
  lru_cache.hpp        线程安全 LRU + TTL
  consistent_hash.hpp  一致性哈希环
  singleflight.hpp     并发回源去重
  json.hpp             JSON 编解码（节点间报文）
  auth.hpp             常量时间的 token 比较（内部接口信任边界）
  tls.hpp              节点间 TLS 的最小封装（pimpl，不暴露 OpenSSL 类型）
  http.hpp             HttpRequest / HttpResponse
  http_client.hpp      同步 HTTP GET 客户端
  http_server.hpp      epoll HTTP 服务器（事件循环 + 工作线程池）
  peer_pool.hpp        PeerPicker 接口 + HttpPeerPool
  group.hpp            缓存分组（LRU + 回源 + singleflight）
  node.hpp             缓存节点（多 Group + PeerPool + /api 路由）
src/                  实现
apps/node_main.cpp    节点进程
apps/bench.cpp        压测客户端
tests/                单元测试
```

## 构建与运行（Linux / WSL）

本项目网络层基于 Linux `socket/epoll`，**需在 Linux 或 WSL 下编译运行**。

### 方式一：Make

```bash
make            # 产出 node、bench
make test       # 编译并运行全部单元测试
```

### 方式二：CMake + CTest

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

可选 Sanitizer：`cmake -S . -B build -DDCACHE_SANITIZE=address,undefined`。
可选覆盖率：`cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DDCACHE_COVERAGE=ON`，
或直接 `bash scripts/ci.sh coverage`（自动配置 + 跑测 + `gcovr` 卡 75% 行覆盖率）。
可选 fuzz：`cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DDCACHE_FUZZ=ON`
（需要 clang），或直接 `bash scripts/ci.sh fuzz`（三个目标各跑 30s，`FUZZ_SECONDS` 可调）。

### 方式三：一键跑全部门禁

```bash
bash scripts/ci.sh all     # build -> asan -> tsan -> lint -> coverage -> fuzz
```

CI（`.github/workflows/ci.yml`）的六个 job 调用的就是同一份脚本，本地跑通基本等价于 CI 跑通 ——
除了平台差异（Linux vs Windows）之外，两边不会有第二套命令。

> 双远端：GitHub 为主仓（CI 跑在这里），Gitee 为国内镜像。
> Gitee 不执行 `.github/workflows/`（Gitee Go 需企业版且自定义 shell 要自备 Linux 主机），
> 所以门禁只在 GitHub 生效，详见 `docs/CODE_REVIEW_GUIDE.md` 第 6 节。
>
> CI 状态：六个 job（build / asan+ubsan / tsan / lint / coverage / fuzz）已全部实跑通过。
> ASan、UBSan、TSan 零报告；`clang-format` 全仓零 diff；
> **行覆盖率 79%**（2261 行中 1806 行，地板值 75%，`--coverage` + `gcovr`，统计 `src/` 与 `include/`）；
> **三个 libFuzzer 目标**（json / url / HTTP 请求行）按不变式断言跑，**首轮就撞出并修掉了
> 一个 P0 级缺陷**：JSON 字段扫描不认字符串边界，值里出现 `"found"` 片段时会把「命中」读成「未命中」。
> 首跑记录见 `docs/REVIEW_REPORT-2026-11-01.md` 第十一节 —— 那次全红，值得一读；
> fuzz 那一轮见第十三节，**节点间 TLS** 见第十四节，
> **慢客户端隔离**（先红后绿）见第十六节，**`\uXXXX` 代理对**见第十七节，
> **压测口径**见第十八节。
> 10 个测试二进制（含 7 组 TLS 用例，其中 3 组断言的是「必须失败」）。


### 单机演示

```bash
./node --self 127.0.0.1:8001 --peers 127.0.0.1:8001

# 另开终端
curl http://127.0.0.1:8001/api/scores/k1        # → {"value":"mock:k1","found":true}
curl http://127.0.0.1:8001/api/scores/miss:x    # → {"value":"","found":false}
curl http://127.0.0.1:8001/stats                 # → 命中率 / 回源次数 / error_count
curl http://127.0.0.1:8001/metrics               # → Prometheus 文本格式
```

**返回码语义**：`200 + {"found":false}` 表示「确定不存在」（会被做短 TTL 负缓存防穿透）；
`503 + {"error":"..."}` 表示「没问到」（超时/连接失败/对端 5xx），**错误结果不会写入缓存**，
客户端可以安全重试。两者绝不能混为一谈——把超时当成不存在，一次抖动就会变成一段时间的错误答案。

### 内部 token（多节点部署必配）

```bash
export DCACHE_TOKEN=$(head -c 24 /dev/urandom | base64)   # 所有节点用同一个值
./node --self 127.0.0.1:8001 --peers ...
```

不配置时，`X-DCache-Hops` 只能按「来源 IP 属于 peers」判定——同机或同网段的任何进程都能伪造它，
进而绕过一致性哈希把流量全压到数据源。配置后改为**只认 token**（常量时间比较），IP 不再参与判定。

`--token` 参数也支持，但命令行参数在 `ps aux` 里对同机所有用户可见，**优先用环境变量**。

> token 解决「你是谁」，不解决「谁在听」。要关掉窃听这个缺口，见下一节。

### 节点间 TLS

```bash
# 自签一张带 IP SAN 的证书（生产请用真 CA 签发）
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout node.key -out node.crt \
  -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

export DCACHE_TOKEN=$(head -c 24 /dev/urandom | base64)
./node --self 127.0.0.1:8001 --peers 127.0.0.1:8001,127.0.0.1:8002 \
       --tls-cert node.crt --tls-key node.key --tls-ca node.crt

# 客户端也要用 TLS（同一张证书当 CA）
curl --cacert node.crt https://127.0.0.1:8001/api/scores/k1
```

三个参数必须同时给：本项目的监听口同时服务外部客户端和集群节点，开了 TLS 就是整口 TLS，
所以「入站加密」必然要求「出站也加密」。只配一半的节点能起来、但对方一连就握手失败，
现象是随机 503，所以启动阶段直接拒绝。

证书校验是**强制**的（`SSL_VERIFY_PEER` + CA），没有「先不校验、跑通再说」的开关——
那种开关会让后面所有「已启用 TLS」的结论失去意义。按 IP 连接时走 iPAddress SAN 校验，
按域名连接时走 SNI + DNS SAN，两条分支都覆盖了（`tests/test_tls.cpp` 里有对应反向用例）。

`-DDCACHE_TLS=OFF` 可以编出不带 OpenSSL 的版本；此时配了 TLS 参数会启动失败并说明原因，
**不会静默退回明文**。

### 证书热加载（SIGHUP）

证书续期如果必须重启，重启就清空整个缓存——一次例行运维换来一次全量回源。
所以换证书不需要重启：

```bash
# 1. 新证书就位。用 mv 而不是 cp：cp 会留下一段「文件写了一半」的窗口，
#    而 reload 恰好可能在那时读到它（虽然失败也不会中断服务，但没必要多一次报警）
mv new.crt node.crt && mv new.key node.key

# 2. 通知进程重读（在途连接不受影响，只影响新连接）
kill -HUP <pid>
```

行为边界写清楚，免得误判：

- **失败不动旧证书**。文件写坏、路径写错、私钥与证书不匹配 → 重载失败，
  继续用旧证书服务，`dcache_tls_reload_failures_total` +1。节点不会因为一次
  错误的证书更新而停止服务。
- **在途连接不受影响**。旧上下文靠 OpenSSL 的引用计数续命，等最后一个会话关闭
  才真正释放；已建立的 keep-alive 连接可以跨过一次热加载继续用。
- **会清空客户端连接池**。CA 换了就不该复用旧连接，代价是一次短暂的重连抖动。
- **只换证书，不吊销**。没有 CRL/OCSP；证书泄露后仍需换一套证书再 reload。

### 监控指标

`GET /metrics` 输出 Prometheus 文本格式，主要指标：

| 指标 | 类型 | 含义 |
|---|---|---|
| `dcache_group_local_hit_total` / `local_miss_total` | counter | 本地命中 / 未命中（带 `group` 标签） |
| `dcache_group_hit_rate` | gauge | 本地命中率 |
| `dcache_group_source_fetch_total` / `remote_fetch_total` | counter | 本机回源 / 转发对端次数 |
| `dcache_group_error_total` | counter | 回源或转发失败（结果不入缓存） |
| `dcache_group_cache_size` | gauge | 本地缓存条目数 |
| `dcache_http_requests_total` / `requests_dropped_total` | counter | 受理的请求 / 报文非法被丢弃的请求 |
| `dcache_http_responses_total{code_class}` | counter | 2xx / 4xx / 5xx 分布 |
| `dcache_http_active_connections` | gauge | 当前挂起连接数（含 keep-alive 空闲） |
| `dcache_client_idle_connections` | gauge | 节点间转发的空闲连接数（复用是否生效） |
| `dcache_hops_rejected_total` | counter | 带 `X-DCache-Hops` 但过不了校验的次数 —— **有人在伪造内部头** |
| `dcache_tls_enabled` | gauge | 入站是否启用 TLS（1/0），一眼看出配置有没有生效 |
| `dcache_tls_handshake_failures_total` | counter | **服务端**握手失败数。抓「对端根本不是 TLS」（明文打 TLS 口）和握手报文损坏；**抓不到「对端不信任我们的证书」**——TLS 1.3 下服务端发完 Finished 就成功了，那次失败只发生在客户端 |
| `dcache_tls_reload_total` | counter | 证书热加载成功次数（SIGHUP） |
| `dcache_tls_reload_failures_total` | counter | 热加载失败次数。**旧证书仍在服务**；持续增长说明有人在反复 reload 但文件始终不对 |

其中 `requests_dropped_total` / `hops_rejected_total` / `tls_handshake_failures_total` 覆盖的都是
「根本没进业务逻辑」的请求，只看访问日志是发现不了的。`tls_enabled` 尤其重要：
它是「你以为配了 TLS」和「实际配了 TLS」之间唯一的机器可读判据。

### 连接复用

节点间转发默认复用 TCP 连接（HTTP/1.1 keep-alive）：客户端按 `host:port` 维护空闲连接池，
服务端在客户端显式发送 `Connection: keep-alive` 时保持连接，处理完把 fd 交还事件循环重新挂回 epoll。

两个关键约束：

- **空闲连接 TTL 5s < 服务端空闲回收 10s**。反过来池子里会堆满「服务端早已关闭、我们还以为可用」的死连接。
- **pipelining 一律不复用**。一条连接上若已收到第二条请求，交接时会丢失这些字节，宁可关连接让客户端重连。

### 三节点演示

```bash
./node --self 127.0.0.1:8001 --peers 127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003 &
./node --self 127.0.0.1:8002 --peers 127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003 &
./node --self 127.0.0.1:8003 --peers 127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003 &

# 从任意节点取数：未命中的 key 会被转发到负责节点并回填
curl http://127.0.0.1:8001/api/scores/k1
```

### 压测

一键（起节点 → 跑三轮 → 取中位数 → 收尸）：

```bash
bash scripts/ci.sh perf
```

它内部调 `scripts/bench_perf.sh`，跑三套配置，差别只在「有没有远端转发」和「会不会淘汰」：

| 配置 | 节点数 | key 空间 | 在测什么 |
|---|---|---|---|
| 单节点·全命中 | 1 | 1000 < 容量 | 本地命中路径的纯成本 |
| 三节点·全命中 | 3 | 1000 < 容量 | 多一层一致性哈希判定的代价 |
| 三节点·会淘汰 | 3 | 100000 > 容量 | 淘汰 + 转发 + 回填，最接近真实负载 |

单跑一次：

```bash
./bench --target 127.0.0.1:8001 --threads 2 --requests 20000 --warmup 2000 --keys 1000
```

输出 QPS、P50/P90/P99/P999（微秒，最近秩）、`/stats`、池内空闲连接数，
最后两行是给机器读的：

```
STATS hit_rate=0.9987 cache_size=1000
RESULT qps=48213 ok=20000 failed=0 found=20000 n=20000 p50=38 p90=52 p99=94 p999=210 max=1830
```

**读数前必须知道的三件事**（口径写错的话，数字比没有更糟）：

1. `found 占比` **不是命中率**。它只表示「响应里有值」；mock 数据源对每个 key 都有值，
   所以它恒等于 1.0。真正的命中率在 `STATS hit_rate`（节点的 `local_hit / (hit+miss)`）。
2. **节点 `--capacity` 默认只有 100**，而 bench 的 `--keys` 默认 1000。不显式给
   `--capacity`，命中率上限就是 10%，压出来的是 LRU 淘汰而不是缓存。bench 会在
   `cache_size < keys` 时直接把这个上界算出来打给你。
3. **失败请求的延迟不进分位数**（失败请求的「延迟」是超时上限，混进来会让 P99 变成
   2000ms，而且失败越多 P99 越像超时值）。失败数单独报——P99 变低可能是「失败那部分
   没算」，不是变快了。

延迟统计的口径（最近秩、不做插值、微秒、只收成功请求）和「为什么分位数不能取各线程
平均」，都在 `include/latency_stats.hpp` 的注释里，并有一条会红的断言钉着
（`tests/test_latency_stats.cpp`）——分位数差一格报出来的数**依然看着很正常**，
只能靠断言。

节点可用 `--workers <n>` 调整 HTTP 工作线程数（默认 4）。

## 单元测试覆盖

| 测试 | 验证点 |
|---|---|
| `test_lru_cache` | 命中 / 未命中 / 更新 / 容量淘汰 / TTL 惰性过期 / 容量 0 / 并发读写 |
| `test_consistent_hash` | 确定性路由、节点分布均衡、新增节点迁移比例 ≤ 2/N、删除 / 空环 |
| `test_singleflight` | 100 并发同一 key 只回源 1 次、共享结果、无 inflight 泄漏、异常传播（leader 抛异常 → 等待者拿到异常而非死锁） |
| `test_json` | 转义往返、控制字符 `\u00XX`、**代理对合并（`\uD83D\uDE00` → 4 字节）与孤立代理替换 U+FFFD**、代理区两侧边界（D7FF/E000）、非转义字节逐字节透传、非法输入安全失败 |
| `test_url` | 百分号编码/解码往返、特殊字符、非法序列原样保留 |
| `test_node` | 单机本地命中 + 回源一次 + 空值缓存防穿透；极小 TTL(1/2/3ms) 边界；并发防击穿；双节点 HTTP 转发回填；handler 抛异常→500 且进程存活；Stop→Start 重启可用；移除节点后 vnode 数不变；对端报错不写负缓存且映射为 503；发完立刻 FIN 仍能收到响应；超大 Content-Length 立即断开；keep-alive 同连接第二条请求仍被服务；伪造 `X-DCache-Hops` 被拒并记账；`/metrics` 格式与取值；token 比较语义；连接池复用生效与开关 |
| `test_worker_isolation` | 慢客户端不独占 worker（另一条连接 1500ms 内必须拿到响应）、大响应分片写出字节不多不少、只发不读的连接最终被回收 |
| `test_tls` | 往返、keep-alive 复用、错误 CA 必须失败、明文客户端打 TLS 端口、IP vs 域名校验分流、双节点集群、证书热加载 7 条断言、4MB 响应的记录层分片 |
| `test_latency_stats` | 分位数口径：最近秩（P99 of 1..100 必须是 99 而不是 100）、ceiling 而非 floor（n=3 的 P50 必须是 3）、乱序输入、查询后再 Add、多线程合并、越界参数夹取 |

## 设计要点

- **一致性哈希 vs 取模**：取模 `key % N` 在节点数变化时几乎全量迁移；一致性哈希把节点映射到哈希环，每个 key 顺时针找第一个节点，新增节点只接管相邻一段（约 1/N），迁移量最小。虚拟节点缓解物理节点哈希分布不均。
- **多级缓存链路**：`Get(key)` 先查本地 LRU，命中即返回；未命中按哈希定位负责节点——自身则回源写本地，远端则经 HTTP 转发取数并回填，热点数据二次访问即本地命中。
- **singleflight 防击穿**：热点 key 过期瞬间大量并发未命中时，同一 key 只放行一次回源，其余共享结果；回源异常会用 `exception_ptr` 传播给所有等待者（对标 Go 版 panic 传递），不会死锁或泄漏。
- **缓存穿透 / 雪崩治理**：key 不存在时缓存空标记（短 TTL）；过期时间加 ±25% 随机抖动（极小 TTL 直接返回，规避模零 UB）。
- **转发防环与报文安全**：节点间转发携带 `X-DCache-Hops` 跳数，超过上限退化为本机回源，避免环配置不一致时无限转发；group/key 转发前做 URL 百分号编码，服务端解码还原。
- **事件循环 + 工作线程池（并发模型）**：epoll 事件循环负责 accept / 读请求 / **写响应**，完整的请求交给工作线程池（`--workers`，默认 4）跑 handler，同步转发 / 慢回源不会阻塞事件循环，避免「一个转发拖垮整节点」的级联串行化；**写响应也在事件循环里**（非阻塞 + `EPOLLOUT`），因为「对端不读就写不完」同样会阻塞——留在工作线程里的话，一个只发请求不读响应的客户端就能按死一个 worker，四个就够让整节点对谁都不响应。两条路径的 socket 写都带 `MSG_NOSIGNAL`，对端节点重启时不会因 SIGPIPE 崩溃。
- **内部信任边界**：`X-DCache-Hops` 这类内部语义头只对「通过 token 校验」的请求生效（未配置 token 时退回来源 IP 判定并告警）。判据做成「配了 token 就只认 token」而不是二者取或——取或等于取最弱的那一个，只要还有一条可绕过的通道，攻击者就会走那条。
- **可观测性覆盖「没进业务逻辑」的请求**：报文非法的丢弃数、伪造内部头的次数都单独计数。这类请求到不了 handler，如果只按业务侧统计，攻击在监控上就是隐形的。
- **不支持更新 / 删除**：沿袭 groupcache「缓存即静态视图」哲学，数据以回源为准，靠 TTL 自然过期。

## 面试高频追问速答

- **一致性哈希和取模区别？虚拟节点数量怎么权衡？** 见上文「设计要点」。虚拟节点越多分布越均衡，但环内存与查找代价上升，150 是常见折中。
- **穿透 / 击穿 / 雪崩分别是什么？如何治理？** 穿透=查询不存在的数据（空值短 TTL / 布隆过滤器）；击穿=热点 key 过期瞬间（singleflight）；雪崩=大量 key 同时过期（过期抖动）。
- **groupcache 为什么不支持更新 / 删除？** 缓存即静态视图，不写回、不主动失效。
- **节点上下线数据迁移与预热？** 一致性哈希把迁移限制在相邻段；预热可离线回放热 key。
- **LRU O(1) 与锁粒度？** 哈希表 + 双向链表配合 `splice`；多线程用一把互斥锁，进阶可分段锁 / 分片。