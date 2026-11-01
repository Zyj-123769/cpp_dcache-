#pragma once

#include <string>

namespace dcache {

// 极简 JSON 工具，仅服务本项目「节点间 HTTP 报文」这一种场景：
// 响应固定形如 {"value":"...","found":true}，不追求完整 JSON 解析器。

// 转义 / 反转义 JSON 字符串字面量。
std::string JsonEscape(const std::string& s);
std::string JsonUnescape(const std::string& s);

// 序列化一次读取结果。
std::string EncodeResult(const std::string& value, bool found);

// 反序列化读取结果；解析失败返回 false。
bool DecodeResult(const std::string& body, std::string& value, bool& found);

// 从一个 JSON 对象中提取字符串 / 布尔字段。
bool JsonFindString(const std::string& body, const std::string& key, std::string& out);
bool JsonFindBool(const std::string& body, const std::string& key, bool& out);

}  // namespace dcache