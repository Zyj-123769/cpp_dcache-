#include "json.hpp"

#include <cstdio>

namespace dcache {

namespace {

void append_hex_escape(std::string& out, unsigned char c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "\\u%04x", c);
    out += buf;
}

// 定位 "key" 字段，并让 pos 指向其值的起始位置（跳过冒号与空白）。
//
// 关键约束：**必须跳过字符串字面量内部**。
// 早期版本用 `body.find(needle)` 直接搜，命中的可能是值内部的一段文本：
// 缓存值本身是 `a"found` 时，编码出来是 "a\"found"，其中就含 `"found"` 这个片段。
// 于是 JsonFindBool 以为找到了 found 字段，读到的却是值里的内容 ——
// 轻则「自己编码的报文自己读不回来」（返回 false 直接判畸形），
// 重则悄悄把「命中」读成「未命中」（值的构造足够巧时，后面真能跟一个 :false）。
// 这条是 fuzz 上线 30 秒撞出来的，人工用例和四批 review 都没看见。
bool find_after_key(const std::string& body, const std::string& key, std::size_t& p) {
    const std::string needle = "\"" + key + "\"";
    bool in_string           = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // 跳过被转义的下一个字符：\" 不是字符串结束
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c != '"') {
            continue;
        }
        // 顶层的引号：要么是目标字段名，要么是一段与本次查找无关的字符串。
        if (body.compare(i, needle.size(), needle) == 0) {
            std::size_t q = i + needle.size();
            while (q < body.size() && (body[q] == ' ' || body[q] == '\t')) {
                ++q;
            }
            if (q < body.size() && body[q] == ':') {
                ++q;
                while (q < body.size() && (body[q] == ' ' || body[q] == '\t')) {
                    ++q;
                }
                p = q;
                return true;
            }
        }
        in_string = true;
    }
    return false;
}

// U+D800..U+DFFF 是 UTF-16 的代理区，本身不是字符。JSON 用它拼出 BMP 外的码点，
// 单独出现时按 JSON 生态的通行做法（WHATWG、多数语言的标准库）替换成 U+FFFD。
constexpr unsigned int kReplacementChar = 0xFFFD;

int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// 读 s[pos..pos+3] 这 4 个十六进制位。越界或含非十六进制字符时返回 false，
// 且不碰 out —— 调用方据此决定是「原样保留」还是「当孤立代理替换」。
//
// 边界写死在这里而不是让调用方各自 +4：代理对要连续做两次这种读取，
// 而第二次的起点是算出来的，一处算错就是越界读（ASan 只有在恰好撞到
// 页尾时才报）。
bool parse_hex4(const std::string& s, std::size_t pos, unsigned int& out) {
    if (pos + 3 >= s.size()) {  // 需要 pos .. pos+3 全部可读
        return false;
    }
    unsigned int v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        const int d = hex_digit(s[pos + k]);
        if (d < 0) {
            return false;
        }
        v = v * 16 + static_cast<unsigned int>(d);
    }
    out = v;
    return true;
}

// 把 Unicode 码点编码为 UTF-8（保留 4 字节路径，兼容外部报文的高位码点）。
void append_utf8(std::string& out, unsigned int code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

}  // namespace

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                append_hex_escape(out, c);
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string JsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        char n = s[++i];
        switch (n) {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            // \uXXXX 转义的是一个 UTF-16 **码元**，不是码点。这两者只对 BMP 内的
            // 字符等价；BMP 外的字符（emoji、部分生僻字）在报文里是一对代理：
            // 😀 = U+1F600 = \uD83D\uDE00。
            unsigned int code = 0;
            if (!parse_hex4(s, i + 1, code)) {
                // 不是合法的 \uXXXX（太短或有非十六进制字符）：原样保留两个字符。
                // 不在这里判畸形，是为了让「畸形报文」的责任留在上层的字段解析，
                // 这个函数只负责转义。
                out += '\\';
                out += 'u';
                break;
            }
            i += 4;  // 指向 4 个十六进制位中的最后一位，外层 ++i 正好跨过 \uXXXX

            // 关键：0xD83D 必须和后面的 0xDE00 合并成 0x1F600，再走四字节分支。
            // 旧实现把 0xD83D 当普通码点编，走的是「三字节」分支，产出 ED A0 BD ——
            // 这是把 UTF-16 码元当码点编码的经典错误（CESU-8），而 RFC 3629
            // 禁止把 U+D800..U+DFFF 编进 UTF-8 字节流。对端拿到的是非法字节，
            // 下游 JSON 解析器通常直接判报文畸形。
            if (code >= 0xD800 && code <= 0xDBFF) {
                // 高代理：向后偷看一个 \uXXXX，是低代理就合并，否则自己作废。
                unsigned int low = 0;
                if (i + 2 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u' &&
                    parse_hex4(s, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF) {
                    code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                    i += 6;  // 第二个 \uXXXX 也吃掉了
                } else {
                    code = kReplacementChar;
                }
            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                code = kReplacementChar;  // 低代理单独出现：没有高代理可与它配对
            }
            append_utf8(out, code);
            break;
        }
        default: {
            out += '\\';
            out += n;
            break;
        }
        }
    }
    return out;
}

std::string EncodeResult(const std::string& value, bool found) {
    std::string s = "{\"value\":\"";
    s += JsonEscape(value);
    s += "\",\"found\":";
    s += found ? "true" : "false";
    s += "}";
    return s;
}

bool DecodeResult(const std::string& body, std::string& value, bool& found) {
    return JsonFindString(body, "value", value) && JsonFindBool(body, "found", found);
}

bool JsonFindString(const std::string& body, const std::string& key, std::string& out) {
    std::size_t p = 0;
    if (!find_after_key(body, key, p)) {
        return false;
    }
    if (p >= body.size() || body[p] != '"') {
        return false;
    }
    ++p;

    std::string raw;
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) {
            raw += body[p];
            raw += body[p + 1];
            p += 2;
        } else {
            raw += body[p];
            ++p;
        }
    }
    if (p >= body.size()) {
        return false;  // 缺少闭合引号
    }
    out = JsonUnescape(raw);
    return true;
}

bool JsonFindBool(const std::string& body, const std::string& key, bool& out) {
    std::size_t p = 0;
    if (!find_after_key(body, key, p)) {
        return false;
    }
    if (body.compare(p, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (body.compare(p, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

}  // namespace dcache