#include "http_parser.hpp"

#include <cstdlib>

namespace dcache {

// 说明：这里刻意不做完整的 HTTP/1.1 合规校验（不支持 chunked、不校验 header 名字符合
// token 规则、不拒绝重复头）。目标只有一个——**把不可信字节安全地拆成字段**：
// 长度全部由上界约束，任何畸形输入都只能返回错误码，不能越界、不能崩溃、不能无限增长。
int ParseRequest(const std::string& data, HttpRequest& req, std::size_t& consumed) {
    consumed               = 0;
    const std::size_t hpos = data.find("\r\n\r\n");
    if (hpos == std::string::npos) {
        // 头部都没收全就已经超限：再等下去也只是把内存耗在同一个连接上。
        return data.size() > kMaxRequestSize ? kParseMalformed : kParseIncomplete;
    }

    const std::string header_block = data.substr(0, hpos);
    const std::size_t line_end     = header_block.find("\r\n");
    const std::string reqline      = header_block.substr(0, line_end);

    // 请求行："GET /api/g/k HTTP/1.1"
    std::size_t sp1 = reqline.find(' ');
    std::size_t sp2 = reqline.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        return kParseMalformed;
    }
    req.method = reqline.substr(0, sp1);
    req.path   = reqline.substr(sp1 + 1, sp2 - sp1 - 1);

    // 头部行（可无）。
    req.headers.clear();
    std::size_t pos = line_end + 2;
    while (pos < hpos) {
        std::size_t line_end2 = header_block.find("\r\n", pos);
        if (line_end2 == std::string::npos)
            line_end2 = hpos;
        const std::string line = header_block.substr(pos, line_end2 - pos);
        if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                if (!v.empty() && v[0] == ' ')
                    v.erase(0, 1);
                req.headers[k] = v;
            }
        }
        pos = line_end2 + 2;
    }

    // 请求体（Content-Length）。本项目只有 GET，body 通常为空。
    long content_length = 0;
    auto it             = req.headers.find("Content-Length");
    if (it == req.headers.end())
        it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        // strtol 溢出时给 LONG_MAX，转 size_t 后必然 > kMaxRequestSize，
        // 于是在下面被判为 kParseBodyTooLarge——不需要额外处理溢出。
        content_length = std::strtol(it->second.c_str(), nullptr, 10);
        if (content_length < 0) {
            return kParseMalformed;  // 负数 Content-Length
        }
    }

    // 声明体超过上限时立刻判失败：读缓冲本来就在 kMaxRequestSize 处封顶，
    // 这种请求永远等不到「收全」，只会把这个连接永久挂在 conns_ 里。
    if (static_cast<std::size_t>(content_length) > kMaxRequestSize) {
        return kParseBodyTooLarge;
    }

    const std::size_t total = hpos + 4 + static_cast<std::size_t>(content_length);
    if (data.size() < total) {
        return kParseIncomplete;  // body 未收完
    }
    req.body = data.substr(hpos + 4, static_cast<std::size_t>(content_length));
    consumed = total;
    return kParseOk;
}

}  // namespace dcache
