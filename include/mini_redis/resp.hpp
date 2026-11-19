#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mini_redis {

enum class RespType {
  kSimpleString,
  kError,
  kInteger,
  kBulkString,
  kArray,
  kNull
};

struct RespValue {
  RespType type = RespType::kNull;
  std::string bulk;              // 简单/批量/错误字符串或整数文本
  std::vector<RespValue> array;
};

class RespParser {
 public:
  // 追加新读到的字节
  void append(std::string_view data);

  // 解析一个完整值；数据不完整返回 nullopt，协议错误返回 type 为 kError 的值
  std::optional<RespValue> tryParseOne();

  // 同时返回消费掉的原始 RESP 字节
  std::optional<std::pair<RespValue, std::string>> tryParseOneWithRaw();

 private:
  bool parseLine(size_t& pos, std::string& out_line);
  bool parseInteger(size_t& pos, int64_t& out_value);
  bool parseBulkString(size_t& pos, RespValue& out);
  bool parseSimple(size_t& pos, RespType t, RespValue& out);
  bool parseArray(size_t& pos, RespValue& out);

 private:
  std::string buffer_;
};

std::string respSimpleString(std::string_view s);
std::string respError(std::string_view s);
std::string respBulk(std::string_view s);
std::string respNullBulk();
std::string respInteger(int64_t v);

}  // namespace mini_redis


