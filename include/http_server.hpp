#pragma once

#include "http.hpp"
#include "tls.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dcache {

// 极简非阻塞 epoll HTTP 服务器（Level-Triggered）+ 工作线程池。
//
// 设计取舍：
//   - 事件循环线程只负责 accept / 读请求；完整的请求交给工作线程池处理，
//     避免「同步转发 / 慢回源」在事件循环里阻塞整节点（连本地命中也被拖垮）
//     的级联串行化问题；
//   - 连接默认「处理完一条请求即关闭」；只有客户端显式发送 Connection: keep-alive
//     时才保持连接（处理完把 fd 交还事件循环重新挂回 epoll）。
//     做成显式 opt-in 而不是 HTTP/1.1 默认长连接，是为了让默认行为与改造前逐字节一致：
//     新路径只在自己人之间启用，真出问题也炸不到普通客户端。
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 绑定 addr:port 并启动后台线程；失败返回 false。
    // addr 支持 "0.0.0.0"（监听所有网卡）与 "127.0.0.1" 等 IPv4 字面量。
    // worker_count 为处理请求的工作线程数（默认 4）。
    //
    // tls_cfg 为空（默认）表示明文监听，行为与改造前逐字节一致。
    // 给了证书与私钥则整个监听口都走 TLS —— 不按协议嗅探、不做「明文/TLS 同端口」，
    // 因为那种设计要靠读第一字节猜协议，猜错一次就是把 TLS 降级成明文。
    bool Start(const std::string& addr, std::uint16_t port, Handler handler, int worker_count = 4,
               const tls::ServerConfig& tls_cfg = tls::ServerConfig{});

    // 停止服务器（阻塞等待后台线程退出）。
    void Stop();

    // 实际监听的端口（传入 0 时返回系统分配的端口）。
    std::uint16_t Port() const { return port_; }

    // 服务端指标快照。全部来自原子量，任意线程可安全读取。
    //
    // 为什么要单独统计而不是「让 handler 自己数」：handler 只看得见进入业务逻辑的请求，
    // 报文非法、被连接上限直接拒掉的请求根本到不了 handler，而这些恰恰是攻击的特征。
    struct Metrics {
        std::uint64_t requests_total   = 0;  // 进入处理流程的请求数
        std::uint64_t requests_dropped = 0;  // 报文非法/声明体过大，未产生响应的请求数
        std::uint64_t responses_2xx    = 0;
        std::uint64_t responses_4xx    = 0;
        std::uint64_t responses_5xx    = 0;
        std::size_t active_conns       = 0;  // 当前挂起的连接数（含 keep-alive 空闲连接）

        // TLS 握手失败数。不用来「挡攻击」，用来发现配置错误：
        // 对端没开 TLS、证书过期、CA 没同步，都会先在这里涨起来。
        // 挂在这里而不是 handler 里，是因为握手失败的连接根本到不了 handler。
        std::uint64_t handshake_failures = 0;
        bool tls_enabled                 = false;
    };
    Metrics GetMetrics() const;

private:
    struct Task {
        int fd = -1;
        // TLS 会话（明文模式下为 nullptr）。它必须跟着 fd 一起跨线程：
        // 工作线程要用它写响应，而 SSL 对象只能有一个所有者。
        std::unique_ptr<tls::Session> ssl;
        HttpRequest req;
        // 本次读取的缓冲区里还有后续请求（pipelining）。有的话不能复用连接：
        // 那些字节在交接时会被丢弃，复用只会让下一个请求拿到错乱的响应。
        bool pipelined = false;
    };

    // 工作线程处理完 keep-alive 请求后交还事件循环的连接。
    // remote_ip 必须一起带回来：复用的连接上后续请求仍要做「是否内部请求」判定，
    // 丢了它就会把对端降级成不可信来源。
    struct Recycled {
        int fd = -1;
        std::unique_ptr<tls::Session> ssl;
        std::string remote_ip;
    };

    void run();
    void handle_read(int fd);
    void handle_request(int fd, HttpRequest req, std::unique_ptr<tls::Session> ssl, bool pipelined);
    void worker_loop();
    void cleanup();

    // 关闭连接（幂等）：已移交给工作线程的 fd 不在此表内，不会被重复 close。
    // 会话对象由 Conn / Task / Recycled 三处的 unique_ptr 负责释放，
    // 这里只管从 conns_ 里摘除（erase 会顺带释放 SSL）。
    void close_conn(int fd);

    // 改 epoll 关注的事件。握手期间要在 EPOLLIN / EPOLLIN|EPOLLOUT 之间来回切，
    // 漏切一次就是「连接挂到 10 秒空闲回收才断」或者「EPOLLOUT 水平触发空转」。
    bool set_interest(int fd, std::uint32_t events);

    // 回收长时间不活动、请求还没收全的连接（slowloris 防护）。
    void sweep_idle();

    // 工作线程：把处理完的 fd 交还事件循环（含唤醒 epoll）。
    void recycle_conn(int fd, std::unique_ptr<tls::Session> ssl, const std::string& remote_ip);

    // 事件循环：把交还的连接重新注册回 epoll。只能由事件循环线程调用。
    void drain_recycled();

    struct Conn {
        std::string read_buf;
        std::string remote_ip;  // accept 时记录，随任务传给工作线程做来源校验
        std::unique_ptr<tls::Session> ssl;
        bool handshaking = false;

        // 最后一次收到数据的时刻。HTTP 请求就几十字节，正常情况下 10 秒内必然收完；
        // 一直收不全的连接只会白占内存和 fd，必须主动回收。
        // 握手阶段同样靠它兜底：没有单独的握手超时，因为 slowloris 在握手期
        // 和请求期是同一个问题（发一半就不动了），没必要写两套。
        std::chrono::steady_clock::time_point last_active;
    };

    // 推进一次握手。返回 false 表示连接已关闭，调用方必须立刻返回。
    bool drive_handshake(int fd, Conn& c);

    int listen_fd_ = -1;
    int epoll_fd_  = -1;

    // 预留给「fd 耗尽」时用的空闲 fd。EMFILE 时 accept 本身就会失败，
    // 连「丢弃一个待处理连接」都做不到；必须先关掉它腾出一个 fd 才能破局。
    int reserve_fd_ = -1;

    // 用于唤醒 epoll 的 eventfd。keep-alive 连接被交还后必须立刻唤醒事件循环，
    // 否则要等 epoll_wait 的 100ms 超时才会重新注册——复用连接省下的时间
    // 会被这 100ms 全部吃掉（甚至更慢）。
    int wake_fd_ = -1;

    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::uint16_t port_ = 0;
    Handler handler_;

    // TLS 上下文（明文模式下为空）。必须比所有 Session 活得久：
    // Stop() 里先等事件循环与工作线程全部退出（那时所有 SSL 已释放），
    // 再由本对象析构释放 SSL_CTX。顺序反过来就是退出路径上的 use-after-free。
    std::unique_ptr<tls::Context> tls_ctx_;
    bool tls_enabled_ = false;

    // 事件循环线程专用：缓冲半包请求。
    std::unordered_map<int, Conn> conns_;

    // 工作线程池：完整请求队列 + 消费线程。
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<Task> queue_;
    bool shutdown_ = false;
    std::vector<std::thread> workers_;

    // 工作线程 -> 事件循环 的单向交接队列。queue_mtx_ 同时保护它：
    // 两者都是「生产者/消费者」队列，没有嵌套持锁的路径，共用一个锁不会引入锁序问题。
    std::deque<Recycled> recycled_;

    // 指标。写侧分散在事件循环线程与工作线程，读侧是任意调用方，
    // 因此一律用原子量而不是「读 conns_.size()」——conns_ 只有事件循环线程能碰。
    std::atomic<std::uint64_t> requests_total_{0};
    std::atomic<std::uint64_t> requests_dropped_{0};
    std::atomic<std::uint64_t> responses_2xx_{0};
    std::atomic<std::uint64_t> responses_4xx_{0};
    std::atomic<std::uint64_t> responses_5xx_{0};
    std::atomic<std::size_t> active_conns_{0};
    std::atomic<std::uint64_t> handshake_failures_{0};
};

}  // namespace dcache