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

### 方式三：一键跑全部门禁

```bash
bash scripts/ci.sh all     # build -> asan -> tsan -> lint
```

CI（`.github/workflows/ci.yml`）的四个 job 调用的就是同一份脚本，本地跑通基本等价于 CI 跑通 ——
除了平台差异（Linux vs Windows）之外，两边不会有第二套命令。

> 双远端：GitHub 为主仓（CI 跑在这里），Gitee 为国内镜像。
> Gitee 不执行 `.github/workflows/`（Gitee Go 需企业版且自定义 shell 要自备 Linux 主机），
> 所以门禁只在 GitHub 生效，详见 `docs/CODE_REVIEW_GUIDE.md` 第 6 节。
>
> CI 状态：四个 job（build / asan+ubsan / tsan / lint）已全部实跑通过。
> ASan、UBSan、TSan 零报告；`clang-format` 全仓零 diff。首跑记录见
> `docs/REVIEW_REPORT-2026-11-01.md` 第十一节 —— 那次全红，值得一读。


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

> 局限：节点间仍是明文 HTTP，token 在网络上是可见的。它解决的是「外部客户端伪造内部语义」，
> 不能替代传输层加密；跨不可信网络部署需在节点之间加 TLS 或做内网隔离。

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

后三项是这次新加的观测点：`requests_dropped_total` 和 `hops_rejected_total` 覆盖的是
「根本没进业务逻辑」的请求，只看访问日志是发现不了的。

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

```bash
./bench --target 127.0.0.1:8001 --threads 8 --requests 20000 --keys 1000
```

输出 QPS、found 占比，并回显目标节点的 `/stats`（本地命中率 / 回源次数）。
节点可用 `--workers <n>` 调整 HTTP 工作线程数（默认 4）。

## 单元测试覆盖

| 测试 | 验证点 |
|---|---|
| `test_lru_cache` | 命中 / 未命中 / 更新 / 容量淘汰 / TTL 惰性过期 / 容量 0 / 并发读写 |
| `test_consistent_hash` | 确定性路由、节点分布均衡、新增节点迁移比例 ≤ 2/N、删除 / 空环 |
| `test_singleflight` | 100 并发同一 key 只回源 1 次、共享结果、无 inflight 泄漏、异常传播（leader 抛异常 → 等待者拿到异常而非死锁） |
| `test_json` | 转义往返、控制字符 `\u00XX` 及高位码点 UTF-8 编解码、非法输入安全失败 |
| `test_url` | 百分号编码/解码往返、特殊字符、非法序列原样保留 |
| `test_node` | 单机本地命中 + 回源一次 + 空值缓存防穿透；极小 TTL(1/2/3ms) 边界；并发防击穿；双节点 HTTP 转发回填；handler 抛异常→500 且进程存活；Stop→Start 重启可用；移除节点后 vnode 数不变；对端报错不写负缓存且映射为 503；发完立刻 FIN 仍能收到响应；超大 Content-Length 立即断开；keep-alive 同连接第二条请求仍被服务；伪造 `X-DCache-Hops` 被拒并记账；`/metrics` 格式与取值；token 比较语义；连接池复用生效与开关 |

## 设计要点

- **一致性哈希 vs 取模**：取模 `key % N` 在节点数变化时几乎全量迁移；一致性哈希把节点映射到哈希环，每个 key 顺时针找第一个节点，新增节点只接管相邻一段（约 1/N），迁移量最小。虚拟节点缓解物理节点哈希分布不均。
- **多级缓存链路**：`Get(key)` 先查本地 LRU，命中即返回；未命中按哈希定位负责节点——自身则回源写本地，远端则经 HTTP 转发取数并回填，热点数据二次访问即本地命中。
- **singleflight 防击穿**：热点 key 过期瞬间大量并发未命中时，同一 key 只放行一次回源，其余共享结果；回源异常会用 `exception_ptr` 传播给所有等待者（对标 Go 版 panic 传递），不会死锁或泄漏。
- **缓存穿透 / 雪崩治理**：key 不存在时缓存空标记（短 TTL）；过期时间加 ±25% 随机抖动（极小 TTL 直接返回，规避模零 UB）。
- **转发防环与报文安全**：节点间转发携带 `X-DCache-Hops` 跳数，超过上限退化为本机回源，避免环配置不一致时无限转发；group/key 转发前做 URL 百分号编码，服务端解码还原。
- **事件循环 + 工作线程池（并发模型）**：epoll 事件循环只负责 accept / 读请求，完整请求交给工作线程池（`--workers`，默认 4）处理，同步转发 / 慢回源不会阻塞事件循环，避免「一个转发拖垮整节点」的级联串行化；写响应带 `MSG_NOSIGNAL`，对端节点重启时不会因 SIGPIPE 崩溃。
- **内部信任边界**：`X-DCache-Hops` 这类内部语义头只对「通过 token 校验」的请求生效（未配置 token 时退回来源 IP 判定并告警）。判据做成「配了 token 就只认 token」而不是二者取或——取或等于取最弱的那一个，只要还有一条可绕过的通道，攻击者就会走那条。
- **可观测性覆盖「没进业务逻辑」的请求**：报文非法的丢弃数、伪造内部头的次数都单独计数。这类请求到不了 handler，如果只按业务侧统计，攻击在监控上就是隐形的。
- **不支持更新 / 删除**：沿袭 groupcache「缓存即静态视图」哲学，数据以回源为准，靠 TTL 自然过期。

## 面试高频追问速答

- **一致性哈希和取模区别？虚拟节点数量怎么权衡？** 见上文「设计要点」。虚拟节点越多分布越均衡，但环内存与查找代价上升，150 是常见折中。
- **穿透 / 击穿 / 雪崩分别是什么？如何治理？** 穿透=查询不存在的数据（空值短 TTL / 布隆过滤器）；击穿=热点 key 过期瞬间（singleflight）；雪崩=大量 key 同时过期（过期抖动）。
- **groupcache 为什么不支持更新 / 删除？** 缓存即静态视图，不写回、不主动失效。
- **节点上下线数据迁移与预热？** 一致性哈希把迁移限制在相邻段；预热可离线回放热 key。
- **LRU O(1) 与锁粒度？** 哈希表 + 双向链表配合 `splice`；多线程用一把互斥锁，进阶可分段锁 / 分片。