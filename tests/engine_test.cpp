#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main
#include "test_common.h"
#include <boost/test/unit_test.hpp>

#include <libdariadb/dariadb.h>
#include <libdariadb/storage/manifest.h>
#include <libdariadb/timeutil.h>
#include <libdariadb/utils/fs.h>
#include <algorithm>
#include <iostream>

class BenchCallback : public dariadb::IReadCallback {
public:
  BenchCallback() { count = 0; }
  void call(const dariadb::Meas &) { count++; }
  size_t count;
};

BOOST_AUTO_TEST_CASE(Engine_common_test) {
  const std::string storage_path = "testStorage";
  const size_t chunk_size = 256;

  const dariadb::Time from = 0;
  const dariadb::Time to = from + 1000;
  const dariadb::Time step = 10;
  using namespace dariadb;
  using namespace dariadb::storage;
  {
    std::cout << "Engine_common_test.\n";
    if (dariadb::utils::fs::path_exists(storage_path)) {
      dariadb::utils::fs::rm(storage_path);
    }

    auto settings = dariadb::storage::Settings::create(storage_path);
    settings->wal_cache_size.setValue(100);
    settings->wal_file_size.setValue(settings->wal_cache_size.value() * 5);
    settings->chunk_size.setValue(chunk_size);
    std::unique_ptr<Engine> ms{new Engine(settings)};

    dariadb_test::storage_test_check(ms.get(), from, to, step, true, true, false);

    auto pages_count = ms->description().pages_count;
    BOOST_CHECK_GE(pages_count, size_t(2));
    BOOST_CHECK(ms->settings() != nullptr);
    BOOST_CHECK(ms->settings()->storage_path.value() == storage_path);
  }
  {
    std::cout << "reopen closed storage\n";
    
	auto ms = dariadb::open_storage(storage_path);
	auto settings = ms->settings();
    
    auto index_files = dariadb::utils::fs::ls(settings->raw_path.value(), ".pagei");
    BOOST_CHECK(!index_files.empty());
    for (auto &f : index_files) {
      dariadb::utils::fs::rm(f);
    }
    index_files = dariadb::utils::fs::ls(settings->raw_path.value(), ".pagei");
    BOOST_CHECK(index_files.empty());

    ms->fsck();

    ms->wait_all_asyncs();

    // check first id, because that Id placed in compressed pages.
    auto values = ms->readInterval(QueryInterval({dariadb::Id(0)}, 0, from, to));
    BOOST_CHECK_EQUAL(values.size(), dariadb_test::copies_count);

    auto current = ms->currentValue(dariadb::IdArray{}, 0);
    BOOST_CHECK(current.size() != size_t(0));

    std::cout << "erase old files" << std::endl;
    ms->settings()->max_store_period.setValue(1);
    while (true) {
      index_files = dariadb::utils::fs::ls(settings->raw_path.value(), ".pagei");
      if (index_files.empty()) {
        break;
      }
      std::cout << "file left:" << std::endl;
      for (auto i : index_files) {
        std::cout << i << std::endl;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  std::cout << "end\n";
  if (dariadb::utils::fs::path_exists(storage_path)) {
    dariadb::utils::fs::rm(storage_path);
  }
}

BOOST_AUTO_TEST_CASE(Engine_compress_all_test) {
  const std::string storage_path = "testStorage";
  const size_t chunk_size = 256;

  const dariadb::Time from = 0;
  const dariadb::Time to = from + 50;
  const dariadb::Time step = 10;

  using namespace dariadb;
  using namespace dariadb::storage;

  {
    std::cout << "Engine_compress_all_test\n";
    if (dariadb::utils::fs::path_exists(storage_path)) {
      dariadb::utils::fs::rm(storage_path);
    }

    auto settings = dariadb::storage::Settings::create(storage_path);
    settings->wal_cache_size.setValue(100);
    settings->wal_file_size.setValue(settings->wal_cache_size.value() * 2);
    settings->chunk_size.setValue(chunk_size);
    settings->strategy.setValue(dariadb::STRATEGY::WAL);
    std::unique_ptr<Engine> ms{new Engine(settings)};

    dariadb::IdSet all_ids;
    dariadb::Time maxWritedTime;
    dariadb_test::fill_storage_for_test(ms.get(), from, to, step, &all_ids,
                                        &maxWritedTime, false);

    ms->compress_all();
    while (true) {
      auto wals_count = ms->description().wal_count;
      if (wals_count == 0) {
        break;
      }
      dariadb::utils::sleep_mls(500);
    }
  }
  if (dariadb::utils::fs::path_exists(storage_path)) {
    dariadb::utils::fs::rm(storage_path);
  }
}

BOOST_AUTO_TEST_CASE(Subscribe) {
  const std::string storage_path = "testStorage";

  {
    if (dariadb::utils::fs::path_exists(storage_path)) {
      dariadb::utils::fs::rm(storage_path);
    }

    auto settings = dariadb::storage::Settings::create(storage_path);

    dariadb::IEngine_Ptr ms = std::make_shared<dariadb::Engine>(settings);

    dariadb_test::subscribe_test(ms.get());
  }
  if (dariadb::utils::fs::path_exists(storage_path)) {
    dariadb::utils::fs::rm(storage_path);
  }
}

BOOST_AUTO_TEST_CASE(Engine_MemStorage_common_test) {
  const std::string storage_path = "testStorage";
  const size_t chunk_size = 128;

  const dariadb::Time from = 0;
  const dariadb::Time to = from + 1000;
  const dariadb::Time step = 10;

  using namespace dariadb;
  using namespace dariadb::storage;

  {
    std::cout << "Engine_MemStorage_common_test\n";
    if (dariadb::utils::fs::path_exists(storage_path)) {
      dariadb::utils::fs::rm(storage_path);
    }

    auto settings = dariadb::storage::Settings::create(storage_path);
    settings->strategy.setValue(STRATEGY::MEMORY);
    settings->chunk_size.setValue(chunk_size);
    settings->max_chunks_per_page.setValue(5);
    settings->memory_limit.setValue(50 * 1024);
    std::unique_ptr<Engine> ms{new Engine(settings)};

    dariadb_test::storage_test_check(ms.get(), from, to, step, true, false, false);

    auto pages_count = ms->description().pages_count;
    BOOST_CHECK_GE(pages_count, size_t(2));
    ms->settings()->max_store_period.setValue(1);
  }
  if (dariadb::utils::fs::path_exists(storage_path)) {
    dariadb::utils::fs::rm(storage_path);
  }
}

BOOST_AUTO_TEST_CASE(Engine_MemOnlyStorage_common_test) {
  const size_t chunk_size = 128;

  const dariadb::Time from = 0;
  const dariadb::Time to = from + 1000;
  const dariadb::Time step = 10;

  using namespace dariadb;
  using namespace dariadb::storage;

  {
    std::cout << "Engine_MemOnlyStorage_common_test\n";

    auto settings = dariadb::storage::Settings::create();
    settings->chunk_size.setValue(chunk_size);
    std::unique_ptr<Engine> ms{new Engine(settings)};

    dariadb_test::storage_test_check(ms.get(), from, to, step, true, false, false);

    auto pages_count = ms->description().pages_count;
    BOOST_CHECK_EQUAL(pages_count, size_t(0));
    ms->settings()->max_store_period.setValue(1);
    while (true) {
      dariadb::QueryInterval qi({dariadb::Id(0)}, dariadb::Flag(), from, to);
      auto values = ms->readInterval(qi);
      if (values.empty()) {
        break;
      } else {
        std::cout << "values !empty() " << values.size() << std::endl;
        dariadb::utils::sleep_mls(500);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(Engine_Cache_common_test) {
  const std::string storage_path = "testStorage";

  const size_t chunk_size = 128;
  const dariadb::Time from = 0;
  const dariadb::Time to = from + 1000;
  const dariadb::Time step = 10;

  using namespace dariadb;
  using namespace dariadb::storage;

  {
    std::cout << "Engine_Cache_common_test\n";
    if (dariadb::utils::fs::path_exists(storage_path)) {
      dariadb::utils::fs::rm(storage_path);
    }

    auto settings = dariadb::storage::Settings::create(storage_path);
    settings->strategy.setValue(STRATEGY::CACHE);
    settings->chunk_size.setValue(chunk_size);
    settings->memory_limit.setValue(50 * 1024);
    settings->wal_file_size.setValue(2000);
    std::unique_ptr<Engine> ms{new Engine(settings)};

    dariadb_test::storage_test_check(ms.get(), from, to, step, true, true, false);

    auto descr = ms->description();
    BOOST_CHECK_GT(descr.pages_count, size_t(0));
    ms->settings()->max_store_period.setValue(1);
  }
  if (dariadb::utils::fs::path_exists(storage_path)) {
    dariadb::utils::fs::rm(storage_path);
  }
}
