#pragma once

#include <libdariadb/interfaces/icallbacks.h>
#include <libdariadb/meas.h>
#include <libdariadb/st_exports.h>
#include <libdariadb/utils/async/locker.h>
#include <condition_variable>
#include <memory>

namespace dariadb {
namespace storage {

struct MArray_ReaderClb : public IReadCallback {
  EXPORT MArray_ReaderClb(size_t count);
  EXPORT void apply(const Meas &m) override;

  MeasArray marray;
  utils::async::Locker _locker;
};

struct MArrayPtr_ReaderClb : public IReadCallback {
  EXPORT MArrayPtr_ReaderClb(MeasArray *output);
  EXPORT void apply(const Meas &m) override;

  MeasArray *marray;
  utils::async::Locker _locker;
};

struct FunctorCallback : public IReadCallback {
  /// return true to cancel;
  typedef std::function<bool(const Meas &)> FunctorType;
  EXPORT FunctorCallback(FunctorType &ft);
  EXPORT void apply(const Meas &m) override;
  FunctorType functor;
};
}
}
