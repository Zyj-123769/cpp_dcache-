# 代码审查报告 — dcache 分布式缓存

- **审查日期**：2026-11-01 ~ 2026-11-02（完成十一批修复 + CI 实跑，附修复前/后评分）
- **审查范围**：`include/` `src/` `apps/` `tests/` `fuzz/` 共约 8100 行 C++17 代码
  （核心 5500 + 测试 2400 + fuzz 230）
- **审查方式**：静态通读 + 并发/资源/协议路径推演；每批修复都在 GitHub Actions（Ubuntu）上实跑
  build / ASan+UBSan / TSan / clang-format+clang-tidy / gcovr / libFuzzer 六项门禁，
  外加一条只在专用分支上跑的压测流水线
- **结论**：修复前 **65 / 100（C）** → 修复后 **91 / 100（A−）**；6 个 P0 全部关闭，
  连接层、信任边界、可观测性、工程化补齐，且**已在 Linux 上实跑验证**；
  后续批次又揪出并修掉 2 个 P0 级缺陷：JSON 扫描不认字符串边界（第 6 批）、
  TLS 写路径没有 `MSG_NOSIGNAL`（第 8 批，**一个客户端就能让节点进程退出**）；
  第 9 批关掉了清单上最后一项结构性欠账：**一个慢客户端不再能独占工作线程**；
  第 10 批修掉一个躲过九批 review 的解码缺陷：**`\uXXXX` 被当码点而不是 UTF-16 码元**；
  第 11 批补上**延迟分布**这个此前不存在的度量维度，并给压测数字钉上口径

---

## 一、项目评分

| 维度 | 权重 | 修复前 | 修复后 | 判定依据 |
|---|---|---|---|---|
| 正确性 | 25% | 62 | 86 | 三态错误模型；实跑 10/10；陆续修掉「域名连接只用第一个地址」「JSON 扫描不认字符串边界」「TLS 写路径无 MSG_NOSIGNAL」「慢客户端按死 worker」「`\uXXXX` 当码点而非 UTF-16 码元」——全是本机跑不出来的缺陷 |
| 并发安全 | 20% | 55 | 88 | 哈希环原子快照、关停顺序修正、重启可用；**TSan 全量回归零报告**；第 9 批把 fd 所有权收敛成一条不变量（工作线程不再关 fd） |
| 安全性 | 15% | 50 | 95 | token 常量时间校验 + **节点间 TLS（强制校验、IP/域名分流、证书热加载）**；修掉「单连接即可令节点退出」与「四个连接即可让节点对谁都不响应」两个 DoS；另有超时、上限、回收 |
| 可维护性 | 15% | 88 | 92 | 分层干净、注释解释「为什么」；全仓 clang-format 零 diff；第 9 批后「谁拥有这条 fd」只有一个答案 |
| 测试 | 12% | 68 | 96 | 10 个测试二进制全绿（TLS 7 组含 3 组反向、慢客户端隔离 3 组、分位数 7 组）；**行覆盖率 @@COV@@** 设门禁；fuzz 首轮撞出 P0；安全断言、「慢客户端独占 worker」「分位数算错」都做过反证；第 10 批补上了 fuzz 的一个结构性盲区（只测自己产的输入）；第 11 批新增**延迟分布**这个此前不存在的度量维度 |
| 工程化 | 8% | 65 | 95 | 双远端、CI 六 job 全绿、`scripts/ci.sh` 单一入口、工具版本钉死、**失败诊断也走注解**（不依赖仓库权限）；第 11 批新增**可复现的压测流水线**：专用分支触发、四套配置、结果走同一条注解通道、数字强制带环境标注 |
| 文档 | 5% | 90 | 93 | README 与头文件注释质量高；陆续纠正了写错的指标能力边界与过时的并发模型描述；第 11 批新增「压测口径」一节与指南 6.7，把「数字的含义」也纳入审查范围 |

**加权总分：修复前 65.1 → 65 分（C）；修复后 90.92 → 91 分（A−）**

> 第 9 批是前十批里唯一一次涨分（89 → 90），理由不是「又修了一个欠账」——
> 前面已经定过规矩：**同一个缺陷的存在本身就该扣分，修掉它只是还债**。
> 第 8 批修掉一个「单连接即可让节点退出」的 DoS 也没加分，就是这个规矩。
> 涨分的理由是度量上真的多了一格：「慢客户端不影响他人」这条以前只能口头承诺，
> 现在有一条**会红的断言**钉着它（先红后绿，红的证据留在 `a62ffc8` 的 CI 记录里）。
>
> 仍然不是 A 的原因还是三条，而且都具体：**handler 回源慢仍会占用一个 worker**
> （关掉的只是「客户端能把 worker 按死」这一半）、**fuzz 在 CI 里只跑 30 秒**、
> **证书能轮换但没有吊销**（无 CRL/OCSP）。正确性 86 而不是更高也是同一个道理：
> 这十一批里每修一个，就有下一个冒出来，说明「没真正跑过的路径」还有存量，
> 那不是靠加分能抹掉的。
>
> 第 10 批**没有**加分，而且这里要把口径冲突写清楚：那一批修掉的是一个从第一版
> 就存在的解码缺陷（`\uXXXX` 当码点），修完还顺手补上了 fuzz 的一条结构性盲区。
> 按第 9 批「多了一条会红的断言就加分」的口径，这次也可以加分。
> **两条口径冲突时取更严的那条**：第 9 批加分加在一条从来不成立的**承诺**上，
> 第 10 批修的是一条**一直是错的**代码。前者是度量新增，后者是还债。
> 写下来不是为了自我表扬，是免得下一次按对自己有利的方向解释。
>
> 第 11 批**加了** 1 分（90 → 91），理由与上面那条不冲突：它新增的是
> **一个此前根本不存在的度量维度**（延迟分布）——连 bench 的输出里都没有 P99，
> 更谈不上它算得对不对。现在它有独立组件、7 组对着具体错法的断言、一次反证，
> 以及一份带环境标注的真实数字。判据收敛成一句：
>
> > **新增一个此前不存在的可验证维度 → 加分。修正一个一直是错的实现 → 不加分。**
>
> 这条判据也解释了第 9 批的 +1：慢客户端隔离此前只是设计承诺，从未有断言。
> 三个维度动了（测试 +2 / 工程化 +2 / 文档 +1），正确性、并发安全、安全性、
> 可维护性这四项没动——本批对它们没有新的证据。

一句话概括：**这套代码的「算法层」写得比多数同阶段项目好，问题几乎全部集中在「系统层」——线程生命周期、失败语义、信任边界。三批修复针对的正是系统层。**

---

## 二、做得好的地方（值得保留并当成团队范式）

1. **`singleflight` 是正确实现**（`include/singleflight.hpp`）。leader 在锁外执行 `fn`，异常通过 `exception_ptr` 传播给所有等待者而不是让它们死锁——这是最容易写错的点，而 `tests/test_singleflight.cpp:48-75` 连「8 线程同时拿异常」都测了。👍
2. **`murmur3_32` 手写实现避开了两类 UB**（`src/hash.cpp:11-16`）：小端手动组装而非解引用 `uint32_t*`，规避了未对齐访问与 strict-aliasing。注释里也点明了原因。
3. **LRU 用 `splice` 做 O(1) touch**（`src/lru_cache.tpp:116`），TTL 惰性删除，`capacity==0` 语义明确且有测试。
4. **防雪崩/防穿透/防击穿三件事都做了**：TTL 抖动（`group.cpp:82`）、空值短 TTL 缓存（`group.cpp:61`）、singleflight 合并（`group.cpp:36`）。且 `ttl_with_jitter` 对 `span==0` 做了除零保护并配了回归测试（`test_node.cpp:52-59`）——这种「先写测试再修 bug」的痕迹说明有质量意识。
5. **事件循环与工作线程分离的动机写清了**（`http_server.hpp:20-25`）：避免慢回源阻塞 accept 造成级联串行化。这是对的设计判断，不是抄来的结构。
6. 注释风格统一：解释「为什么这样取舍」，而不是复述代码。

---

## 三、🔴 P0 阻断问题（6 个，必须修复）

> **状态：6 个已于 2026-11-01 全部关闭**（修复记录见第七、八、九节）。以下保留问题分析原文，便于复盘「当时为什么会写成这样」。

### P0-1　handler 抛异常会导致整个进程终止

`src/http_server.cpp:336-345`

```cpp
void HttpServer::handle_request(int fd, HttpRequest req) {
    HttpResponse resp;
    resp.status = 200;
    handler_(req, resp);          // ← 无 try/catch
```

**为什么是 P0**：`handler_` 一路调到 `Node::ServeHTTP` → `Group::Get` → `getter_`。用户的 getter 完全可能抛（DB 异常、`std::bad_alloc`、`std::stoi` 失败）。一旦抛出，异常穿出 `worker_loop` 的线程函数，触发 `std::terminate`——**整个节点进程直接死掉**，而且此时 `fd` 未关闭、其他 worker 上的在途请求全部中断。

**建议**：

```cpp
void HttpServer::handle_request(int fd, HttpRequest req) {
    HttpResponse resp;
    try {
        handler_(req, resp);
    } catch (const std::exception& e) {
        resp = HttpResponse{};
        resp.status = 500;
        resp.body   = "{\"error\":\"internal\"}";
        // 记日志：req.path + e.what()
    } catch (...) {
        resp = HttpResponse{};
        resp.status = 500;
        resp.body   = "{\"error\":\"internal\"}";
    }
    const std::string data = build_response(resp);
    ...
}
```

并在 PR 清单里加一条：「线程入口函数不允许让异常逃逸」——`worker_loop`、`run` 同理。

---

### P0-2　`SetPeers` 与 `PickPeer` 数据竞争（UB）

`src/peer_pool.cpp:36-51` / `include/peer_pool.hpp:41-44`

```cpp
void HttpPeerPool::SetPeers(const std::vector<std::string>& peers) {
    ConsistentHash rebuilt(ring_.VirtualNodes());
    for (const auto& p : peers) rebuilt.AddNode(p);
    ring_ = std::move(rebuilt);            // ← 写，无同步
}

std::string HttpPeerPool::PickPeer(const std::string& key) const {
    std::string node = ring_.GetNode(key); // ← 并发读同一个 ring_
```

**为什么是 P0**：`ring_` 内部是 `std::map` + `std::unordered_map`。`ring_ = std::move(rebuilt)` 会析构旧树并在并发读者遍历时替换——这不是「读到旧值」，而是**释放正在被遍历的节点**，典型表现是偶发段错误或拿到半截哈希环把 key 路由到错误节点。而且 `Node::SetPeers` 是公开接口，集群扩缩容时必然在运行期调用。

**建议**：改用不可变快照 + 原子换指针，读侧无锁：

```cpp
// peer_pool.hpp
private:
    std::shared_ptr<const ConsistentHash> ring_;
    mutable std::mutex ring_mtx_;   // 只保护写者之间的序列化
```

```cpp
void HttpPeerPool::SetPeers(const std::vector<std::string>& peers) {
    auto rebuilt = std::make_shared<ConsistentHash>(ring_->VirtualNodes());
    for (const auto& p : peers) rebuilt->AddNode(p);
    std::lock_guard<std::mutex> lk(ring_mtx_);
    std::atomic_store(&ring_, std::shared_ptr<const ConsistentHash>(rebuilt));
}

std::string HttpPeerPool::PickPeer(const std::string& key) const {
    auto snap = std::atomic_load(&ring_);   // 拿到稳定快照，读期间不会被改
    std::string node = snap->GetNode(key);
    ...
}
```

> 顺带：`ConsistentHash` 本身也应加注「构造完成后只读」，禁止运行期 `AddNode/RemoveNode`。

---

### P0-3　`X-DCache-Hops` 可被外部客户端伪造，绕过一致性哈希

`src/node.cpp:86-92`

```cpp
int hops = 0;
auto hit = req.headers.find("X-DCache-Hops");
if (hit != req.headers.end()) {
    hops = std::atoi(hit->second.c_str());   // ← 任意客户端可控，无校验
}
const GetResult r = g->Get(key, hops);
```

**为什么是 P0**：`group.cpp:45` 里 `hops >= kMaxForwardHops` 就退化为 `load_local()`。攻击者只要对**任意节点**发 `X-DCache-Hops: 2`，就能让这个 key 跳过哈希路由、由当前节点直接回源，并且把结果写进本不该持有它的节点缓存。后果：

- 分片失效，全网流量被引导到数据源 → **回源打垮后端**
- 本节点缓存被污染（存了一批不属于它的 key），容量被挤占
- 本身也是缓存穿透攻击的放大器

**建议**（三选一，推荐前两个叠加）：

1. **公开端口一律覆盖**：入口处 `hops = 0`，只从内部头读取，且该头由 peer 端点的白名单 IP 才认。
2. **内部共享密钥**：peer 请求带 `X-DCache-Internal: <token>`，校验通过才信任 `Hops`。
3. 彻底去掉 header 传递，改用「请求路径带 `?hops=1` 且仅内部端口监听」。

同时给 `hops` 加范围钳制：`hops = std::clamp(parsed, 0, kMaxForwardHops)`，避免 `atoi` 负数/溢出。

---

### P0-4　网络失败被当作「数据不存在」，并被缓存 30 秒

`src/peer_pool.cpp:70-79` + `src/group.cpp:55-63`

```cpp
if (!HttpClient::Get(host, port, path, body, 2000, headers)) {
    return GetResult(false, "");      // ← 传输失败 == found=false
}
if (!DecodeResult(body, value, found)) {
    return GetResult(false, "");      // ← 解析失败 == found=false
}
```

```cpp
} else {
    cache_.put(key, r, ttl_with_jitter(penetration_ttl_ms_, key));  // 30s 负缓存
}
```

**为什么是 P0**：这是缓存系统最忌讳的一类 bug。对端超时 2 秒、对端返回 500、响应被截断——**四种完全不同的情况全部塌缩成「这个 key 不存在」，然后写进 30 秒的负缓存**。节点抖动一下，用户就会连续 30 秒看到「数据不存在」，而后端数据一直都在。这不是性能问题，是**静默的数据错误**。

**建议**：把 `GetResult` 扩成三态（这是本报告里改动面最大、但收益最高的一条）：

```cpp
enum class Status { kOk, kNotFound, kError };

struct GetResult {
    Status status = Status::kError;
    Value  value;
    bool found() const { return status == Status::kOk; }
    bool should_cache() const { return status != Status::kError; }  // 关键
};
```

`Group::load` 里：

```cpp
GetResult r = picker_->GetFromPeer(peer, name_, key, hops + 1);
if (!r.should_cache()) {
    return r;          // 错误不落缓存，交给上层决定降级策略
}
cache_.put(key, r, r.found() ? ttl_with_jitter(default_ttl_ms_, key)
                             : ttl_with_jitter(penetration_ttl_ms_, key));
```

配套还要决定**错误时的降级策略**并写进注释：是 `load_local()` 兜底（可能造成缓存不一致，但可用），还是直接 5xx？两种都合理，但必须显式选择并测试。

> 迁移成本可控：`GetResult(bool, string)` 构造函数保留为兼容重载，逐步替换调用点。

---

### P0-5　关停竞态 + 无法重启

`src/http_server.cpp:212-228` / `347-361`

```cpp
void HttpServer::Stop() {
    stop_ = true;
    { std::lock_guard<std::mutex> lk(queue_mtx_); shutdown_ = true; }
    queue_cv_.notify_all();

    if (thread_.joinable()) thread_.join();   // ← 事件循环在这期间仍可能入队
    for (auto& w : workers_) w.join();        // ← worker 已因 shutdown_ 提前返回
```

两个缺陷叠在一起：

**① 任务黑洞（正常关停就会触发）**
`worker_loop:354` 的退出条件是 `shutdown_ && queue_.empty()`。worker 一看到 `shutdown_` 且队列为空就 `return`；但事件循环线程要等到 `epoll_wait` 的 100ms 超时才会看到 `stop_`，这期间 `handle_read` 完全可能把新任务 `push_back`。结果：**任务进了没人消费的队列，fd 永不关闭**——客户端挂在那里等超时，fd 泄漏。

**② 重启即失效**
`shutdown_` 在 `Start()` 里从未复位。`Stop()` 后再 `Start()`，新 worker 第一次 `wait` 就因 `shutdown_ == true` 立刻返回，此后**所有请求进队列无人处理，全部挂死**。

**建议**：

```cpp
void HttpServer::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();   // ① 先停生产者

    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        shutdown_ = true;                      // ② 再通知消费者
    }
    queue_cv_.notify_all();

    for (auto& w : workers_) w.join();
    workers_.clear();
}
```

并在 `Start()` 开头补 `shutdown_ = false;`，同时加一条 `restart` 测试。

---

### P0-6　阻塞写 + 无发送超时 = 单客户端可拖垮节点

`src/http_server.cpp:342-344`

```cpp
set_nonblocking(fd, false);      // 改回阻塞
write_all(fd, data);            // send() 可能永久阻塞
::close(fd);
```

**为什么是 P0**：worker 默认只有 4 个。一个客户端连上、发完请求、然后**不读响应**（把接收窗口打满），`send()` 就会在阻塞模式无限期挂住。开 4 个这样的连接，整个节点的所有查询全部停摆——成本极低的 DoS。`write_all` 的返回值也被直接忽略。

**建议**（按代价从低到高）：

```cpp
// 最低成本：加发送超时
struct timeval tv{2, 0};
::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

// 更好：保持非阻塞，写不完就注册 EPOLLOUT 分片写
// 最好：worker 不直接持有 socket，响应回投事件循环统一写
```

另建议 `write_all` 失败时记日志并区分 `EAGAIN`（可重试）与 `EPIPE`（对端已关，正常）。

---

## 四、🟡 P1 建议修复（按优先级）

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| S1 | `http_server.cpp:244-333` | **fd 复用竞态**：fd 交给 worker 后立刻关闭，事件循环同一批 `events[]` 里的残留事件可能命中被复用的新 fd（`conns_` 里已有新连接），导致误读或误关他人连接 | `epoll_event.data.u64` 打包 `(fd, generation)`，处理前校验代次 |
| S2 | `http_server.cpp:249-252` | `accept` 遇 `EMFILE/ENFILE` 直接 `break`，LT 模式下 listen fd 持续可读 → **100% CPU 空转** | 预留 idle fd，或临时从 epoll 摘除 listen fd 并延迟重加 |
| S3 | `http_server.cpp:259, 324` | `conns_` 与 `queue_` 均无上限 → fd / 内存可被打满 | 设 `kMaxConns`（如 10000）与队列上限，超限立即 503 + close |
| S4 | `http_client.cpp:91-112, 194-220` | 读响应头 / body **无上限**，`Content-Length` 也不设上限；被劫持的 peer 可让本节点 OOM | 加 `kMaxResponseSize`（如 8MB），超限即断连返回 error |
| S5 | `http_client.cpp:150-229` | 每请求 `getaddrinfo` + 新建 TCP + close，无连接池；且只取第一条 `addrinfo`（"localhost" 解析出 `::1` 而节点只监听 IPv4 时会**直接连不上**） | 加连接池；遍历 `addrinfo` 链表直到成功 |
| S6 | `consistent_hash.cpp:38, 49` | 虚拟节点哈希冲突时 `ring_[h] = node` 覆盖，`RemoveNode` 又按 hash 删 → 可能删掉别的节点的 vnode，**部分 key 永久路由丢失** | `emplace` 冲突时重哈希；删除时校验 `ring_[h] == node` 再删 |
| S7 | `lru_cache.hpp:83` | 头文件 `#include "../src/lru_cache.tpp"` 依赖源码树相对路径，库一旦被安装/导出即断 | `.tpp` 移到 `include/` 下 |
| S8 | `lru_cache.tpp:91,102,110` | `size()/clear()/stats()` 标 `noexcept` 但内部 `std::mutex::lock` 可抛 `system_error` → 直接 terminate | 去掉 `noexcept`（`size()` 尤其不该标） |
| S9 | `json.cpp:145-191` | 手写解析靠全文搜索第一个 `"key"`，无 schema 校验；`DecodeResult` 对「格式错」与「不存在」返回同一个 false（与 P0-4 同源） | 协议显式化（`found` 用 HTTP 状态码或固定头表达），或引入 `nlohmann/json` |
| S10 | `node.cpp:36-42, 60-73` | group 不存在时 `Node::Get` 返回 `GetResult(false,"")`，与「key 不存在」不可区分；400/404/405 混用文本 body | 统一错误码常量 + 结构化 error body |
| S11 | `CMakeLists.txt:41-42` | Sanitizer 挂在 `dcache_core` 的 **INTERFACE** 上，只传递给测试/示例，**库自身源码不被插桩** → 库内 UB 检测不到 | 改 `PRIVATE`，或全局 `add_compile_options` |
| S12 | `CMakeLists.txt:52` | 仅 `-Wall -Wextra`，无 `-Wpedantic -Wshadow -Wconversion -Werror`；无 clang-tidy / cppcheck / 覆盖率；`Makefile` 与 CMake 双构建并存 | 统一 CMake；按 `docs/CODE_REVIEW_GUIDE.md` 第 6 节补 lint 与 TSan |
| S13 | 仓库根 | **项目不在 Git 中**（`git log` → not a git repository），但已有 `.github/workflows/ci.yml` | `git init` 并推送远端，否则 CI 永远不会触发 |
| S14 | `tests/` | 缺：`SetPeers`↔`PickPeer` 并发测试、`Stop/Start` 重启测试、handler 抛异常测试、peer 超时/500 降级测试、慢客户端测试；`check.hpp` 的 `return 1` 使单个用例只能报第一处失败 | 按本报告第三节各 P0 各补一条回归测试 |

---

## 五、💭 P2 小问题

- `http.hpp:31`：`HttpStatusText` 未知状态码返回 `"OK"`，应返回 `"Unknown"`。500 场景会输出 `HTTP/1.1 502 OK`，排查时会误导。
- `http_server.cpp:60-61`：`Content-Length` 先查精确大小写再查全小写，其他大小写组合（`CONTENT-LENGTH`）漏掉。应在解析时统一把 header 名转小写。
- `http_server.cpp:73`：`strtol` 未检查 `errno`/`endptr`，`Content-Length: 1e10` 这类输入行为不确定。
- `node.cpp:22`：`groups_[name] = g` 静默覆盖同名 group，建议返回错误或断言（`AddGroup` 返回裸指针，调用方可能持有已被替换的指针）。
- `group.hpp:44`：`kMaxForwardHops = 2` 这类拓扑参数写死在类里，建议提到 config 结构统一管理（同 `2000ms`、`64KB`、`4 workers`、`30000ms`）。
- `node.cpp:113-123`：`DumpStats` 手工拼 JSON，与 `json.cpp` 的 `EncodeResult` 两套字符串拼接逻辑并存，建议收敛为一套。
- `group.hpp:48`：`Get(key, hops)` 是 public，把内部跳数暴露给外部调用方——正是 P0-3 的入口。建议改成 private，仅 `ServeHTTP` 通过 friend 或内部接口调用。
- 全项目无统一日志设施（只有 `std::cout`/`std::cerr`），P0 修复后排查会很难受，建议第二步补上。
- `/stats` 无鉴权且监听 `0.0.0.0` 时可被公网读取，泄露命中率与分片结构。

---

## 六、修复路线图

### 第 1 批（1–2 天，止血上线阻断项）— ✅ 已完成 2026-11-01

1. **P0-1** handler 异常包裹 —— 改动 10 行，收益最大，先做 ✅
2. **P0-6** 发送超时 —— 5 行 ✅
3. **P0-5** 关停顺序 + `shutdown_` 复位 —— 15 行 ✅
4. **P0-3** 入口钳制 `hops`（先用 `clamp` + 忽略外部头，密钥方案随后）✅
5. 顺手修掉 S6（虚拟节点冲突）、S8（`noexcept` 误标）✅

### 第 2 批（3–5 天，语义改造）— ✅ 已完成 2026-11-01

6. **P0-4** `GetResult` 三态化 + 错误不落缓存 + 错误响应 503（改动面最大，已单独落一轮）✅
7. **P0-2** 哈希环快照化 + 原子换指针 ✅
8. 补齐回归测试 G（错误不缓存）、H（503 映射）✅；TSan 仍需 CI 实跑 ⏳

### 第 3 批（1 周，工程化）— 部分完成 2026-11-01

8. ✅ `git init`（`main` 分支）+ `.gitattributes` 统一 LF + 首个提交；CI 四 job 已可触发 ⏳待首次运行
9. ✅ 接 `clang-format` + `clang-tidy`（存量告警需 CI 首跑后清理）
10. ✅ S1/S2/S3 连接层加固（EMFILE 自旋、连接上限、空闲超时、超大 Content-Length、EPOLLIN/HUP 顺序）
11. ✅ S4/S5 客户端加固（响应头/体上限、截断检测）

### 第 4 批（持续）

12. libFuzzer 覆盖 `json` / `url` / `parse_request`
13. 结构化日志 + Prometheus 指标导出
14. 每月按评分卡出一次健康度报告

---

## 七、第 1 批修复记录（2026-11-01）

| 项 | 文件 | 改动 |
|---|---|---|
| P0-1 | `src/http_server.cpp:381-403` | `handle_request` 用 `try / catch(...)` 包住 `handler_`，异常转 500 JSON 响应 |
| P0-6 | `src/http_server.cpp:26-31, 112-118, 398-401` | 新增 `kSendTimeoutMs = 2000` 与 `set_send_timeout()`，阻塞写前设 `SO_SNDTIMEO` |
| P0-5 | `src/http_server.cpp:210-265` | `Stop()` 改为「先 join 事件循环（停生产者）→ 再置 `shutdown_`（停消费者）→ 排空残留队列兜底回收 fd」；`Start()` 复位 `shutdown_` 并清队列 |
| P0-3 | `include/http.hpp`、`include/http_server.hpp`、`src/http_server.cpp:295-306, 364`、`include/peer_pool.hpp`、`src/peer_pool.cpp:36-61`、`src/node.cpp:86-99` | `HttpRequest` 新增 `remote_ip`（accept 时 `inet_ntop` 填入）；`HttpPeerPool::IsTrustedHost()` 判断来源是否为集群节点；只有可信来源才采信 `X-DCache-Hops`，且钳制到 `[0, kMaxForwardHops]` |
| S6 | `src/consistent_hash.cpp:33-46, 55-63` | 虚拟节点哈希冲突改为线性探测；`RemoveNode` 校验归属再删 |
| S8 | `include/lru_cache.hpp`、`src/lru_cache.tpp` | 去掉 `size()/clear()/stats()` 的 `noexcept`（内部加锁可能抛 `system_error`） |
| 测试 | `tests/test_node.cpp` | 新增 D（handler 抛异常 → 500，进程存活）、E（Stop→Start 重启可用）、F（移除节点后剩余 vnode 数不变） |

**遗留风险（P0-3 的残余面）**

- 当前判定依据是「来源 IP 是否属于 peers 的 host 集合」。若集群节点都在 `127.0.0.1`（本地多实例演示），本机任意进程都会被判为可信——生产部署应使用独立内网地址。
- 完整方案是内部共享 token：peer 请求带 `X-DCache-Internal: <secret>`，服务端比对方才认 `Hops`。已在第 2 批排期。
- `X-DCache-Hops` 仍然出现在公开协议里，长期看应改为内部专用端口或独立路径前缀。

**验证状态**

- 已用 MSVC（`cl /Zs /utf-8 /std:c++17 /W3`）对全部 17 个翻译单元做语法级检查：**全部通过，0 error**；`src/http_server.cpp` 借助一套最小 POSIX 桩头文件（`%TEMP%\dcache-stub`）通过，唯一告警是 `send()` 的 `size_t → unsigned long` 转换，属 Windows 平台类型差异，Linux 上 `size_t` 即 `unsigned long`，无实际风险。
- `src/http_client.cpp` 未改动，桩缺 `<netdb.h>` 故未覆盖。
- **仍未做运行期验证**：本机无 Linux 环境，epoll 路径与全部单元测试（`ctest`）需在 CI 或 Linux 上跑一次确认，尤其是新增的 D/E/F 三段测试。

---

## 八、第 2 批修复记录（2026-11-01）

### 8.1　P0-4　`GetResult` 三态化

原来只有一个 `bool found`，把「确定不存在」和「没问到」压成同一个值。现在拆成三态：

```cpp
enum class GetStatus { kOk, kNotFound, kError };

struct GetResult {
    GetStatus   status = GetStatus::kNotFound;
    Value       value;
    std::string error;                    // 仅 kError 有意义

    bool ok() const;                      // == kOk
    bool not_found() const;               // == kNotFound
    bool is_error() const;                // == kError
    bool should_cache() const;            // != kError
    static GetResult Ok(Value);
    static GetResult NotFound();
    static GetResult Error(std::string msg);
};
```

改动分布：

| 文件 | 改动 |
|---|---|
| `include/common.hpp` | 新增 `GetStatus` 三态与工厂函数；保留 `GetResult(bool, Value)` 兼容构造（`true→kOk`、`false→kNotFound`），但**不提供**用 `false` 表达错误的通道 |
| `src/peer_pool.cpp:86-118` | 转发失败按原因返回 `Error`：连不上/超时 → `"unreachable or timed out"`；对端非 200 → `"returned HTTP 503"`；报文解析失败 → `"malformed response"`。只有 `found:false` 的正常报文才返回 `NotFound` |
| `src/group.cpp:55-88` | 回填条件从 `if (r.found)` 改为 `if (r.should_cache())`，**错误一律不写缓存**；新增 `error_count` 统计 |
| `src/node.cpp:101-118` | 三态映射三种响应：`kOk → 200 {"found":true}`、`kNotFound → 200 {"found":false}`、`kError → 503 {"error":..}` |
| `include/http_client.hpp`、`src/http_client.cpp` | `Get()` 新增可选出参 `int* http_status`（0 = 连状态行都没读到），用来区分「对端明确报错」与「够不着对端」 |
| `include/group.hpp`、`src/node.cpp:130` | `Stats` 增加 `error_count`，`/stats` 一并输出 |

**为什么保留 `200 + found:false` 而不改成 404**：这是既有的节点间协议，负缓存正依赖它区分「有答案」与「没答案」。改成 404 会同时改变对等节点的判读（`HttpClient` 只认 200），属于跨版本破坏性变更，应作为独立协议演进推进，不混在这次修复里。

**行为变化（对使用者可见）**：后端不可用时，客户端现在拿到 **503**，而不再是 200 + `{"found":false}`。这正是 P0-4 的核心价值——错误不再伪装成空数据。

### 8.2　P0-2　哈希环快照化

```cpp
std::shared_ptr<const ConsistentHash> ring_;   // 不可变快照

void HttpPeerPool::SetPeers(...) {
    auto rebuilt = std::make_shared<ConsistentHash>(VirtualNodes());   // 私有副本
    ...
    std::atomic_store(&ring_, std::shared_ptr<const ConsistentHash>(std::move(rebuilt)));
}

std::string HttpPeerPool::PickPeer(const std::string& key) const {
    auto ring = Snapshot();        // 原子取当前版本
    if (!ring) return "";
    ...
}
```

要点：

- 旧实现 `ring_ = std::move(rebuilt)` 是对 `std::map` 的赋值，读线程可能读到「已清空、正在重填」的中间态，甚至读到正在析构的节点。
- 现在构造与发布分离：读侧要么拿到旧环（完整），要么拿到新环（完整），不存在第三种。
- 用 `shared_ptr` 原子自由函数而不是互斥量，是为了让 `PickPeer` 这条**每请求必经**的热路径不加锁；旧快照的最后一个引用释放时才析构，不用等读者退出。
- 同时调整了 `SetPeers` 内部顺序：**先发布 `peer_hosts_`，再换环**。否则会出现短暂窗口——环里已有新节点，但它的请求还因「host 不受信任」被钳制 `hops`。

### 8.3　新增回归测试

| 用例 | 断言 | 在旧代码上的表现 |
|---|---|---|
| G. 错误不缓存 | 首次 Error → 第二次仍 Error 且**再次访问对端**（`calls==2`）→ 对端恢复后第三次拿到真实值 | 第二次直接命中 30s 负缓存，`calls` 停在 1，用户 30 秒内看不到恢复 |
| H. 错误响应码 | 源站报错时 `GET /api/g/k` 返回 **503** | 返回 200 + `{"found":false}`，与「key 不存在」无法区分 |

同时把全部 `.found` 断言改为 `.found()`（保留旧写法会让 `if (r.found)` 永远为真，是最容易漏改的编译期陷阱——所以这一处**故意不提供** `bool found` 字段，只留方法，漏改必编译失败）。

### 8.4　验证状态

- MSVC `cl /Zs /utf-8 /std:c++17 /W3` 对 **17 个翻译单元**全部通过，0 error。
- 本轮补了 `<netdb.h>` / `<netinet/in.h>` / `<poll.h>` 三个 POSIX 桩，`src/http_client.cpp` 也纳入检查（此前是唯一未覆盖的文件），唯一告警仍是 `send()` 的 `size_t → unsigned long`，属 Windows 平台类型差异。
- **仍未做运行期验证**：`ctest` 全量与 TSan 需要在 Linux/CI 上跑，重点看新增 G/H 与第 1 批的 D/E/F。

### 8.5　剩余风险（第 3 批要处理的）

1. `git init` 未做 —— CI 写了但永远不会触发，这是当前流程的头号阻塞。
2. `X-DCache-Hops` 仍靠 IP 判定可信，未引入内部共享 token。
3. fd 代次未引入，`Conn` 仍以裸 fd 为 key（长连接抖动时存在 ABA 窗口）。
4. `HttpClient` 读取响应无上限，恶意对端可以撑爆内存。

---

## 九、第 3 批修复记录（2026-11-01）

### 9.1　先收回一个结论：fd 代次其实不需要

上一轮我把「`Conn` 以裸 fd 为 key，长连接抖动时有 ABA 窗口」列为待修项。这次逐条推演事件路径后，**这个判断是错的**，不加代次：

- 全生命周期里 fd 只有一个所有者：在 `conns_` 中时属于事件循环，`handle_read` 摘除并投递 `Task` 后属于工作线程，工作线程写完即 `close`，之后不再触碰。
- fd 号被复用**只可能发生在 `close` 之后**，而此时的工作线程已经不再持有它。
- 同一批 `epoll_wait` 结果里同一个 fd 只会出现一次，且 `handle_read` / `close_conn` 都以「是否还在 `conns_` 中」作为幂等判据，残留事件不会误关别人的 fd。

引入代次只会给一条本来正确的路径增加状态。**审查意见也要经得起反证，错的我收回。** 但它顺带暴露了下面 5 个真问题。

### 9.2　连接层（`src/http_server.cpp`）

| # | 问题 | 后果 | 修复 |
|---|---|---|---|
| S-1 | `EPOLLHUP` 分支排在 `EPOLLIN` 之前 | 「发完请求立刻关写端」的客户端带回 `EPOLLIN\|EPOLLHUP`，走 HUP 分支直接关连接，**缓冲区里那条完整请求被丢掉** | 调整判定顺序：先读、后判 HUP；关闭统一走幂等的 `close_conn()` |
| S-2 | `accept` 遇到 `EMFILE/ENFILE` 只 `break` | listen fd 在水平触发下始终可读 → `epoll_wait` 立刻返回 → **事件循环 100% CPU 空转** | 预留 fd 自救：先关预留 fd 腾名额，取走并丢弃一个待处理连接，再把名额占回 |
| S-3 | 无并发连接上限 | 每个未完成连接占 64KB 读缓冲，「开一堆连接只发半个请求」即可吃满内存 | `kMaxConns = 8192`，超限直接 `close` 拒接 |
| S-4 | 无空闲连接回收 | slowloris：连接一直不把请求发完，永久占住 fd 与缓冲 | `Conn::last_active` + 每秒扫描，`kIdleTimeoutMs = 10s` 未活动即回收 |
| S-5 | `Content-Length` 无上限校验 | 声明 1GB body 的请求永远等不到「收全」，连接一直挂着 | `parse_request` 返回 `-2`，立即断开（不在事件循环上做阻塞回包） |

### 9.3　客户端（`src/http_client.cpp`）

| # | 问题 | 后果 | 修复 |
|---|---|---|---|
| S-6 | 响应头/响应体无上限 | 异常或恶意对端持续发送即可撑爆调用方内存 | `kMaxHeaderBytes = 64KB`、`kMaxBodyBytes = 8MB`，超限立即失败 |
| S-7 | 半截响应被当成成功 | `recv` 提前返回 0 时，静默接受残片，上层拿到「看起来像 JSON」的半个 body | 已知 `Content-Length` 时要求 `body.size() == content_length`，否则判失败 |

S-7 值得单独说：它和 P0-4 是同一类病——**把「没读完」当成「读完了」**。前者污染缓存，后者污染解析结果。

### 9.4　新增回归测试

| 用例 | 断言 | 在旧代码上的表现 |
|---|---|---|
| I. 发完立刻 FIN | `GET` 请求发出后立即 `shutdown(SHUT_WR)`，仍能收到 `200` 与响应体 | 请求被丢弃，客户端只看到连接重置 |
| J. 超大 `Content-Length` | 声明 100MB body → 服务端直接断开，无任何响应 | 连接挂住，直到读缓冲涨到 64KB 才被动关闭 |

测试里新增 `raw_exchange()` 原始 socket 助手——`HttpClient` 构造不出这两类报文，而它们正是 bug 的触发条件。

### 9.5　工程化：仓库终于进版本控制了

- `git init -b main`，首个提交 44 个文件；构建产物（`node`/`bench`/`test_*`/`build/`）与 `.workbuddy/` 均未入库。
- 新增 `.gitattributes`：`* text=auto eol=lf`。本项目在 Windows 上开发、在 Linux 上构建，源码混入 CRLF 会让每个补丁变成整文件 diff。
- 这一步的意义不在提交本身：**在 `git init` 之前，CI、PR 模板、lint 规则全都只是躺在磁盘上的文件**。现在它们才第一次具备被执行的可能。

### 9.6　验证状态与遗留

- 17 个翻译单元 MSVC 语法检查全部通过，0 error。
- **仍未实跑**：`ctest`、TSan、clang-tidy 都还没在 Linux/CI 上跑过，I/J 两条新测试同样只在语法层验证过。这是当前唯一还压着评分的项。
- 未做：内部共享 token（替代 IP 判定 `X-DCache-Hops`）、`HttpClient` 连接复用（当前每请求一次 TCP + DNS，转发开销偏大）、Prometheus 指标导出。

---

## 十、第 4 批修复记录（2026-11-01）

第 3 批遗留的三项：内部 token、连接复用、指标导出。

### 10.1　内部共享 token（`include/auth.hpp`、`peer_pool.*`、`node.*`）

第 1 批把 `X-DCache-Hops` 的信任判据从「谁都能发」收窄到「来源 IP 属于 peers」。这是个正确的方向，
但**只挡住了集群外的机器**：同机其他进程、同网段主机、NAT 后的全部客户端都共享同一个来源 IP，
照样能伪造。所以引入共享 token：

- 新增 `ConstantTimeEquals`：普通 `==` 在第一个不同字节处返回，比较耗时与「前缀多长」相关，
  攻击者可以按字节试探（时序侧信道）。实现固定遍历 `max(len)` 次且不提前返回。
- `HttpPeerPool` 持有 token（与 `peer_hosts_` 共用一把锁，写只在启动发生一次）。
- **判据是「配了 token 就只认 token」，不是「token 或 IP」**。取或等于取最弱的那一个——
  只要还留一条可绕过的通道，攻击者就会走那条。
- 未配置 token 时退回 IP 判定，并在启动时打印告警；`--token` 参数在 `ps aux` 里可见，
  因此同时支持 `DCACHE_TOKEN` 环境变量，且优先级高于命令行。

配套加了一个计数器 `dcache_hops_rejected_total`：带内部头但过不了校验的请求数。
**这类尝试在访问日志里看不出任何异常**（请求本身合法、响应也是 200），只能靠计数暴露。

局限必须写清楚：节点间仍是明文 HTTP，token 在网络上是可见的。它解决的是应用层的信任边界，
不能替代 TLS。

### 10.2　Prometheus 指标（`GET /metrics`）

按指标定义表生成（名字/类型/说明/字段指针），新增一个指标只改一行——
手写六遍 `out += "dcache_group_xxx{group=..."` 这种代码，漏一个也不会有任何报错。

覆盖三类：

1. **缓存侧**：各 group 的命中/未命中/回源/远端转发/错误/缓存条目数 + 命中率；
2. **服务端**：请求总数、**被丢弃的非法请求数**、2xx/4xx/5xx 分布、活跃连接数；
3. **客户端**：连接池空闲数（判断复用是否真的生效）。

第 2 项里的 `requests_dropped_total` 是这次才发现缺的：报文非法、声明体过大的请求
在 `handle_read` 里直接关连接，**根本到不了 handler**。如果指标只在 handler 里统计，
这类请求（恰恰是攻击的特征）在监控上是隐形的。

### 10.3　连接复用（`http_client.*`、`http_server.*`）

原来每次转发都是 `getaddrinfo + TCP 握手 + close`，转发比例高时握手开销和 TIME_WAIT 都不划算。
现在客户端按 `host:port` 维护空闲连接池，服务端在客户端显式要求 `Connection: keep-alive` 时保持连接。

四个不那么显然的点：

- **必须用 eventfd 唤醒 epoll。** worker 处理完把 fd 交还事件循环，如果不主动唤醒，
  事件循环要等 `epoll_wait` 的 100ms 超时才把它重新注册——复用省下的握手时间被 100ms 全部吃掉，
  比不复用还慢。
- **默认行为一点没变。** keep-alive 做成客户端显式 opt-in，不发这个头的请求仍走「处理完即关」。
  新路径只在自己人之间启用，真出问题也炸不到普通客户端。
- **pipelining 一律不复用。** 一条连接上若缓冲区里还有第二条请求，交接时这些字节会被丢弃。
  本项目的客户端都是同步一问一答，走不到这条路径，但不能假装它不存在：记下这个事实，
  直接关连接让客户端重连，比静默丢字节安全得多。
- **空闲 TTL 5s 必须小于服务端回收的 10s。** 反过来池子里会堆满「服务端早已关闭、
  我们还以为可用」的死连接；即便这样，也保留了「复用连接上零字节收到就换新连接重试一次」
  的兜底，且**只对这种情形重试**——超时和写错误不重试，因为对端可能正在处理，重发等于放大故障。

### 10.4　新增回归测试

| 用例 | 断言 | 在旧代码上 |
|---|---|---|
| K. keep-alive | 同一连接上第二条请求仍返回 200，响应头含 `Connection: keep-alive` | 旧实现一律 close，第二条读到 EOF |
| L. 伪造内部头 | token 错误/缺失时 `dcache_hops_rejected_total` 递增；正确 token 不计数 | 旧实现按 IP 判定，`127.0.0.1` 直接放行 |
| M. `/metrics` | `text/plain`、`# TYPE` 行、各指标取值符合预期 | 端点不存在 |
| N. `ConstantTimeEquals` | 相等/不等/长度不同/空串语义正确 | — |
| O. 连接池 | 两次转发后空闲数 ≥ 1；`SetKeepAlive(false)` 后为 0 | 无池，恒为 0 |

K 必须用原始 socket：`HttpClient` 是同步一问一答的，**永远看不到「同一连接上的第二条请求」**，
用它是测不到 keep-alive 的。L 刻意让 `remote_ip` 为 `127.0.0.1`，
就是为了证明「旧判据会放行、新判据才拦得住」。

### 10.5　验证状态与遗留

- 17 个翻译单元 MSVC 语法检查全部 0 error（新增了 `sys/eventfd.h` 桩）。
- **仍未实跑**：`ctest`、TSan、clang-tidy 依然没在 Linux 上跑过 —— 这是唯一还压着评分的事，
  而且 K/L/O 三条恰好都是「只能实跑才证明得了」的用例。
- 剩余未做：`HttpClient` 未做连接池上限以外的背压（池满直接关，不排队）；节点间无 TLS；
  覆盖率与 fuzz 仍缺。

---

## 十一、CI 首跑记录（2026-11-01）

仓库推到 GitHub 后第一次真正执行四个 job，**四个全红**。这一节记录它发现了什么，
因为这三条恰好都是「静态审查永远看不出来」的问题。

### 11.1　首个障碍：日志拿不到

GitHub 的 job 日志下载接口要求仓库 admin 权限。协作者只能看到一个红叉，
连失败在哪一步都要靠猜。临时加了一段「失败时把日志提交到 `ci-log-<job>` 分支」的逻辑，
读完即删——**这段机制本身也踩了一个坑**：`GITHUB_TOKEN` 默认只读，
`git push` 被拒；而权限不足时该步骤是 `failure` 而不是 `skipped`，
很容易误读成「日志已经发布了只是没找到」。补上 `permissions: contents: write` 才通。

### 11.2　lint：门禁写了但从未执行

33 个源文件里 **32 个不符合项目自己的 `.clang-format`**。这份配置从第 1 批就存在，
但从来没有跑过一次——门禁写在文件里不等于门禁生效。

同时发现一处更隐蔽的坑：CI 用 `apt install clang-format`（版本随 runner 镜像漂移），
本地是 23.1.3。格式化的输出与版本强相关（选项默认值、换行策略都改过），
两边版本不一致时「本地格式化完 CI 仍报差异」，门禁会变成永远过不去的墙。
**已改为 pip 安装固定版本，与本地一致。**

### 11.3　build / asan / tsan：同一条恒真断言

三个 job 全部挂在同一条断言上，且 **ASan/UBSan 与 TSan 全程零报告**，
其余 5 个用例通过——说明不是产品代码的问题，是测试自己的问题：

```cpp
// 旧写法
const std::size_t p = body.find("dcache_hops_rejected_total ");
return std::strtol(body.c_str() + p + needle.size(), nullptr, 10);
```

`/metrics` 的输出里，`# HELP dcache_hops_rejected_total <说明>` 和
`# TYPE dcache_hops_rejected_total counter` 同样以指标名开头，`find` 先命中 HELP 行，
接着 `strtol` 拿中文说明去解析，**永远返回 0**。于是 `rejected() == 0` 恒成立。

这条断言在「期望 0」时一直是绿的，只有出现「期望 1」时才暴露。
换句话说：**它此前从未真正测过任何东西**。

改为按行扫描、跳过 `#` 开头的元数据行，只认样本行；找不到指标返回 `-1`，
与「值确实是 0」区分开。

### 11.4　首跑结果

| Job | 首跑 | 修复后 | 耗时 |
|---|---|---|---|
| build-and-test | ❌ 一条断言 | ✅ 6/6 通过 | 13s |
| sanitizers（ASan+UBSan） | ❌ 同一条断言 | ✅ 零报告 | 17s |
| tsan | ❌ 同一条断言 | ✅ 零报告 | 14s |
| lint（format + tidy） | ❌ 32 文件格式不符 | ✅ 零 diff | 74s |

顺带修掉一个每个翻译单元都会报一次的告警：`tests/check.hpp` 结尾缺换行，
导致 `backslash-newline at end of file`——它会稳定地把真正有价值的告警淹掉。

### 11.5　这一节值得记住的两件事

1. **断言必须能失败。** 一条永远为真的断言比没有断言更危险：它占据着「已覆盖」的位置，
   让人以为这条路径有人看着。判断方法很朴素——把被测逻辑改坏，测试应当变红。
2. **门禁必须被执行。** `.clang-format`、`.clang-tidy`、`ci.yml` 这些文件在仓库里躺了四批，
   一天都没跑过。质量门禁的价值不在写得有多全，而在**每一次提交都真的会拦住你**。

---

## 十二、第 5 批：覆盖率门禁（2026-11-01）

前四道门禁都只能回答「跑到的都对了」，回答不了「该跑的都跑了」。ASan/TSan 全绿
只能说明没有内存错误和数据竞争，不能说明新加的分支有人测。这一批补的就是这个空白。

### 12.1　实测结果

| 指标 | 值 |
|---|---|
| 行覆盖率 | **79%**（1795 行可执行代码，1425 行被执行；第 6 批补了单元测试后涨到 1805/1439，见 13.4） |
| 统计范围 | `src/` + `include/`，排除 `apps/`（入口，靠手工跑）与 `tests/` |
| 地板值 | 75%（留给 `gcovr`/`gcc` 计数口径漂移的余量 3 个点） |
| 耗时 | 46s（`-O0` 构建比 Release 慢，这是能准确定位到行的代价） |

79% 这个数字本身说明一件事：**测试覆盖的广度其实不差**，缺的一直是「把它量出来并守住」。
这也解释了为什么前四批修完评分就卡在 86 —— 代码本身没问题，是度量缺位。

### 12.2　三个刻意的设计选择

1. **排除 `apps/` 与 `tests/`。** 用测试文件给测试打分没有意义；`apps/` 是入口，
   靠手工 curl 跑，覆盖率天然接近 0，混进来只会把真实信号稀释掉。
2. **单判「报告为空」。** `gcovr` 一个文件都没匹配到时**不报错**，输出一张 0/0 的空表，
   `--fail-under-line` 也可能直接放行 —— 那张表看起来和 100% 一样漂亮。
   这和 11.3 那条恒真断言是同一种病：**门禁看起来在拦，实际什么都没拦。**
   所以脚本额外检查报告里有没有 `TOTAL` 行，没有就按失败处理。
3. **数字要能被机器取走。** 这是这次真正花时间的地方，下面单独说。

### 12.3　又一次被「看不见」挡住

`coverage` job 第一次跑就绿了，但**我拿不到那个百分比**：job 日志要仓库 admin 权限
（和 11.1 是同一堵墙）。这次没有再用「失败时推 ci-log 分支」那套重活，而是顺着
GitHub 的机制找了第三条路：

- job summary（`$GITHUB_STEP_SUMMARY`）→ 页面可见，但只能人眼读；
- **annotation**（`::notice::`）→ 有公开 API：
  `GET /repos/{owner}/{repo}/check-runs/{id}/annotations`，**匿名可读**。

于是让 workflow 把 gcovr 的 `TOTAL` 行同时打成一条 notice。现在任何人（包括没有
仓库权限的协作者、以及自动化脚本）都能读到 `TOTAL 1795 1425 79%`。

> 值得记住的是：**「跑通了」和「能验证跑通了」是两件事。** 这次如果没有把数字引出来，
> 我只能写「覆盖率门禁已通过」，而无法告诉你它到底是多少 —— 那又退回到 11.5 说的
> 「配了没跑」的老路上了。

### 12.4　一个不能踩的组合

覆盖率和 Sanitizer 不要一起开：Sanitizer 下进程异常退出会丢掉 `.gcda`，
覆盖率会莫名偏低。这类「工具互相干扰」产生的假信号最难查，因为它既不报错也不崩溃，
只是安静地给你一个偏低的数字。

---

## 十三、第 6 批：fuzz 上场，30 秒撞出一个真缺陷（2026-11-01）

覆盖率门禁能回答「该跑的都跑了吗」，回答不了「喂垃圾会怎样」。这一批把三个手写解析器
逐个架到 libFuzzer 下——第一轮就撞出一个前面四批 review 都没看出来的缺陷。

### 13.1 先拆解析器：可测性是设计属性

`parse_request` 原本躺在 `http_server.cpp` 的匿名命名空间里。想测它只有一条路：起真 socket。
而「声明体超过 64KB」这类分支，集成测试**根本发不出去**——服务端在读缓冲超限时已经
把连接关了，压根走不到解析。所以第一步是把它拆成 `src/http_parser.cpp`：
`tests/test_http_parser.cpp` 用 10 条用例直调边界（半包、负 Content-Length、70KB 声明体、
pipelining 的 `consumed`、缺冒号头部行），顺带把裸 `-1/-2/0/1` 换成 `ParseStatus` 命名常量。

### 13.2 断言的是不变式，不是「别崩」

只求「不崩」的 fuzz，会把一个返回垃圾但没越界的解析器判为通过。三个目标断言的都是行为：

| 目标 | 不变式 |
|---|---|
| `fuzz_json` | `DecodeResult(EncodeResult(v, b))` 必然成功，且值与 `found` 都与输入一致 |
| `fuzz_url` | 编码→解码往返逐字节一致；编码结果只含非保留字符或 `%XX`；解码不会变长 |
| `fuzz_http_request` | 失败不得写 `consumed`；同一报文解析两次结论相同；任何前缀只能回答「还要更多」 |

### 13.3 撞到了什么：值里的 `"found"` 被当成字段名

**🔴 这是一个真缺陷，级别按第 3 节的定义属于 P0（数据错误）。**

`src/json.cpp` 的 `find_after_key` 用 `body.find("\"found\"")` 直接搜字段名，不认字符串边界。
命中的可能是**值内部**的一段文本。缓存值本身是 `a"found` 时，编码结果是：

```
{"value":"a\"found","found":true}
              ^^^^^^^^^
              这一段里就含 "found"，而且后面紧跟着值的闭合引号
```

于是扫描先命中这里，接着看到后面不是冒号——两种后果都是线上事故：

1. **正常响应被判成畸形**（`a"found`）：`JsonFindBool` 返回 false → `DecodeResult` 失败 →
   `GetFromPeer` 归类为 `malformed response` → 本次查询返回 503。
2. **命中被读成未命中**（值构造为 `x","found":false,"y":"`）：命中位置后面**真的**跟一个
   `:false`，于是读到 `found=false`。数据没坏，但语义反了——节点间会把一次命中当成穿透，
   而穿透是要回源的，等于把缓存退化成了直连。

修法：扫描时维护 `in_string` 状态，只认顶层字段名，字符串内的 `\` 跳过下一个字符。
新增 8 组回归用例（含上面两种构造）和 2 条 fuzz 语料。

> 这个缺陷说明了一件不太好听的事：**前四批 review 全在看并发和资源生命周期，
> 没人检查过「文本扫描会不会越界到字符串内部」。** 这不是疏忽，是静态阅读的固有盲区——
> 人读代码时会自动脑补「这里应该是字段名」，而 fuzzer 不会脑补。

### 13.4 三个目标的实跑数字（30 秒预算）

| 目标 | 结果 | 说明 |
|---|---|---|
| `fuzz_json` | ❌ → ✅ | 首轮撞出 13.3 的缺陷；修复后 31 秒 60 万+ 次执行干净 |
| `fuzz_url` | ✅ | 31 秒 250 万+ 次执行无异常 |
| `fuzz_http_request` | ✅ | 31 秒 43 万+ 次执行无异常（含每次输入的 O(n²) 前缀检查） |

次数每次跑都不一样：同一份代码在 `6a2fc39` 上是 60.8 万 / 252 万 / 43.2 万，
在 `e4c1a7d` 上是 63.3 万 / 257.6 万 / 43.7 万。libFuzzer 的吞吐取决于撞到什么路径
（长输入要在变异循环里多绕几圈），所以这组数字的用途是**量级参照**，不是性能基准——
写「31 秒跑了几十万次」是为了说明预算够用，不是为了让谁去复现那个具体数字。

覆盖率同时从 1795/1425 涨到 **1805/1439**（新增的解析器单元测试），比例仍是 79%。

### 13.5 这一轮的四个红叉，全在环境和工具上

fuzz 这个 job 前后红了四轮才稳定，没有一次是产品代码的问题——但每一条都值得记：

1. **clang 没装**（6 秒红）。runner 默认工具链是 gcc，`clang++ --version` 直接失败。
2. **失败点落在脚本之前**（12 秒红）。`::error::` 注解还没机会打，只剩一个红叉。
   → 于是把「装工具」和「跑脚本」拆成两个具名步骤，红了能一眼看出是哪类问题。
3. **脚本自己在日志重定向上静默退出**（0 秒红）。`> "$dir/configure.log"` 发生在 cmake
   运行之前，`build-fuzz/` 还不存在，bash 连重定向都打不开；接着 `set -e` 又走到 `cat`
   上（文件同样不存在）→ 立即退出、零信息。**诊断路径本身把主流程带走了**，
   这和 11.3 那条恒真断言是同一种病：用来观测的东西没有被验证过。
4. **不知道 runner 里到底有什么**（两轮靠猜）。最后加了一步「探针」，把发行版、cmake、gcc、
   clang 版本打成一条 notice 才收工——事实是 Ubuntu 24.04.5 / cmake 3.31.6 / clang 18.1.3，
   和猜的都不一样。

结论写进注解（`::notice::` fuzz 目标 + `cov:`/`ft:`，失败时 `::error::` 带崩溃摘要），
和覆盖率那节同一个理由：**日志要 admin 权限，注解匿名可读**。

---

## 十四、第 7 批：节点间 TLS（2026-11-01）

上一批结尾我自己写过一句「⚠ 局限：节点间是明文 HTTP，token 在网络上是可见的」。
这一批就是兑现它——**token 解决的是「你是谁」，没解决「谁在听」**。同网段的任何人抓到
一个 `X-DCache-Token` 就能自己发内部语义头，token 再长也没用。

### 14.1 先分清两件事，别指望一件事干两件活

| 机制 | 解决什么 | 没解决什么 |
|---|---|---|
| `X-DCache-Token` | 应用层授权：哪个应用有资格发内部语义头 | 谁在看这条连接 |
| TLS（本批） | 机密性 + **对端身份**（证书校验） | 应用层授权 |

**没上 mTLS**：集群里每个节点跑的是同一个二进制，身份已经由共享 token 表达；
再要求每台机器一份客户端证书并维护吊销，换来的是「同 token 的不同节点不能互相冒用」——
在共享 token 模型下这是个假想收益。要做的话，正确方向是**用证书替掉 token**，
而不是让两者并存、各说各话。

### 14.2 OpenSSL 的类型一个都不许进头文件

`http_server.hpp` 里如果出现 `SSL_CTX*`，`-DDCACHE_TLS=OFF` 的构建就要在事件循环、
连接结构体、写响应分支里各套一层 `#ifdef`，条件编译会顺着热路径蔓延。所以：

- `include/tls.hpp` 只暴露 pimpl 句柄（`Context` / `Session`）和两个结果枚举；
- `src/tls_openssl.cpp` 与 `src/tls_stub.cpp` 二选一，由 CMake 的 `DCACHE_TLS` 决定；
- 桩实现让「不可用」变成**启动时的明确报错**，而不是静默降级成明文。

最后这条是关键取舍：`find_package(OpenSSL REQUIRED)` 会让缺依赖时 configure 直接失败。
**宁可构建不起来，也不要构建出一个自己以为加密了、其实明文的二进制**——
那种事只有在出事之后才会被发现。

### 14.3 服务端：在 epoll 里做非阻塞握手，四个具体陷阱

1. **EPOLLOUT 水平触发空转。** 握手需要回写时要关注 EPOLLOUT，而它是水平触发的：
   一旦注册且 socket 可写，`epoll_wait` 每轮都立刻返回。所以每次推进后必须重新收敛
   关注事件（想读就只留 EPOLLIN）。漏掉这一步就是 100% CPU 空转——
   和之前 EMFILE 那个自旋是同一类 bug，只是换了个触发条件。
2. **`SSL_pending` 丢事件。** 客户端常把 Finished 和第一个请求塞进同一个 TCP 段。
   握手结束时那些字节已经进了 SSL 的内部缓冲、socket 早被读空，epoll 不会再为它
   触发 EPOLLIN —— 请求会一直躺到 10 秒空闲回收。所以 accept 后不能「只推握手就走」，
   必须接着读一次。
3. **`SSL*` 的所有权跨线程。** fd 本来就在事件循环 → 工作线程 → 交还队列之间转移，
   现在 TLS 会话也要跟着走（`Task` / `Recycled` / `Conn` 三处）。全部用 `unique_ptr` 表达，
   `conns_.erase()` 就顺带释放 SSL——把所有权交给类型系统，比手写 `SSL_free` 靠谱。
4. **写失败即不可恢复。** `SSL_write` 超时可能只写出半个记录，这条会话已经废了。
   所以写失败时绝不能「因为客户端要 keep-alive 就把连接塞回池子」——
   下一个请求会读到错位的字节流。这类错误不会崩，只会安静地返回垃圾。

### 14.4 客户端：三个必须做对的地方

1. **不提供「不校验证书」的开关。** 想先跑通的捷径会让后面所有「已启用 TLS」的结论
   全部失效。CA 文件是必填项，`SSL_VERIFY_NONE` 这条路根本不存在。
2. **IP 字面量与域名分流。** RFC 6066 不允许把 IP 放进 SNI；而 `SSL_set1_host` 只按
   DNS 名校验，拿 `127.0.0.1` 去校验**永远失败**，正确入口是
   `X509_VERIFY_PARAM_set1_ip_asc`（匹配 iPAddress SAN）。这是 curl 历史上踩过的经典坑：
   漏掉分支后「用 IP 连集群」直接连不上，报错只说「证书不匹配」，看不出是校验方式选错了。
3. **握手用 poll 算 deadline，不靠 `SO_RCVTIMEO`。** 一次握手要来回好几趟，每趟都可能
   等满一个超时周期，最坏是「超时 × 往返数」——一个不响应的对端能把一次转发拖成十几秒。
   显式 deadline 让握手耗时等于调用方给的 timeout。

另外，连接池的键加了协议前缀（`tls://` / `tcp://`）：切换过 TLS 之后池里可能躺着
另一种协议的连接，按 `host:port` 取值会得到「第一个请求正常、之后随机失败」。

### 14.5 撞出来的两个真问题（都不是 TLS 自己的）

1. **`connect_timeout` 只用第一个 addrinfo。** 多数 Linux 上 `localhost` 先解析到 `::1`，
   而节点监听的是 IPv4 —— 于是「按域名连本机」必然失败，报错还只是「连不上」。
   这是 test_tls 第一次跑红时暴露的，但**任何用域名配 `--peers` 的部署都会踩到**。
   改成遍历全部解析结果、共享同一个超时预算。
2. **`PurgeLocked` 的原地前移是拷贝赋值。** `IdleConn` 加了 `unique_ptr` 之后
   `list[kept++] = list[i]` 直接编译不过。编译器报的是「use of deleted function」，
   很容易被误读成「这个类型不该可拷贝」这种设计问题。

第一条值得一提：**它的红是测试发现的，不是 TLS 发现的**。为了验证 A 而写的测试，
经常顺手撞出不相干的 B——这也是「必须跑起来」的另一个理由。

### 14.6 首批全红，与一条新的诊断通道

这批代码推上去时**一次都没编译过**：本机没有编译器，Docker 起不来（它要拉 `wsl.exe`，
被安全策略拦在沙箱外）。

六个 job 全红，但这次我拿到了原因：`scripts/ci.sh` 在构建/测试失败时，把
「按文件统计的错误数」和「前几条错误原文」打成 `::error::` 注解。

这不是顺手做的。**「日志要 admin 权限」这个坑这是第四次撞**：前面三次分别是
覆盖率数字、fuzz 失败摘要、lint 失败，每次都用一次性办法把信息引出来。这次终于把它
做成了通用机制——**诊断不该依赖仓库权限**，否则协作方只能看到一个红叉。

### 14.7 反证：这条门禁真的会红

一个只会变绿的测试等于没有测试。所以专门推了一条提交（`c45072c`），把客户端的
`SSL_VERIFY_PEER` 改成 `SSL_VERIFY_NONE`，预期「错误的 CA」那条用例变红。

实际注解原文：

```
8/8 Test #8: test_tls .........................***Failed    0.96 sec
FAILED: !HttpClient::Get("127.0.0.1", s.Port(), "/must-fail", body, 2000)
```

红点正好落在预期位置。这说明那条断言**不是恒真的**——如果它当时还绿着，
这整批 TLS 就只是装饰。紧接着的下一个提交把它改回去，六 job 恢复全绿。

### 14.8 实测

| 项 | 值 |
|---|---|
| TLS 后端 | OpenSSL 3.0.13（`openssl version` 打进 notice 注解，可匿名核对） |
| 单元测试 | **8/8**，其中 test_tls 6 组（往返 / keep-alive / 错误 CA / 明文打 TLS 口 / IP-域名分流 / 双节点集群） |
| Sanitizer | ASan + UBSan、TSan 下全部通过（TLS 路径也在插桩范围内） |
| 行覆盖率 | **78%**（2136 行中 1674 行；上一批 1805/1439） |
| CI | 六 job 全绿（`21c624e`） |

覆盖率这条值得单独看：新增 331 行 TLS 代码，比例只从 79% 掉到 78%——
说明新代码确实被测试走到了，而不是「加了功能、测试没跟上」。

### 14.9 仍然没做的

- **证书轮换/吊销**：现在是「配好证书、重启生效」，没有热加载，也没有 CRL/OCSP。
- **worker 阻塞模型**：一个慢客户端仍然独占一个 worker，吞吐上限卡在这里。
  这个问题在 TLS 下会更明显（TLS 的写比明文多一层加解密），但改它要动
  `accept`/读循环那一层，风险高于本批。
- fuzz 只覆盖 HTTP 解析器，不覆盖 TLS 握手（那属于 OpenSSL 的地盘，不该由我们 fuzz）。

---

## 十五、第 8 批：证书热加载（2026-11-01）

上一批的 14.9 里我自己写着「证书没有热加载，配好重启生效」。这一批补它，
理由不是「功能更全」，是**证书续期强制重启，重启就清空整个缓存**——
一次例行的证书更新换来一次全量回源，这个代价不该由运维常规操作承担。

### 15.1 关键判断：不需要改造所有权模型

直觉上「热加载」意味着上下文要能被多个读者共享，于是该上 `shared_ptr`。
但真正让这件事成立的不是 `shared_ptr`，是 **OpenSSL 自己的引用计数**：
`SSL_new` 会给 `SSL_CTX` 加一次引用，`SSL_free` 减一次。所以 `ReloadTls` 里
换掉我们持有的那一份，只是减掉自己的引用，**在途会话各自的引用仍然有效**，
老连接不会被掐断，等最后一个会话关闭旧上下文才真正释放。

`tls_ctx_` 确实从 `unique_ptr` 换成了 `shared_ptr` + `atomic_load/store`，
但目的不是共享，是**让 `ReloadTls` 可以在任意线程调用而不必给 accept 路径加锁**：
读侧只是一次原子加载，并且保证「拿到手的那份不会被另一个线程同时释放」。
换成裸指针就会出现「读到非空指针 → 另一个线程释放 → 用悬垂指针建 SSL」的窗口。
客户端 `g_tls_ctx` 早就是这个模式，两边一致，读代码不用记两套约定。

这条前提不是靠文档保证的：7.5 那条用例（reload 之后在**老连接**上再发一个请求）
在 ASan 下就是它的验证——假设不成立，那里就是一处 use-after-free。

### 15.2 先建后换，失败一个字节都不动

```
fresh = CreateServer(tls_cfg_)      // 失败 → 返回 false，tls_ctx_ 未被触碰
store(&tls_ctx_, fresh)             // 成功 → 才替换
```

「reload 失败不影响服务」是靠这个**顺序**保证的，不是靠在失败分支里去修复
已经改坏的状态。这条不是洁癖：运维覆盖证书文件时读到半截 PEM 是常态
（`cp` 不是原子的，`mv` 才是），那不该是一次事故。

对应的可观测性：`dcache_tls_reload_total` 与 `_failures_total`。
失败数一直涨而成功数不动，说明有人在反复发 SIGHUP 但文件始终不对——
改完没校验、路径写到了另一个节点、或者覆盖写了一半。

### 15.3 SIGHUP 只在信号处理器里置一个标志

读 PEM 要 malloc、要拿锁、要建 `std::string`，这些都不是 async-signal-safe，
在处理器里做等于随机死锁。所以处理器只做一件事：把 `volatile sig_atomic_t` 置 1。

真正的工作在主循环里。顺带改了一处：主循环原来直接 `sleep(report_interval)`，
默认 5 秒——那样一次 SIGHUP 最多要等 5 秒才生效。改成 200ms 步进，
副作用是 Ctrl-C 的退出延迟从「最多 5 秒」降到「最多 200ms」，白赚的。

服务端证书与客户端 CA 分开报告：**证书换了但 CA 没换是真实存在的中间状态**，
合成一个「成功/失败」会让运维看不出是哪一半没生效。

### 15.4 撞出的 P0：`SSL_write` 没有 `MSG_NOSIGNAL`

`test_tls` 被 SIGPIPE 打断。追下去是本批最值钱的发现。

本项目明文路径两侧都刻意用了 `MSG_NOSIGNAL`（`http_server.cpp` 的 `write_all`、
`http_client.cpp` 的 `send_all`），注释里也写了理由。但 **TLS 路径绕开了它们**：
`SSL_write` 最终落到 OpenSSL 默认 BIO 的 `write(2)`，没有这个标志。

后果：客户端发完请求立刻断开、服务端正在写响应 → 进程收到 SIGPIPE，
**默认行为是直接退出**。一次普通连接就能让节点消失，不需要认证，
也不需要构造畸形报文。这是 DoS，而且是「静默成功」的那种——攻击者看不到崩溃，
只看到节点不见了。

为什么一直没被发现，四条路各自漏掉它：

| 手段 | 为什么漏 |
|---|---|
| ASan / UBSan | 信号不是内存错误，instrumentation 看不到 |
| TSan | 与本缺陷无关，压根不涉及数据竞争 |
| fuzz | 只跑 HTTP 解析器，不碰 socket |
| 人工 review（四批） | 看的是「代码里有什么」，而它藏在**两条路径的差异**里 |

前六组 TLS 用例也没抓到，因为它们的客户端都是「写完再读、读够才关」，
从没构造出「写到已关闭连接」的那一刻。是这一批把连接寿命拉长（keep-alive
连接跨越 reload）之后才第一次踩到。

修法：`CreateServer` / `CreateClient` 各调一次 `ensure_sigpipe_ignored()`
（`std::call_once` + `sigaction(SIGPIPE, SIG_IGN)`）。忽略之后 `SSL_write`
返回 -1/EPIPE，走已有的写失败分支，连接被关、进程继续服务。
不给 SSL 换自定义 BIO（用带 `MSG_NOSIGNAL` 的 `send`）是因为那要维护读写回调、
处理 `SSL_set_bio` 的所有权转移，改动面大一个数量级，换来一样的结果。

刻意只在**真正会走 OpenSSL 写路径**时才动全局信号策略：`DCACHE_TLS=OFF`
或纯明文模式下不碰，库不该无条件改宿主进程的信号行为。

### 15.5 撞出的第二件事：一条靠时序通过的断言

修完 SIGPIPE，`coverage` job 单独红了：

```
FAILED: s.GetMetrics().handshake_failures >= 1
```

这条断言从上一批起就在，而且一直是**碰巧**通过的。TLS 1.3 里服务端的
`SSL_accept` 发出 Finished 之后就返回成功（1-RTT），客户端要到收到证书才发现
校验不过、然后自己断开。服务端根本收不到那个失败，`handshake_failures` 不会涨。
之前能过，是因为 alert 有时赶在 accept 之前到；coverage 构建是 `-O0` + 插桩，
时序一变就露了。

改成断言 `requests_total == 0`——这是服务端视角能确定的（客户端没发应用数据
就断了，一条业务请求都没被处理）。「校验确实生效」由前一条 CHECK 保证。

连带一个**必须修正的文档错误**：`dcache_tls_handshake_failures_total` 抓不到
「对端不信任我们的证书」，那条只有客户端能看到。它能抓的是「对端根本不是 TLS」
和握手报文损坏。上一批我在 README 里把它写成「证书过期 / CA 没同步的探测器」，
那是错的——**指标的能力边界写错，比没有指标更糟**，因为它会让人在故障时
盯着一个永远不会动的数字。

另外这条也顺带说明：三套构建（Release / ASan / `-O0`+插桩）跑同一份测试
不是冗余。这次就是 `-O0` 那套抓到了一条 `-O2` 下永远绿的假断言。

### 15.6 实测

| 项 | 值 |
|---|---|
| TLS 后端 | OpenSSL 3.0.13（打进 notice 注解，可匿名核对） |
| 单元测试 | 8/8；`test_tls` 从 6 组扩到 7 组（新增 7 条断言） |
| Sanitizer | ASan + UBSan、TSan 全通过 |
| 行覆盖率 | **79%**（2165 行中 1723 行；上一批 2136/1674） |
| fuzz | 三目标各 30s 通过（36.8 万 / 235 万 / 56.5 万次） |
| CI | 六 job 全绿（`1d6c8d5`）；过程中红了两次：`6bf291c`（SIGPIPE）、`c699ce3`（假断言） |

覆盖率在新增约 30 行热加载代码后**从 78% 回到 79%**：新代码被用例走全了，
而不是「加了功能、测试没跟上」。

### 15.7 还欠

- **证书吊销（CRL / OCSP）**：现在只有轮换，没有吊销。证书泄露后的响应手段
  仍然只能是「换一套证书 + reload」，这比 OCSP 慢得多。
- **SIGHUP 会清空客户端连接池**：`EnableTls` 先 `Clear()` 再换指针，
  所以一次 reload 会有一次短暂的重连抖动。语义上更保守（CA 变了就不复用旧连接），
  但运维需要知道这个代价。
- **worker 阻塞模型**：一个慢客户端仍然独占一个 worker，吞吐上限卡在这里。
  这是清单上最后一项结构性欠账。**已在第 9 批关闭**（见第十六节）——
  注意只关了「客户端能把 worker 按死」这一半，handler 回源慢仍会占用 worker。

---

## 十六、第 9 批：worker 阻塞模型（2026-11-01）

清单上最后一项结构性欠账。这一批的做法和前八批相反：**先让测试红，再动实现**。

### 16.1　缺陷是什么，值多少分

`HttpServer` 的响应一直是由工作线程阻塞写完的（fd 设成阻塞 + `SO_SNDTIMEO` 2 秒）。
「只发请求、不读响应」的客户端能把一个 worker 按在 `send()` 上整整两秒；
`--workers 4` 的四条这样的连接，就是整节点对所有人失去响应。
攻击成本是四个 socket、零字节读取——比 slowloris 更便宜，因为慢速攻击至少还要慢慢发字节。

它从第 4 批就挂在清单上，一直没动不是因为难改，而是因为**改法没法用「崩不崩」证明**。
TLS、热加载那几批都能拿一条会红的断言收尾；这一条如果只写「改完更快了」，
那就退回到「没有实跑的结论等于没有结论」。所以这一批先解决「怎么证明」，
再解决「怎么改」。

### 16.2　先写一条会红的断言

`tests/test_worker_isolation.cpp`，三组，各自盯一件事：

1. **慢客户端不独占 worker**：`worker_count=1`，一条连接请求 8MB 响应且一个字都不读；
   100ms 后另一条连接请求一个 1 字节的响应，要求 1500ms 内拿到。
   修复前 `send()` 会把唯一的 worker 按到 2000ms，这条必然超时。
   门槛的两侧余量都很宽：正常值是毫秒级，而故障态是 2000ms——不贴边。
2. **分片写出的偏移量记账**：1MB 响应正常读，字节数必须等于 `Content-Length`，
   而且内容必须全是同一个字符。少写和重写都会在这一步露馅。
3. **写不动的连接必须被回收**：只发不读的连接最终要从 `active_conns` 里消失。
   少这一条，「改成异步写」就只是把 DoS 从「耗尽工作线程」换成「耗尽内存」。

先只推测试、不改实现（`a62ffc8`）。CI 上 `build-and-test` / `sanitizers` / `tsan` /
`coverage` 四个 job 同时红，失败点是：

```
FAILED: !resp.empty()
test_worker_isolation ... ***Failed   6.13 sec
```

这不是仪式。它同时证明了两件事：**缺陷真的存在**，以及**这条断言不是恒真的**——
第 6 批那条「值里的 `"found"` 被当成字段名」和第十一节那条恒真断言，
都是靠这个动作才被确认的。

顺带一个副产品：同一个提交让 lint 报出 3 处 `clang-format` 差异，
于是补装了一个本地 `clang-format`（23.1.3，与仓库钉死的版本一致），
格式不再只能靠 CI 反馈。

### 16.3　改法：工作线程只产生字节

一句话：**把「写 socket」从工作线程搬到事件循环**。工作线程跑完 handler、
把整条响应拼好，连同 TLS 会话一起经交接队列还给事件循环；由事件循环非阻塞地写。

- `Conn` 增加写回状态：`out` / `out_off` / `writing` / `keep_alive_after`；
- 新增 `flush_conn()`：能写多少写多少，`EAGAIN` 就注册 `EPOLLOUT` 等下一轮；
- 新增 `write_deadline`：停滞超过 `kSendTimeoutMs` 仍然回收；
- `Recycled`（交还的 fd）改成 `Completed`（交还的响应字节），
  `recycle_conn` → `queue_completed`，`drain_recycled` → `drain_completed`。

改完之后顺手把 fd 所有权的表述收紧成一条不变量，写进了头文件：

> 一条 fd 在任意时刻只有一个所有者：在 `conns_` 里归事件循环，在 `queue_` 的 `Task` 上归
> 工作线程（工作线程从不关闭它），在 `completed_` 里归事件循环，事件循环已退出时归 `Stop()`。

这比改之前更强：**改之前工作线程是会关 fd 的**（写失败就关），于是「谁该关」有两个答案。
现在只有一个。

### 16.4　四个必须同时成立的前提

1. **事件分发不能按事件位分流。** 写回也用 `EPOLLOUT`，而 `EPOLLOUT` 原本只服务于 TLS 握手。
   第一版写成「`EPOLLOUT` 走写回、否则走读」，结果是「握手需要回写」被误送进写回逻辑——
   握手永远推不完，要等 10 秒空闲回收才断。正确做法是仍然一律进 `handle_read`，
   由它按**连接状态**分流。这个错在写代码的当下就被复审抓到了，但它说明一件事：
   给一个共享的标志位增加第二个使用者时，分流依据必须从「事件」换成「状态」。
2. **TLS 要打开两个 mode。** `SSL_MODE_ENABLE_PARTIAL_WRITE`（否则一条 8MB 响应要么全写完、
   要么 `WANT_WRITE`，剩下的全靠 OpenSSL 内部缓存，缓存多少由对端读得多快决定）；
   `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`（否则 `WANT_WRITE` 之后带着新偏移量重试会被判成
   `bad write retry`，连接当场被掐断）。后一个是纯知识型陷阱：不打开它，代码在
   「响应小到一次写完」的所有测试里都是绿的。
3. **一次 flush 要让出事件循环。** 写本身是 memcpy，但事件循环是单线程的：
   一条 8MB 响应一口气推完，这一轮里其它连接的响应就被推迟了。
   上限取 256KB——多花的只是一次 `epoll_wait` 往返。能给这个上限，正是因为
   `EPOLLOUT` 是水平触发的：没写完，下一轮一定会再进来。
4. **超时要从「总时长」改成「停滞时长」。** 原来的 `SO_SNDTIMEO` 是单次 `send()` 的上限；
   现在没有任何一次调用会等，所以判据必须变成「距离上次写出有进展过了多久」。
   否则一条给慢客户端正常吐 8MB 的连接会被误杀。

### 16.5　超时语义没变，变的是「等的时候占着什么」

这一批刻意没有改 `kSendTimeoutMs = 2000`。客户端能拖多久、服务端多久放弃，
这两个对外可见的量一个都没动——变的只是「等」的时候占着什么：
原来占一个工作线程，现在占一个 `EPOLLOUT` 关注位。

代价是明确的，也写进了注释：一条写不动的连接会**持有整条响应在内存里**。
最坏情况下 8192 条连接 × 单条响应体，这是新的内存上界。
它不比改之前更糟（改之前那条响应同样在内存里，而且还额外占一个 worker），
但它确实把「并发连接上限」和「内存上限」绑在了一起——这条写进了 16.7。

### 16.6　实测

| 项 | 值 |
|---|---|
| 单元测试 | **9/9**（新增 `test_worker_isolation`；`test_tls` 增加一组 4MB 响应，覆盖 TLS 记录层的分片写出与重试） |
| Sanitizer | ASan + UBSan、TSan 全通过（写回状态在事件循环内，跨线程只有一条队列） |
| 行覆盖率 | **79%**（2242 行中 1771 行；上一批 2165/1723。新增约 77 行，覆盖率没跌） |
| fuzz | 三目标各 30s 通过（41.3 万 / 416 万 / 83.8 万次） |
| CI | 六 job 全绿（`a7d7025`）；上一次 `a62ffc8` 是刻意留的红，用于反证 |

覆盖率数字来自 workflow 打出的 `::notice::` 注解，可用匿名 API 核对，不依赖仓库权限：

```
GET /repos/{owner}/{repo}/check-runs/{id}/annotations
→ 行覆盖率 | TOTAL   2242   1771   79%
```

### 16.7　改完之后还欠什么

- **handler 本身仍可能阻塞一个 worker**：回源到慢对端时，`HttpClient` 的超时就是
  worker 被占用的上限。这一批关掉的是「客户端能把 worker 按死」，
  没关掉「对端慢会占着 worker」——后者要靠异步转发或协程，是另一个量级的改造。
- **内存上界变成「连接数 × 响应体」**：见 16.5。要真正封顶，得给单条响应加大小上限，
  或者在写回队列上做全局字节配额。
- **TLS 的 `WANT_READ`（KeyUpdate）直接关连接**：写方向要读、而我们没法在读之前推进这次写，
  继续重试就是 `EPOLLOUT` 空转。选择关掉是明确的取舍，不是遗漏。
- **证书吊销（CRL / OCSP）** 仍然没有：证书泄露后的响应手段还是「换一套证书 + reload」。
- **fuzz 在 CI 里只有 30 秒**：夜间长跑仍然是欠的。

---

## 十七、第 10 批：`\uXXXX` 是码元，不是码点（2026-11-01）

这一批的输入不是自查，是一份外部 review 清单。逐条核对之后：
**2 条把机制读反了**（第 3、6 条），**1 条结论对但机制说错**（第 10 条），
**2 条把影响说大了**（第 2、5 条），**2 条给的建议本身会引入新问题**
（第 3 条的 `volatile`、第 9 条的返回 `const V&`）。
真正算缺陷的只有第 1 条（第 2 条的 `atoi` 与第 9 条那次多余拷贝顺带清理了，
见 17.7；剩下的是取舍、或者前提不成立）。核对过程本身值得单独写一节——
它比结论更能说明「审查意见该怎么用」。

### 17.1　十条意见里，两条把机制读反了

清单的行号全对：`json.cpp:163`、`node.cpp:169`、`auth.hpp:16-30`、
`http_server.cpp:601`、`json.cpp:24-58`、`http_server.cpp:436`，一处不差。
说明对方确实打开过文件。错的是「读出了什么」：

| # | 清单结论 | 核对到的事实 |
|---|---|---|
| 1 | 不处理代理对 | ✅ **成立**（见 17.2） |
| 2 | `atoi` 处理 hops 会溢出 | ⚠️ 有 `atoi`，但前提不成立：这行在 `IsTrustedRequest` 通过之后才执行，且结果立刻被夹进 `[0, kMaxForwardHops]` |
| 3 | `diff \| (ca^cb)` 会被编译器短路 | ❌ `diff` 是 `return diff == 0` 的输入；循环体内没有数据相关分支，**没有可短路的位置** |
| 4 | 不支持 chunked | ✅ 存在，且 `http_parser.cpp:7` 就写着这是刻意取舍 |
| 5 | pipelined 字节被丢弃 | ✅ 存在，但定性过重：发 `Connection: close` 再关是规范允许的，且因此不存在走私风险 |
| 6 | `find_after_key` 只扫顶层 | ❌ **说反了**：它不跟踪深度，**会**命中嵌套里的第一个 |
| 7 | 几乎没有运行时日志 | ✅ 存在 |
| 8 | `atomic_load(shared_ptr)` 在 C++20 弃用 | ✅ 存在（本项目 C++17，故当前无告警） |
| 9 | `get()` 拷贝 V | ✅ 存在，但建议的「返回 `const V&`」会引入 use-after-free |
| 10 | `split_addr` 不支持 IPv6 | ⚠️ 结论对、机制错：`[::1]:8000` 靠 `rfind` 其实切得对，坏在方括号没剥；真切错的是裸 `::1` |

这几条错得比缺陷本身更值得记：**它们每一条都能编出一段看起来合理的修改**。
按第 3 条给 `diff` 加 `volatile`，只会让循环失去向量化、一点也没更安全
（那是个错误的诊断：真正历史上的风险是 loop-idiom 把比较循环改写成
`memcmp`，而这里算的是 XOR 的按位或归约，不匹配那个模式）；
按第 6 条把 `find_after_key` 改成跟踪深度，会改掉「取第一个匹配」这个现有语义
而收益为零；按第 9 条返回引用，直接换来一个并发下的悬垂引用。
照单全改，会得到一个更慢、更复杂、并且多了一个 use-after-free 的解码器，
而真正的缺陷还在原地。

### 17.2　缺陷：0xD83D 不是字符

JSON 的 `\uXXXX` 转义的是一个 **UTF-16 码元**，不是 Unicode 码点。两者只对
BMP 内的字符等价；BMP 外的字符（emoji、部分生僻字）在报文里是一对代理：
😀 = U+1F600 = `\uD83D\uDE00`。

旧实现让每个 `\uXXXX` 独立走 `append_utf8`，于是 0xD83D 落进「小于 0x10000」
的三字节分支：

```
0xD83D → ED A0 BD        0xDE00 → ED B8 80
```

这是把 UTF-16 码元当码点编码的经典错误（CESU-8 风格），而 **RFC 3629 明确禁止
把 U+D800..U+DFFF 编进 UTF-8 字节流**。后果分两级：对端拿到非法字节，
下游 JSON 解析器通常直接判报文畸形（一次「命中」变成 503）；
下游若宽松，就是静默乱码。

**可达性要说清楚**：这条路径只在「对端不是我方二进制」时走得到——异构 encoder、
中间代理，或者以后接入的真实数据源（用户昵称里一个 emoji 就够）。同构集群里
`JsonEscape` 对非 ASCII 原样透传、从不产出 `\uXXXX`，所以自己人之间碰不到。
按当前暴露面是 P2，按设计是 P1；两种说法都不算错。

### 17.3　九批 review、ASan、fuzz 都没抓到，原因是一条

三个工具各自看不见它，而且是三个不同的原因：

1. **人工 review 只确认了「分支存在」。** `case 'u'` 里确实在拼字节、确实调了
   `append_utf8`，看着是工作的。要看出错，得知道 `\uXXXX` 是码元而不是码点——
   这是知识问题，不是注意力问题。
2. **fuzz 从构造上摸不到。** 往返断言是 `Decode(Encode(x)) == x`，而 `JsonEscape`
   对非 ASCII 原样透传、**从不产出 `\uXXXX`**。也就是说这条目标只能测
   「我方 encoder 产生的输入」，测不到「别人产生的输入」。
3. **ASan / TSan 与语义无关。** 越界会报、竞争会报，而 `ED A0 BD` 既不越界
   也不竞争——它只是「合法进程里的非法字节」。

第 2 条是这一批最该带走的东西：**往返测试天生有盲区，它测的是自己和自己一致，
不是自己符合标准。** 第 6 批 fuzz 首轮撞出 P0，容易让人以为「fuzz 上线了，
语义层就有人看着了」；其实那次成功靠的是输入空间里恰好有，不是方法上必然。

### 17.4　先红：四个 job 红在同一条断言上

`9b9372a` 只加断言、不动实现：

```
FAILED: JsonUnescape("\\ud83d\\ude00") == emoji
test_json ........................***Failed    0.00 sec
```

`build-and-test` / `sanitizers` / `tsan` / `coverage` 四个 job 同时红，`lint` 绿
（格式没问题，逻辑错了）。同一提交里那条新 fuzz 不变式也当场撞了：
语料 `surrogate_pair.json` 一喂进去就报「输出不是合法 UTF-8」。

一条提交拿到两份独立证据：单元测试说「这条具体向量错了」，fuzz 说「这类输入整类都错」。

### 17.5　改法：把「读四位」的边界收进一个函数

代理对要求**连续读两次** `\uXXXX`，而第二个的起点是算出来的。原来那段
`for (k = 1; k <= 4; ++k)` 内联在 `case 'u'` 里，再复制一份给第二个转义，
就有两处 `i+4` 要各自维护——这种错 ASan 只在恰好撞到页尾时才报，可以潜伏很久。
所以先抽 `parse_hex4(s, pos, out)`，边界只写一次。

然后：

- 高代理（0xD800..0xDBFF）：向后偷看一个 `\uXXXX`，若落在 0xDC00..0xDFFF 就合并成
  `0x10000 + ((hi-0xD800) << 10) + (lo-0xDC00)`，再 `i += 6` 跨过第二个转义的
  6 个字符；
- 不是低代理（语法不对、越界、或 `\uD800\u0041` 这种假配对）：**不吞掉后一个**，
  高代理自己替换成 U+FFFD，后面那个码元照常解析；
- 低代理单独出现：同样替换成 U+FFFD。

U+FFFD 是 JSON 生态的通行做法（WHATWG、多数语言标准库），但重点不是「换成哪个
字符」，而是**任何路径都不能再产出 ED A0 xx**。

改完必须防的反方向是「修得太狠」：把代理区两侧一起替换掉。所以断言里专门留了
两个边界向量——`\uD7FF` 仍是 `ED 9F BF`、`\uE000` 仍是 `EE 80 80`。

### 17.6　然后 CI 又红了：不变式少了一个前提

修完代理对，fuzz 仍然红，报的是**同一条**不变式，但崩溃 hash 换了一个
（`974e9562` → `d1accf95`）。推演后确认：不是还有第二个代理对漏洞，
而是**不变式写得太过**。

`JsonUnescape` 对非转义字节是逐字节透传的，这是设计：缓存值可以是任意字节串，
这一层要是「顺手做 UTF-8 校验并修复」，就会静默改掉用户数据——那比留一个乱码
更糟，因为它是静默的。所以喂进 `0xFF` 这类字节、输出跟着畸形，是调用方违反了
「JSON 文本本身是合法 UTF-8」这个前提，不是 decoder 的缺陷。

不变式改成「输入合法 UTF-8 时，输出也必须合法」，同时补两条断言把透传语义钉住
（`0xFF`/`0x80` 透传、`0x01` 走 `\u0001` 往返）。这样「为什么必须有这个前提」
在代码里就有答案，不用靠提交信息考古。

这是**第二次**栽在同一件事上：第 8 批的 `handshake_failures >= 1` 也是
「对窄场景成立、对全场景恒假」。那次是 TLS 1.3 的时序，这次是输入前提。
恒假的断言比没有断言更糟——它会把真信号淹掉，而这次它恰好淹在了修复提交里。

### 17.7　三处顺手清理，以及它们为什么没有断言

| 位置 | 改动 | 为什么没有会红的断言 |
|---|---|---|
| `peer_pool.cpp:28` | `atoi` → `strtol` | `atoi` 溢出是 UB，UB 不产生可复现的错误答案 |
| `node.cpp:169` | `atoi` → `strtol` | 同上；这处原本还被 clamp 压住，改的是「不让下游去夹上游的 UB」 |
| `group.cpp:22` | `return *cached` → `std::move(*cached)` | 少一次拷贝在语义上不可观测 |

`split_addr` 那处比 `node.cpp` 更需要改：它没有下游夹取兜底。顺带把一个既有的
宽松行为收紧了——`"127.0.0.1:8000abc"` 以前被 `atoi` 当成 8000，现在判失败。
**配置写错应该在启动时炸，而不是安静地用一个猜出来的端口。**

第三行还说明我自己那份核对漏了一层：清单只提到 `get()` 里那次拷贝，实际上
命中路径有**两次**——`get()` 内一次（出锁必需），`group.cpp` 里
`return *cached` 又一次（纯浪费，因为 `*cached` 是左值）。

### 17.8　实测

| 项 | 值 |
|---|---|
| 单元测试 | **9/9**（`test_json` 新增 20 条断言：成对合并、孤立代理、假配对、截断、边界两侧、透传语义） |
| Sanitizer | ASan + UBSan、TSan 全通过 |
| 行覆盖率 | **79%**（2261 行中 1806 行；上一批 2242/1771。新增 19 行，覆盖掉的比新增的多） |
| fuzz | 三目标各 30s 通过（43.6 万 / 310 万 / 83.6 万次；json 目标从上一批的 41 万涨到 83.6 万，因为语料里多了两个代理样本） |
| CI | 六 job 全绿（`b43e283`）；过程中红了两次：`9b9372a`（只有断言的版本）、`f7b7044`（不变式缺前提） |

```
GET /repos/{owner}/{repo}/check-runs/{id}/annotations
→ 行覆盖率 | TOTAL   2261   1806   79%
```

### 17.9　这一批不加分，以及为什么

评分保持 **90（A−）**。

理由在第一节的注里：这是还债，不是度量新增。但补一句不那么舒服的：
**这个缺陷从第一版就存在，按规矩它本来就该扣分，扣到 89 也说得过去。**
之所以没往回扣，是因为每次往回改分数，评分就会变成一件可以商量的事——
分数只在有新的、可验证的度量时才动，否则不动。

还欠的三条没有变化：handler 回源仍占 worker、fuzz 只跑 30 秒、无 CRL/OCSP。

---

## 十八、第 11 批：压测口径（2026-11-02）

这一批的输入也是外部 review：「README 里没写『3 节点、10 万 QPS、P99 多少』这种硬数字」，
建议「写一份 benchmark 报告放 README，半天工作量」。

核对之后，结论是：**方向对，工作量估错了一倍，而照着做会产出一份自己打自己脸的数字。**

### 18.1　「半天工作量」低估的是一半代码，不是一半时间

「加个百分位」听起来是一行 `sort` 加一次取值。实际上：

- 分位数至少有四种算法（最近秩 / 线性插值 / 独占 / 包括），选哪种会改变报出来的数；
- 最容易顺手写出来的那版是 `samples[n * p / 100]`——把 0-based 下标当 1-based 秩用。
  n=100 时 P99 报成 **100 而不是 99**，P100 直接越界。

关键在于：**这两种错都不会崩，报出来的数也完全合理。**「P99 = 100µs」没人能靠肉眼判断对错。
所以「跑一次拿到数字」和「数字是对的」之间隔着一步：**先把分位数变成有断言的组件**。

这一批因此先做了 `include/latency_stats.hpp` + `tests/test_latency_stats.cpp`，
再让 bench 去用它——顺序反过来就会产出一份无法自证正确的报告。

### 18.2　三个会改变数字**含义**的错误

它们都不会让程序失败，只是让报出来的数不再是它声称的那个东西。

| # | 错误 | 后果 |
|---|---|---|
| 1 | 把「响应里有值」当成命中率 | mock 对每个 key 都有值，这个比例**恒等于 1.0**。把它写进报告，等于把一个常数当结论 |
| 2 | 不显式配 `--capacity` | 节点默认容量 100、bench 默认 key 空间 1000，命中率上限就是 10%。此时压的是 **LRU 淘汰**，不是缓存 |
| 3 | 把失败请求的延迟混进分位数 | 失败请求测到的是超时上限（2000ms）。混进去会让 P99 变成超时值，而且失败率越高 P99 越像超时——一个由错误率驱动的延迟指标，看着还挺合理 |

第 2 条是最容易踩的，因为它**不会报错**：bench 老老实实压完，给出一份「命中率 10%」的报告，
读起来像「这个缓存不太行」。现在 bench 会在 `cache_size < key 空间` 时直接把上界算出来提示。

命中率改成从节点侧 `/stats` 读（`local_hit / (local_hit + local_miss)`），单列一行 `STATS`——
不跟 `RESULT` 那行混在一起，因为**两边的字段顺序一旦成了紧耦合，改一次就全断**。

### 18.3　断言对着具体写法错误写

`test_latency_stats` 的七组用例，每组对应一种具体的错法：

| 组 | 用例 | 挡住的错 |
|---|---|---|
| 1 | 空集 / 单样本 / 全相同 | 边界与除零（n=1 时算出下标 -1） |
| 2 | 1..100 的 P99 必须正好是 **99** | 下标少减 1（0-based 混用） |
| 3 | n=3 的 P50 必须是 **3** | 用 floor 代替 ceiling（floor 会报 1，那是**最小值**不是中位数） |
| 4 | 倒序输入 | 忘了排序 |
| 5 | 查询之后再 Add | 惰性排序的有序标记没清 |
| 6 | 多线程合并 | 各线程各自算再取平均（**百分位不可平均**） |
| 7 | p 扫 0..1000 单调性 + 越界参数夹取 | p=1000 越界访问 |

另外两条实现约束写进了注释：**秩用整数算**（`(p*n + 999) / 1000` 把「向上取整」
编码进算术本身；算完再取整最常见的写法 `(int)(p*n/1000.0)` 是**截断**，
p=500、n=3 时 1.5 被截成 1、报出第 1 小而不是中位数），以及**千分位接口**
（P99.9 恰恰是「偶发卡顿」最先露头的地方）。

### 18.4　注释里的具体例子，也要能被验算

这一节是本批自己踩的坑，而且踩得有点讽刺。

`latency_stats.hpp` 里有一行注释，原话是「浮点会把 p=999、n=1000 算成 998.999…，
截断后偏一格」。这句话是我自己写的，用来说明**为什么必须用整数式算秩**。

复核时验算了一下：

```
999 * 1000 / 1000 = 999.0        ← 精确可表示，根本不会出现 998.999
```

**这个例子不成立。** 那个组合下三种写法（截断 / 浮点 ceil / 整数式）给出的结果
**完全一致**，都是 999。真正会偏一格的是另一个组合：

| p, n | 截断 `(int)(p*n/1000.0)` | 浮点 ceil | 整数式 `(p*n+999)/1000` | 正确 |
|---|---|---|---|---|
| 999, 1000 | 999 | 999 | 999 | 999（**无差别**） |
| 500, 3 | **1** | 2 | 2 | 2 |
| 999, 1001 | **999** | 1000 | 1000 | 1000 |
| 99, 100 | **9** | 10 | 10 | 10 |

所以正确的说法是：**问题不在「浮点不可靠」，而在「取整那一步太容易被写成截断」。**
`ceil(p * n / 1000.0)` 与整数式完全一致，而 `(int)(p * n / 1000.0)` 少一格。
三处（代码注释、指南 6.7、本节）都已按实测数据改写。

这件事值得单独记一笔，因为它同时踩中了两条：

1. **注释里的具体例子必须能被验算。** 「p=999、n=1000」这种数字看起来专业，
   而且**方向对**（确实是取整的问题），所以几乎不会有人去算它。审阅时也是靠
   真的算了一遍才发现的。
2. **一个用来防止出错的注释里，自己写错了一个例子**——这是最尴尬的一种错。
   读者会照着这句话建立信心，而那句话的证据部分是假的。
   第 6 批那条「JSON 扫描不认字符串边界」的教训是「别信注释里的推断」，
   这一条是「别信注释里的算术」。

同一个自查里还抓到第二处，性质类似：`Mean()` 的注释写着「累加用 double 而不是 int64：
样本数上万、单值上千微秒时 int64 也会逼近溢出线」。验算一下：
1e6 个样本 × 1e4 微秒 = 1e10，而 int64 的上限是 9.2e18——**差着八个数量级**，
根本谈不上逼近。这条注释制造了一个「不这么写会溢出」的假印象，而实际上两者都够用。
已改成「两者都够，而这里要的精度只到 0.1 微秒」。

所以这条规矩的适用范围比「例子」更宽：**注释里的量级估计也要能被验算。**
「差八个数量级」和「逼近溢出线」之间，隔着一句 `1e6 * 1e4 = 1e10` 的乘法。

### 18.5　反证：先证明这些断言真的会红

只改 `include/latency_stats.hpp` 最后一行：`samples_[rank - 1]` → `samples_[rank]`
（夹取同步改成 n-1 以免撞成越界），其余一字未动。提交 `c940ace` 推上去，四个 job 同时变红：

```
FAILED: hundred.Permille(0) == 1
test_latency_stats ...............***Failed    0.00 sec
```

有个细节值得记：**红点落在 `Permille(0)`（最小值）而不是 `Percentile(50)`**。
因为 `CHECK` 遇错即停，落点是组 2 的第一条断言。这反而是好消息——
0-based 下标错会让**所有**分位数整体偏移一格，而在**最小值**处这个偏移最容易被识破
（1..100 的最小值必须是 1，人人心算得出；P99 该是 99 还是 100，差一格很容易被当成取整）。

同一个错如果只测 P99，报出来的是「P99=100」，看起来完全像正常结果。

随后 `git revert`（`06ab419`）恢复。

### 18.6　压测脚本：四套配置，第三套之后补了 TLS

`scripts/bench_perf.sh` 跑三套配置 × 三轮取中位数，差别只在「有没有远端转发」「会不会淘汰」。
本批补了第四套「单节点·TLS」——与「单节点·全命中」**完全相同的负载**，唯一差别是传输层。

它的价值不在绝对值（「TLS 每请求贵多少微秒」），而在**这个比值**：同一台机器、同一份负载、
同一个客户端，唯一变量是传输层。拿别处的数字对比毫无用处，那不是同一个实验。

证书在整轮里只生成一次：换证书会让连接池里的旧会话全部失效，那样测出来的是
「重新握手」而不是「TLS 的稳态开销」。

证书有两个细节，写进注释并**实测过**：

1. **SAN 必须是 `IP:127.0.0.1`**。客户端连的是 IP 字面量，走
   `X509_VERIFY_PARAM_set1_ip_asc`，只认 IP 形式的 SAN。实测把同一份证书的 SAN
   换成 `DNS:127.0.0.1` 之后，`openssl verify -verify_ip 127.0.0.1` 直接报
   `IP address mismatch`。
2. **CA:TRUE**。节点用同一份证书校验对端，而它是自签的；`req -x509` 默认就带
   CA:TRUE，这里显式再写一遍（实测不是叠加，不冲突）。

另外节点侧的 TLS 三个参数是**强制成组**的（cert/key/ca 缺一即拒启动）——入站监听与
出站转发必须同时开关，否则会出现「自己收 TLS、转发明文」这种半吊子配置。

### 18.7　一条操作约束：匿名 API 配额是 60 次/小时

读 CI 注解靠的是未认证 API，额度 **60 次/小时**。本批轮询 CI 状态时把它吃光了，
导致后面所有诊断一起失效（连续几个 429）。

这条不是「技巧」，是**约束**：诊断通道本身有配额，轮询策略必须先算次数预算，
而不是「每 20 秒查一次直到结束」。第 1 到第 10 批的诊断都没踩到它，
是因为那时候轮询次数少——这一批加了 perf 之后轮询变密，问题才浮出来。

### 18.8　实测

数字全部来自 CI 注解（匿名 API 可核对），且**带环境标注**：

```
GET /repos/{owner}/{repo}/check-runs/{id}/annotations
→ bench | 单节点·全命中 nodes=1 capacity=10000 keys=1000 transport=tcp qps=@@QPS_A@@ p50=@@P50_A@@us p99=@@P99_A@@us p999=@@P999_A@@us
→ bench | 单节点·TLS     nodes=1 capacity=10000 keys=1000 transport=tls qps=@@QPS_D@@ p50=@@P50_D@@us p99=@@P99_D@@us p999=@@P999_D@@us
```

| 项 | 值 |
|---|---|
| CI | 六项门禁全绿（`06ab419` 代码 / `@@DOC_SHA@@` 文档）；`perf` 只在 `perf/**` 分支上跑，其余情况下是 skipped |
| 单元测试 | **10/10**（新增 `test_latency_stats`，7 组） |
| 行覆盖率 | @@COV@@ |
| 反证 | `c940ace` 红在 `FAILED: hundred.Permille(0) == 1`，`06ab419` revert |
| 环境 | GitHub Actions `ubuntu-latest` 共享 runner，@@CPU@@ 逻辑核；**绝对 QPS 不可迁移** |

**这个环境标注不是客套。** 共享 runner 的 vCPU 会被邻居抢，同一个提交两次跑能差 20%。
可比的只有**同一份报告内部**的相对差异——「单节点·全命中」与「单节点·TLS」的差值、
「全命中」与「会淘汰」的差值。

### 18.9　评分为什么 +1，以及它和第 10 批差在哪

评分 90 → **91**。

第 10 批修的是「一直是错的代码」，按规矩不加分。这一批不一样，它补的是
**一个此前根本不存在的度量维度**：延迟分布。连 bench 的输出里都没有 P99 这个数，
更谈不上它算得对不对。现在它有了独立组件、有七组对着具体错法的断言、有一次反证，
以及一份带环境标注的真实数字。

判据写在这里，免得下次按有利方向解释：

> **新增一个此前不存在的可验证维度 → 加分。修正一个一直是错的实现 → 不加分。**

这条判据也解释了第 9 批的 +1：慢客户端隔离此前只是设计承诺，从未有断言。

还欠的三条没有变化：handler 回源仍占 worker、fuzz 只跑 30 秒、无 CRL/OCSP。

---

## 十九、给团队的一句话

这套代码的**算法直觉**和**注释自觉**都在线——`singleflight` 的异常传播、`murmur3` 的 UB 规避、TTL 抖动的除零保护，这些不是照抄能写出来的。真正欠的是「系统工程师的那一层肌肉」：谁在什么时候拥有这个 fd、失败和空值是不是同一件事、外部输入能不能决定内部行为、关掉再打开还能不能用。

6 个 P0 已全部关闭，评分从 65 到 91。十一批之后，清单上「设计层面的欠账」已经没有结构性条目了：
失败语义、信任边界、连接生命周期、可观测性、覆盖率度量、模糊测试、传输层加密、
证书轮换、慢客户端隔离、解码器的字节契约、延迟分布的可信口径，都有了明确答案。
还欠的三条都在「更进一步的工程投入」这一侧（handler 异步化、证书吊销、fuzz 长跑），
不再是「哪里想错了」。

如果这次只带走一句话：**没有实跑的测试等于没有测试**。这句话在 CI 首跑时被具象化了——
四个 job 全红，其中三条失败都指向「写了断言但断言不测东西」。而那一批改动里最危险的三处
（keep-alive 的 fd 交还、`EMFILE` 自救、`EPOLLIN|EPOLLHUP` 顺序）恰恰都属于
「写错了也不会编译报错」的类型，只有跑起来才能证明。

第二个更硬的结论来自 fuzz：**人工 review 有结构性盲区**。四个批次、六个 P0 全部集中在并发和
资源生命周期上，谁也没去看「文本扫描会不会越界到字符串内部」——而 fuzzer 用 30 秒就撞开了，
而且撞出来的后果不是崩溃，是**语义反了**（命中被读成未命中）。这类 bug 不会在日志里尖叫，
只会让缓存悄悄退化成直连。静态阅读永远会替代码脑补「这里应该是字段名」。

第三个结论来自第 8 批的 SIGPIPE：**最危险的缺陷往往不在某条代码里，在两条路径的差异里**。
明文路径两侧都写了 `MSG_NOSIGNAL`，TLS 路径绕开了它们，于是一次普通连接就能让节点退出。
ASan 看不到信号，TSan 不涉及竞争，fuzz 不碰 socket，四批 review 看的是「这段代码对不对」——
四条防线各自都漏了，因为它们检查的对象都是「某一条路径」，而没人问「另一条路径一样吗」。
同一批里那条靠时序通过的假断言也是这个形状：它证明了「客户端连不上」，
却顺手断言了一件服务端在 TLS 1.3 下永远观察不到的事。

第四个结论来自第 9 批，它是唯一一批**先红后绿**走完的：先只提交测试，让 CI 红在
`!resp.empty()` 上，再动实现。前一晚我在这条欠账前停了很久，理由写下来是「改法无法用
崩不崩证明」——后来意识到那句话其实是在说「我还不知道怎么把它变成一条断言」。
一旦先写出那条断言，改法自己就浮出来了：**难的不是改，是先把「改好了」定义清楚。**

同一批里还有个更小的教训：**给共享的标志位增加第二个使用者时，分流依据要从「事件」换成「状态」**。
第一版把 `EPOLLOUT` 当成「要写响应」的判据，直接撞上「TLS 握手也要回写」——
两件事共用一个事件位，而判据问的是事件，不是「这条连接现在处在哪一阶段」。

第五个结论来自第 10 批，它把这个方法论又推进了一层：**往返测试只能证明「自己和自己一致」，
不能证明「自己符合标准」**。`Decode(Encode(x)) == x` 这条断言十年不会红，因为 `Encode` 从来不
产出它不认识的东西；而 `\uD83D` 该不该被当成一个字符，答案不在这一对函数里，在 RFC 3629 里。
更要紧的是它顺带解释了第 6 批那次 fuzz 成功为什么不可复制：fuzzer 撞开的是「输入空间里恰好有」，
不是「方法上必然」。所以第 10 批补的那条不变式带了一个前提（输入须是合法 UTF-8）——
而第一版忘了这个前提，fuzz 立刻用一条恒假断言把它打了回来。

第六个结论来自第 11 批，它讲的是一份数字在被相信之前要过的那一关：
**「量到了」和「量的是你以为的那个东西」之间隔着一步。**
外部建议里那句「写个 benchmark 报告，半天工作量」——方向完全对，工作量估错了一倍，
而照着做会产出一份自己打自己脸的数字：节点的默认容量是 100、bench 的默认 key 空间是 1000，
不显式配 `--capacity`，命中率上限就是 10%，压出来的是 LRU 淘汰，读起来像「这个缓存不太行」。
另外两个错误更隐蔽：「响应里有值」的比例在 mock 下恒等于 1.0（它不是命中率），
失败请求的「延迟」测到的是超时上限（混进分位数会让 P99 变成超时值）。
这三个都不会让程序失败，它们只是让数字不再是它声称的那个东西。
所以顺序必须是：**先把分位数变成有断言的组件（错出来的数不会崩，只能靠断言钉住），
再拿它出数**。这一批还留了一条反证证据：把 0-based 下标当 1-based 秩用，
四个 job 同时红在 `FAILED: hundred.Permille(0) == 1`——红点落在**最小值**上，
因为那个错会让全部分位数整体偏移一格，而在最小值处最容易识破。

同一批还有一处是**我自己踩的**：写完分位数组件后，我在注释里加了一句
「浮点会把 p=999、n=1000 算成 998.999…，截断后偏一格」，用来解释为什么必须用整数式。
复核时真的算了一遍——`999 * 1000 / 1000 = 999.0`，精确可表示，**根本不会出现 998.999**；
那个组合下三种写法结果完全一致。真正会偏一格的是 `p=500, n=3`（1.5 被截成 1）和
`p=999, n=1001`（999.999 被截成 999）。

这句话错得很有欺骗性：方向对（确实是取整的问题）、数字专业、**几乎没人会去算它**。
而它恰好写在一条**用来防止出错的注释里**——读者会照着它建立信心，而它的证据部分是假的。
所以追加一条规矩：**注释里的具体例子必须能被验算，尤其是数字。**
第 6 批学到的是「别信注释里的推断」，这一条是「别信注释里的算术」。

最后一节留给这条方法论：**审查意见也要经得起反证**。这是第五次验证它了：
第一次是「fd 代次」，我列成待修项、推演后发现根本不成立，于是收回；
第四次是第 10 批那份十条清单——行号全对，机制却有两条读反、一条说错，
其中三条建议（给 `diff` 加 `volatile`、把 `find_after_key` 改成跟踪深度、让 `get()` 返回引用）
每一条都能编出看起来合理的代码，而照做会换来「更慢、更复杂、并且多了一个 use-after-free」
的解码器，真正的缺陷还在原地。

**审查意见的价值不在它有多详细，在它能不能被证伪。** 一条「这里可能有问题」，
如果推演三步就散架，那就该收回；如果推演三步反而更清楚，那才值得动手。
第五次是第 11 批那条「半天工作量」——它低估的不只是时间，还有**代码量**：
「加个百分位」听起来是一行 `sort`，实际上难点在于错出来的数**依然看着完全正常**，
没人能靠肉眼判断对错，于是「跑一次拿到数字」和「数字是对的」之间隔着一步。
照着原建议做会得到一份「命中率 10%」的报告——不是假的，是**测错了对象**。

这十一批里，真正让代码变好的从来不是「意见多」，是「每一条意见都被要求给出一个
能红的证据」——给不出来的时候，要么是理解错了，要么是这条意见本身不成立。
