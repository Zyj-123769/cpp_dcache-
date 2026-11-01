#pragma once

#include "http.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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
//   - 每个连接处理完一条请求即关闭（响应带 Connection: close），
//     不实现完整的 keep-alive 状态机。
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 绑定 addr:port 并启动后台线程；失败返回 false。
    // addr 支持 "0.0.0.0"（监听所有网卡）与 "127.0.0.1" 等 IPv4 字面量。
    // worker_count 为处理请求的工作线程数（默认 4）。
    bool Start(const std::string& addr, std::uint16_t port, Handler handler,
               int worker_count = 4);

    // 停止服务器（阻塞等待后台线程退出）。
    void Stop();

    // 实际监听的端口（传入 0 时返回系统分配的端口）。
    std::uint16_t Port() const { return port_; }

private:
    struct Task {
        int         fd;
        HttpRequest req;
    };

    void run();
    void handle_read(int fd);
    void handle_request(int fd, HttpRequest req);
    void worker_loop();
    void cleanup();

    // 关闭连接（幂等）：已移交给工作线程的 fd 不在此表内，不会被重复 close。
    void close_conn(int fd);

    // 回收长时间不活动、请求还没收全的连接（slowloris 防护）。
    void sweep_idle();

    struct Conn {
        std::string read_buf;
        std::string remote_ip;  // accept 时记录，随任务传给工作线程做来源校验

        // 最后一次收到数据的时刻。HTTP 请求就几十字节，正常情况下 10 秒内必然收完；
        // 一直收不全的连接只会白占内存和 fd，必须主动回收。
        std::chrono::steady_clock::time_point last_active;
    };

    int                            listen_fd_ = -1;
    int                            epoll_fd_  = -1;

    // 预留给「fd 耗尽」时用的空闲 fd。EMFILE 时 accept 本身就会失败，
    // 连「丢弃一个待处理连接」都做不到；必须先关掉它腾出一个 fd 才能破局。
    int                            reserve_fd_ = -1;

    std::atomic<bool>              stop_{false};
    std::thread                    thread_;
    std::uint16_t                  port_ = 0;
    Handler                        handler_;

    // 事件循环线程专用：缓冲半包请求。
    std::unordered_map<int, Conn>  conns_;

    // 工作线程池：完整请求队列 + 消费线程。
    std::mutex                     queue_mtx_;
    std::condition_variable        queue_cv_;
    std::deque<Task>               queue_;
    bool                           shutdown_ = false;
    std::vector<std::thread>       workers_;
};

} // namespace dcache