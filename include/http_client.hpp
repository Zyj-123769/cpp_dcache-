#pragma once

#include "tls.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace dcache {

// 极简同步 HTTP GET 客户端（POSIX socket），供节点间转发使用。
//
// 连接复用：默认对 host:port 维持一组空闲连接（HTTP/1.1 keep-alive）。
// 节点间转发原本每次都要 getaddrinfo + TCP 握手 + close，转发比例高时
// 握手开销与 TIME_WAIT 堆积都不划算。
class HttpClient {
public:
    // 向 host:port 发起 GET path；成功返回 true 且 body 写入 out。
    // 单次调用自带超时（默认 2000ms），超时/连接失败返回 false。
    // extra_headers 以 "Name: value" 追加到请求头（如 X-DCache-Hops）。
    //
    // http_status（可选出参）：拿到的 HTTP 状态码；0 表示根本没读到状态行
    // （连接失败/超时/报文损坏）。调用方据此区分「对端明确报错」与「够不着对端」，
    // 两者都不等于「数据不存在」。
    static bool Get(const std::string& host, std::uint16_t port, const std::string& path,
                    std::string& out, int timeout_ms = 2000,
                    const std::map<std::string, std::string>& extra_headers = {},
                    int* http_status                                        = nullptr);

    // 开关连接复用。关闭后退回「每请求一次 TCP」，行为与旧实现一致（排查问题时用）。
    static void SetKeepAlive(bool on);

    // 打开节点间的 TLS（给定 CA 文件，用于校验对端证书）。
    //
    // 只在**出站**方向生效；入站由 HttpServer::Start 的 tls 参数控制。两侧都开才算
    // 真的加密——只开一侧连不上，这正是刻意的：一个「自动降级成明文」的开关
    // 会让「已启用 TLS」这个结论永远不成立。
    //
    // 必须在并发调用 Get 之前完成（和 SetKeepAlive 同一条约束）。
    // 切换时会清空空闲连接池：池里的连接属于另一种协议，留着就是对不上话的僵尸。
    static bool EnableTls(const tls::ClientConfig& cfg, std::string& err);
    static bool TlsEnabled();

    // 当前空闲连接数（用于 /metrics 观察池子是否漏连接）。
    static std::size_t IdleConnections();

    // 关闭并清空空闲连接池。进程退出前调用，避免静态析构顺序导致的 fd 泄漏。
    static void CloseIdleConnections();
};

}  // namespace dcache