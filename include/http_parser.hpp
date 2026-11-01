#pragma once

#include <cstddef>
#include <string>

#include "http.hpp"

namespace dcache {

// 单条请求的最大字节数（请求行 + 头部 + 请求体）。
// 这是解析器与服务端读循环的共同契约，改一处必须改另一处：
// HttpServer 在 kMaxRequestSize 处封顶读缓冲，解析器在同一数值上判「声明体过大」。
// 不封顶的后果很具体——「只发半个请求然后挂着」能廉价地把节点内存吃干净。
constexpr std::size_t kMaxRequestSize = 64 * 1024;

// ParseRequest 的返回码。
// 用命名常量而不是裸 -1/-2/0/1：调用点写成 `rc == kParseBodyTooLarge` 才读得懂，
// 而 `rc == -2` 得回头翻实现才知道是什么。
enum ParseStatus : int {
    kParseIncomplete   = 0,   // 数据不够，等更多字节
    kParseOk           = 1,   // 解析出一条完整请求
    kParseMalformed    = -1,  // 报文非法（请求行缺空格、负 Content-Length 等）
    kParseBodyTooLarge = -2,  // 声明的 Content-Length 超过 kMaxRequestSize
};

// 从 data 中解析一条 HTTP/1.1 请求（本服务只关心 method + path，其余原样留存）。
// 返回 ParseStatus。consumed 仅在 kParseOk 时有意义：本条请求占用的字节数，
// 调用方用它判断缓冲区里是否还有下一条请求（pipelining）。
//
// 为什么单独成一个模块：这是全项目唯一「直接吞不可信外部字节」的组件。
// 拆出来之后 fuzz（tests/fuzz_http_request.cpp）能直接喂任意畸形输入，
// 不必为了触发解析器去起一个真 socket。
int ParseRequest(const std::string& data, HttpRequest& req, std::size_t& consumed);

}  // namespace dcache
