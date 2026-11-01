#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace dcache {

// 极简同步 HTTP GET 客户端（POSIX socket），供节点间转发使用。
class HttpClient {
public:
    // 向 host:port 发起 GET path；成功返回 true 且 body 写入 out。
    // 单次调用自带超时（默认 2000ms），超时/连接失败返回 false。
    // extra_headers 以 "Name: value" 追加到请求头（如 X-DCache-Hops）。
    //
    // http_status（可选出参）：拿到的 HTTP 状态码；0 表示根本没读到状态行
    // （连接失败/超时/报文损坏）。调用方据此区分「对端明确报错」与「够不着对端」，
    // 两者都不等于「数据不存在」。
    static bool Get(const std::string& host, std::uint16_t port,
                    const std::string& path, std::string& out,
                    int timeout_ms = 2000,
                    const std::map<std::string, std::string>& extra_headers = {},
                    int* http_status = nullptr);
};

} // namespace dcache