#pragma once

#include <memory>

#include <libdariadb/meas.h>
#include <libdariadb/compression/v2/bytebuffer.h>
#include <libdariadb/compression/v2/delta.h>
#include <libdariadb/compression/v2/flag.h>
#include <libdariadb/compression/v2/xor.h>
#include <libdariadb/dariadb_st_exports.h>

namespace dariadb {
namespace compression {
namespace v2 {
class CopmressedWriter {
public:
  DARIADB_ST_EXPORTS CopmressedWriter(const ByteBuffer_Ptr &bw_time);
  DARIADB_ST_EXPORTS ~CopmressedWriter();

  DARIADB_ST_EXPORTS bool append(const Meas &m);
  bool is_full() const { return _is_full; }

  size_t used_space() const { return time_comp.used_space(); }

  ByteBuffer_Ptr get_bb()const { return _bb; }
protected:
	ByteBuffer_Ptr _bb;
	Meas _first;
	bool _is_first;
	bool _is_full;
	DeltaCompressor time_comp;
	XorCompressor value_comp;
	FlagCompressor flag_comp;
};

class CopmressedReader {
public:
	CopmressedReader() = default;
  DARIADB_ST_EXPORTS CopmressedReader(const ByteBuffer_Ptr &bw_time, const Meas &first);
  DARIADB_ST_EXPORTS ~CopmressedReader();


  dariadb::Meas read() {
	  Meas result{};
	  result.id = _first.id;
	  result.time = time_dcomp.read();
	  result.value = value_dcomp.read();
	  result.flag = flag_dcomp.read();
	  return result;
  }


protected:
	dariadb::Meas _first;
	DeltaDeCompressor time_dcomp;
	XorDeCompressor value_dcomp;
	FlagDeCompressor flag_dcomp;
};
}
}
}
