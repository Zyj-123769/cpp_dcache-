#pragma once

#include <cstddef>
#include <memory>
#include <string>

// 节点间 TLS 的最小封装。
//
// 为什么单开一层：OpenSSL 的类型（SSL_CTX / SSL）如果出现在 http_server.hpp 或
// http_client.hpp 里，`-DDCACHE_TLS=OFF` 的构建就得在每个用到它们的文件里写 #ifdef。
// 条件编译会顺着热路径蔓延——先是读写分支，再是连接结构体，最后整个事件循环被
// 两份代码塞满。这里只暴露 pimpl 句柄和两个结果枚举，没编进 OpenSSL 时由
// src/tls_stub.cpp 把「不可用」如实报出来，调用方只有一条分支要写。
//
// 分工（刻意划清，别混）：
//   - TLS 负责 **机密性 + 对端身份**（certificate），解决「同网段能嗅到 token」；
//   - X-DCache-Token 负责 **应用层授权**（哪个应用有资格发内部语义头）。
// 两者不互相替代：token 明文时代 TLS 没上，等于把口令写在信封外面。

namespace dcache {
namespace tls {

// 这个二进制里 TLS 是否可用（DCACHE_TLS=OFF 时为 false）。
bool Available();

// 编译进二进制的 TLS 实现名（"openssl 3.x" 或 "none"），用于启动日志与 /metrics。
std::string Backend();

struct ServerConfig {
    std::string cert_file;  // PEM，可含中间证书链
    std::string key_file;   // PEM 私钥
};

struct ClientConfig {
    // 必须给：不校验证书的 TLS 只是把明文换了个包装，攻击者照样能中间人。
    // 想「先跑通再说」的捷径（SSL_VERIFY_NONE）不提供——它会让后面所有
    // 「已启用 TLS」的结论都失去意义。
    std::string ca_file;
};

// 握手推进结果。kWantRead / kWantWrite 等价于 EAGAIN：
// 调用方必须等 socket 可读/可写后**原样重试**，绝不能当成失败——
// 把它当失败的话，握手会在第一个分片上就断，看起来像「对端不支持 TLS」。
enum class Step { kOk, kWantRead, kWantWrite, kFailed };

// 一次读的结果。kClosed 与 kFailed 的区别只影响日志，不影响调用方：
// 两种情况都意味着这条连接不再有数据可读。
enum class Io { kOk, kClosed, kWantRead, kWantWrite, kFailed };

class Session;

// SSL_CTX 的所有者。生命周期必须覆盖所有由它创建的 Session：
// HttpServer 把它作为成员，析构顺序上先 Stop()（等所有 worker 退出、会话释放）
// 再销毁它。反过来会得到 use-after-free，而且只在退出路径上才炸。
class Context {
public:
    static std::unique_ptr<Context> CreateServer(const ServerConfig& cfg, std::string& err);
    static std::unique_ptr<Context> CreateClient(const ClientConfig& cfg, std::string& err);

    ~Context();
    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    void* native() const { return ctx_; }

private:
    explicit Context(void* ctx) : ctx_(ctx) {}
    void* ctx_ = nullptr;
};

// 一条连接上的 TLS 状态，独占一个 SSL*。
//
// 非线程安全：同一时刻只能有一个线程用它。本项目的交接规则是「同一时刻只有
// 事件循环线程或工作线程之一持有它」，靠 fd 的所有权转移保证，与 fd 同进同退。
class Session {
public:
    // 服务端：包装一条刚 accept 出来的 fd，随后必须调 Handshake()。
    static std::unique_ptr<Session> Accept(Context& ctx, int fd);
    // 客户端：包装一条刚 connect 成功的 fd。
    // host 用于 SNI 与证书校验（IP 字面量与域名走不同分支，见实现里的注释）。
    static std::unique_ptr<Session> Connect(Context& ctx, int fd, const std::string& host);

    ~Session();
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    Step Handshake();

    // 读：返回 >0 为字节数；==0 见 io（kClosed）；<0 见 io（kWantRead/kWantWrite/kFailed）。
    long Read(void* buf, std::size_t len, Io& io);

    // 非阻塞写：返回 >0 为本次写出的字节数；<0 时看 io——
    // kWantWrite/kWantRead 表示「现在写不动，等 socket 可写/可读后原样重试」，
    // kClosed/kFailed 表示这条会话已经不可恢复。
    // 与 WriteAll 的唯一区别是「不替你等」，调用方自己注册 EPOLLOUT。
    long Write(const void* data, std::size_t len, Io& io);

    // 阻塞语义的整段写：调用方负责把 fd 设为阻塞并设好 SO_SNDTIMEO。
    // 超时或对端断开一律返回 false —— 此时 TLS 记录可能只写了一半，
    // 这条会话已经不可恢复，调用方必须关闭连接而不是重试。
    bool WriteAll(const void* data, std::size_t len);

    std::string LastError() const;

    void* native() const { return ssl_; }
    bool server_side() const { return server_side_; }

private:
    Session(void* ssl, bool server_side) : ssl_(ssl), server_side_(server_side) {}
    void* ssl_        = nullptr;
    bool server_side_ = false;
};

}  // namespace tls
}  // namespace dcache
