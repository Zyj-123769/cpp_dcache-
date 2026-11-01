#pragma once

#include <map>
#include <string>

namespace dcache {

// HTTP 请求（极简，本项目只需 GET + path）。
struct HttpRequest {
    std::string                         method;
    std::string                         path;
    std::string                         body;
    std::map<std::string, std::string> headers;

    // 对端 IPv4 字面量，由 HttpServer 在 accept 时填入。
    // 空串表示来源未知，一律按「不可信」处理（见 Node::ServeHTTP 的跳数校验）。
    std::string                         remote_ip;
};

// HTTP 响应。
struct HttpResponse {
    int                                 status = 200;
    std::string                         body;
    std::map<std::string, std::string> headers;
};

// HTTP 状态码 -> 原因短语（只覆盖本项目会用到的几个）。
inline const char* HttpStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

} // namespace dcache