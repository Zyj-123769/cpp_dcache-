# 代码审查报告 — dcache 分布式缓存

- **审查日期**：2026-11-01（同日完成三批修复，附修复后评分）
- **审查范围**：`include/` `src/` `apps/` `tests/` 全部 2791 行 C++17 代码
- **审查方式**：静态通读 + 并发/资源/协议路径推演（未在 Linux 环境实跑，epoll 与本机平台不符）
- **结论**：修复前 **65 / 100（C）** → 修复后 **80 / 100（B）**；6 个 P0 全部关闭，连接层与工程化补齐，余项为「需实跑验证」与长期加固

---

## 一、项目评分

| 维度 | 权重 | 修复前 | 修复后 | 判定依据 |
|---|---|---|---|---|
| 正确性 | 25% | 62 | 80 | 「失败」与「不存在」已拆成三态，错误不再写入负缓存；残余：getter 抛异常仍靠 HTTP 层兜底 |
| 并发安全 | 20% | 55 | 80 | 哈希环原子快照、关停顺序修正、重启可用；已论证 fd 所有权唯一、无需代次 |
| 安全性 | 15% | 50 | 80 | hops 仅可信来源 + 钳制、发送超时、连接上限、空闲回收、请求/响应双向大小上限 |
| 可维护性 | 15% | 88 | 90 | 分层干净、命名一致、注释解释「为什么」；三态让调用方语义更明确 |
| 测试 | 12% | 68 | 78 | 新增 7 段回归（异常/重启/vnode/错误不缓存/503/半关/FIN/超限）；**全部尚未实跑** |
| 工程化 | 8% | 65 | 75 | `git init` 完成、CI 四 job、lint 规则、`.gitattributes`；CI 尚未首次运行 |
| 文档 | 5% | 90 | 90 | README 与头文件注释质量高 |

**加权总分：修复前 65.1 → 65 分（C）；修复后 81.4 → 记 80 分（B）**

> 折减 1.4 分只为一件事：**21 段断言全部还没有在 Linux 上跑过一次**。CI 首跑全绿后这一项可直接补回。

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

## 十、给团队的一句话

这套代码的**算法直觉**和**注释自觉**都在线——`singleflight` 的异常传播、`murmur3` 的 UB 规避、TTL 抖动的除零保护，这些不是照抄能写出来的。真正欠的是「系统工程师的那一层肌肉」：谁在什么时候拥有这个 fd、失败和空值是不是同一件事、外部输入能不能决定内部行为、关掉再打开还能不能用。

6 个 P0 已全部关闭，评分从 65 到 80。剩下的不再是「设计走错了」，而是「可以按清单慢慢补」：CI 真正跑一次、内部 token、连接复用、指标导出。

最后一句留给这次最值得记住的方法论：**审查意见也要经得起反证**。上一轮我把「fd 代次」列成待修项，这一轮逐条推演后发现根本不成立——于是收回，并把省下的精力放到 EMFILE 自旋、`EPOLLIN|EPOLLHUP` 丢请求这些真问题上。如果当时照着清单埋头改，会得到一个更复杂、但一点也没更安全的服务器。
