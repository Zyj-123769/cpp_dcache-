#pragma once

#include <string>

namespace dcache {

// 对 URL 路径段做百分号编码：保留 RFC 3986 非保留字符（A-Z a-z 0-9 - _ . ~），其余转义。
std::string UrlEncodeComponent(const std::string& s);

// 对 URL 路径段做百分号解码（%XX）；非法序列原样保留。
std::string UrlDecodeComponent(const std::string& s);

} // namespace dcache