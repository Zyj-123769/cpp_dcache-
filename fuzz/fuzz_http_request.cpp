// libFuzzer 目标：HTTP 请求解析器。
//
// 这是全项目唯一直接吃不可信外部字节的组件，也是最值得 fuzz 的一个。
// 三条不变式分别对应三类真实 bug：
//   1. 只有成功才写 consumed —— 否则调用方会把「吃掉的 0 字节」当成已消费，
//      把缓冲区里真正的请求丢给下一条解析（丢请求）；
//   2. 幂等 —— 同一段字节解析两次结论必须一致，状态的没清干净会在这里暴露；
//   3. 前缀性质 —— 请求收到一半时只能回答「还要更多」，不能提前说成功。
//      半包是 TCP 的常态，破坏了它，服务端会在半包到达时直接丢请求。

#include <cstddef>
#include <cstdint>
#include <string>

#include "check.hpp"
#include "http_parser.hpp"

namespace {

// 前缀检查是 O(n²)：每切一刀都要从头解析一次。
// 64KB 的报文切 65536 刀就是 40 亿次字符比较，fuzzer 会被拖到每秒几 exec。
// 只在短样本上跑，长样本交给另外两条线性检查。
constexpr std::size_t kPrefixCheckLimit = 512;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    dcache::HttpRequest req;
    std::size_t consumed = 0;
    const int rc         = dcache::ParseRequest(input, req, consumed);

    if (rc != dcache::kParseOk) {
        fuzz_check::require(consumed == 0, "解析失败却报告了 consumed", input);
        return 0;
    }
    fuzz_check::require(consumed > 0 && consumed <= input.size(), "consumed 超出输入范围", input);

    // 幂等：把前 consumed 字节原样重新解析，结论必须完全一致。
    {
        dcache::HttpRequest req2;
        std::size_t consumed2 = 0;
        const int rc2         = dcache::ParseRequest(input.substr(0, consumed), req2, consumed2);
        fuzz_check::require(rc2 == dcache::kParseOk, "同一报文二次解析失败", input);
        fuzz_check::require(consumed2 == consumed, "同一报文二次解析 consumed 不一致", input);
        fuzz_check::require(req2.method == req.method && req2.path == req.path,
                            "同一报文二次解析字段不一致", input);
        fuzz_check::require(req2.headers == req.headers, "同一报文二次解析头部不一致", input);
    }

    // 前缀性质：任何「缩短了但没缩到零」的版本都必须回答 kParseIncomplete。
    if (consumed <= kPrefixCheckLimit) {
        for (std::size_t cut = 0; cut < consumed; ++cut) {
            dcache::HttpRequest partial;
            std::size_t partial_consumed = 0;
            const int rc_prefix =
                dcache::ParseRequest(input.substr(0, cut), partial, partial_consumed);
            fuzz_check::require(rc_prefix == dcache::kParseIncomplete, "半包被判定为终态", input);
        }
    }
    return 0;
}
