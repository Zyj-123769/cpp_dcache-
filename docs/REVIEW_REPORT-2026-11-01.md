# 代码审查报告 — dcache 分布式缓存

- **审查日期**：2026-11-01（同日完成六批修复 + CI 实跑，附修复前/后评分）
- **审查范围**：`include/` `src/` `apps/` `tests/` `fuzz/` 全部约 3600 行 C++17 代码
- **审查方式**：静态通读 + 并发/资源/协议路径推演；四批修复后在 GitHub Actions（Ubuntu）实跑
  build / ASan+UBSan / TSan / clang-format+clang-tidy / gcovr / libFuzzer 六项门禁，全部通过
- **结论**：修复前 **65 / 100（C）** → 修复后 **87 / 100（B+）**；6 个 P0 全部关闭，
  连接层、信任边界、可观测性、工程化补齐，且**已在 Linux 上实跑验证**；
  第 6 批 fuzz 上线又揪出并修掉 1 个 P0 级缺陷（JSON 扫描不认字符串边界）

---

## 一、项目评分

| 维度 | 权重 | 修复前 | 修复后 | 判定依据 |
|---|---|---|---|---|
| 正确性 | 25% | 62 | 82 | 「失败」与「不存在」已拆成三态，错误不再写入负缓存；Linux 实跑 6/6 用例通过 |
| 并发安全 | 20% | 55 | 86 | 哈希环原子快照、关停顺序修正、重启可用；**TSan 全量回归零报告** |
| 安全性 | 15% | 50 | 84 | hops 改为 token 校验（常量时间比较）+ 伪造可观测、发送超时、连接上限、空闲回收、双向大小上限 |
| 可维护性 | 15% | 88 | 91 | 分层干净、注释解释「为什么」；全仓 clang-format 零 diff，格式不再靠人工维持 |
| 测试 | 12% | 68 | 91 | 13 段回归实跑通过；**行覆盖率 79%** 设了门禁；**三个 libFuzzer 目标**首轮即撞出一个 P0 级缺陷 |
| 工程化 | 8% | 65 | 92 | `git init` + 双远端、CI 六 job 全绿、`scripts/ci.sh` 单一入口、lint 与覆盖率工具版本钉死、CI 结论走注解 |
| 文档 | 5% | 90 | 90 | README 与头文件注释质量高 |

**加权总分：修复前 65.1 → 65 分（C）；修复后 86.7 → 87 分（B+）**

> 未到 90 的扣分点很具体：节点间仍是明文 HTTP（token 可被嗅探）、worker 是阻塞模型、
> fuzz 在 CI 里只跑 30 秒（没有夜间长跑）。
> 另外正确性仍是 82：fuzz 揪出的那个缺陷说明「文本扫描类代码」还有没被覆盖的想象空间，
> 这一条不是靠加分能抹掉的。

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

## 十四、给团队的一句话

这套代码的**算法直觉**和**注释自觉**都在线——`singleflight` 的异常传播、`murmur3` 的 UB 规避、TTL 抖动的除零保护，这些不是照抄能写出来的。真正欠的是「系统工程师的那一层肌肉」：谁在什么时候拥有这个 fd、失败和空值是不是同一件事、外部输入能不能决定内部行为、关掉再打开还能不能用。

6 个 P0 已全部关闭，评分从 65 到 87。六批之后，清单上「设计层面的欠账」基本清完了：
失败语义、信任边界、连接生命周期、可观测性、覆盖率度量、模糊测试都有了明确答案。

如果这次只带走一句话：**没有实跑的测试等于没有测试**。这句话在 CI 首跑时被具象化了——
四个 job 全红，其中三条失败都指向「写了断言但断言不测东西」。而那一批改动里最危险的三处
（keep-alive 的 fd 交还、`EMFILE` 自救、`EPOLLIN|EPOLLHUP` 顺序）恰恰都属于
「写错了也不会编译报错」的类型，只有跑起来才能证明。

第二个更硬的结论来自 fuzz：**人工 review 有结构性盲区**。四个批次、六个 P0 全部集中在并发和
资源生命周期上，谁也没去看「文本扫描会不会越界到字符串内部」——而 fuzzer 用 30 秒就撞开了，
而且撞出来的后果不是崩溃，是**语义反了**（命中被读成未命中）。这类 bug 不会在日志里尖叫，
只会让缓存悄悄退化成直连。静态阅读永远会替代码脑补「这里应该是字段名」。

最后一句留给这次最值得记住的方法论：**审查意见也要经得起反证**。上一轮我把「fd 代次」列成待修项，这一轮逐条推演后发现根本不成立——于是收回，并把省下的精力放到 EMFILE 自旋、`EPOLLIN|EPOLLHUP` 丢请求这些真问题上。如果当时照着清单埋头改，会得到一个更复杂、但一点也没更安全的服务器。
