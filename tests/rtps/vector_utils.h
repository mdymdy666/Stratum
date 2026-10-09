// 向量文件可用性检查（供测试显式断言）
// 单独成文件以便 test_spdp_parser 与 test_submessage_parser 共用。

#ifndef STRATUM_TESTS_RTPS_VECTOR_UTILS_H_
#define STRATUM_TESTS_RTPS_VECTOR_UTILS_H_

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace stratum {
namespace testing {

// 读取抓包向量文件（格式：[u32 count][u32 len][bytes]...）
// 失败时返回 false —— 调用方**必须**据此判定测试失败，
// 不得静默跳过（规范：禁止通过跳过测试来"通过"）。
inline bool LoadCaptureVectors(const std::string& path,
                               std::vector<std::vector<uint8_t>>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  uint32_t count = 0;
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  if (!in || count == 0 || count > 100000) return false;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in || len == 0 || len > 65535) return false;
    std::vector<uint8_t> buf(len);
    in.read(reinterpret_cast<char*>(buf.data()), len);
    if (!in) return false;
    out->push_back(std::move(buf));
  }
  return !out->empty();
}

// 抓包向量文件的约定路径（相对 build 目录运行）
inline const char* CaptureVectorPath() {
  return "tests/vectors/spdp_capture.bin";
}

}  // namespace testing
}  // namespace stratum

#endif  // STRATUM_TESTS_RTPS_VECTOR_UTILS_H_
