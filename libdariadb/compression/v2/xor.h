#pragma once

#include "base_compressor.h"

namespace dariadb {
namespace compression {
namespace v2 {
namespace inner {

inline int64_t flat_double_to_int(dariadb::Value v) {
  static_assert(sizeof(dariadb::Value) == sizeof(int64_t), "size not equal");
  auto result = reinterpret_cast<int64_t *>(&v);
  return *result;
}
inline dariadb::Value flat_int_to_double(int64_t i) {
  auto result = reinterpret_cast<dariadb::Value *>(&i);
  return *result;
}
}

struct XorCompressor : public BaseCompressor {
public:
  XorCompressor(const ByteBuffer_Ptr &bw_);

  bool append(Value v);

  bool _is_first;
  uint64_t _first;
  uint64_t _prev_value;
  uint8_t _prev_lead;
  uint8_t _prev_tail;
};

struct XorDeCompressor : public BaseCompressor {
public:
  XorDeCompressor(const ByteBuffer_Ptr &bw, Value first);

  Value read();

  uint64_t _prev_value;
  uint8_t _prev_lead;
  uint8_t _prev_tail;
};
}
}
}
