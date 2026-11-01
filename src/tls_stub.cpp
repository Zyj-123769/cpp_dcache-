#include "tls.hpp"

// -DDCACHE_TLS=OFF 时的桩实现。
//
// 存在的意义不是「留个后路」，而是让 CMake 不必在热路径文件里撒 #ifdef：
// http_server.cpp / http_client.cpp 永远只有一份代码，TLS 关掉时启动阶段就会
// 拿到明确的错误文案，而不是链接失败或者悄悄以明文运行。
//
// 「悄悄以明文运行」是这里最想避免的结果：如果 TLS 配置被静默忽略，
// 运维会以为集群已经加密，而实际上 token 还在网上裸奔。

namespace dcache {
namespace tls {

namespace {
const char* const kUnavailable = "本二进制未编译 TLS（DCACHE_TLS=OFF）";
}  // namespace

bool Available() {
    return false;
}

std::string Backend() {
    return "none";
}

Context::~Context() {}

// 参数必须写名字：readability-named-parameter 会拦无名参数，
// 而 -Wunused-parameter 又不许名字写了却不用，所以统一 (void) 掉。
std::unique_ptr<Context> Context::CreateServer(const ServerConfig& cfg, std::string& err) {
    (void)cfg;
    err = kUnavailable;
    return nullptr;
}

std::unique_ptr<Context> Context::CreateClient(const ClientConfig& cfg, std::string& err) {
    (void)cfg;
    err = kUnavailable;
    return nullptr;
}

Session::~Session() {}

std::unique_ptr<Session> Session::Accept(Context& ctx, int fd) {
    (void)ctx;
    (void)fd;
    return nullptr;
}

std::unique_ptr<Session> Session::Connect(Context& ctx, int fd, const std::string& host) {
    (void)ctx;
    (void)fd;
    (void)host;
    return nullptr;
}

Step Session::Handshake() {
    return Step::kFailed;
}

long Session::Read(void* buf, std::size_t len, Io& io) {
    (void)buf;
    (void)len;
    io = Io::kFailed;
    return -1;
}

bool Session::WriteAll(const void* data, std::size_t len) {
    (void)data;
    (void)len;
    return false;
}

std::string Session::LastError() const {
    return kUnavailable;
}

}  // namespace tls
}  // namespace dcache
