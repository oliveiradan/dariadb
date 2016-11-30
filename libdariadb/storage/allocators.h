#pragma once

#include <libdariadb/st_exports.h>
#include <libdariadb/utils/locker.h>
#include <libdariadb/storage/chunk.h>
#include <libdariadb/utils/utils.h>
#include <memory>

namespace dariadb {
namespace storage {

struct MemChunkAllocator:public utils::NonCopy {
	struct AllocatedData {
		ChunkHeader *header;
		uint8_t *buffer;
		size_t position;
		AllocatedData(ChunkHeader *h, uint8_t *buf, size_t pos) {
			header = h;
			buffer=buf;
			position = pos;
		}
		AllocatedData() {
			header = nullptr;
			buffer = nullptr;
			position = std::numeric_limits<size_t>::max();
		}
	};
  
  const AllocatedData EMPTY = AllocatedData(nullptr, nullptr, std::numeric_limits<size_t>::max());

  size_t _maxSize;        /// max size in bytes)
  size_t _chunkSize;      /// size of chunk
  size_t _capacity;       /// max size in chunks
  size_t _allocated;      /// already allocated count of chunks.

  ChunkHeader *_headers;
  uint8_t *_buffers;
  uint8_t *_region;

  std::list<size_t> _free_list;
  utils::Locker _locker;

  EXPORT MemChunkAllocator(size_t maxSize, size_t bufferSize);
  MemChunkAllocator(const MemChunkAllocator&)=delete;
  EXPORT ~MemChunkAllocator();
  EXPORT AllocatedData allocate();
  EXPORT void free(const AllocatedData &d);
};
}
}
