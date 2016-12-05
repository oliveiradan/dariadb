#include <libdariadb/engine.h>
#include <libdariadb/config.h>
#include <libdariadb/flags.h>
#include <libdariadb/timeutil.h>
#include <libdariadb/storage/memstorage.h>
#include <libdariadb/storage/engine_environment.h>
#include <libdariadb/storage/dropper.h>
#include <libdariadb/storage/lock_manager.h>
#include <libdariadb/storage/manifest.h>
#include <libdariadb/storage/page_manager.h>
#include <libdariadb/storage/subscribe.h>
#include <libdariadb/utils/exception.h>
#include <libdariadb/utils/locker.h>
#include <libdariadb/utils/logger.h>
#include <libdariadb/utils/metrics.h>
#include <libdariadb/utils/thread_manager.h>
#include <libdariadb/utils/utils.h>
#include <libdariadb/utils/strings.h>
#include <libdariadb/utils/fs.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <shared_mutex>

using namespace dariadb;
using namespace dariadb::storage;
using namespace dariadb::utils::async;

class Engine::Private {
public:
  Private(Settings_ptr settings) {
	  _settings = settings;
	  _strategy = _settings->strategy.value;

	  _engine_env = EngineEnvironment_ptr{ new EngineEnvironment() };
	  _engine_env->addResource(EngineEnvironment::Resource::SETTINGS, _settings.get());

    logger_info("engine: project version - ", PROJECT_VERSION_MAJOR,'.',PROJECT_VERSION_MINOR,'.',PROJECT_VERSION_PATCH);
    logger_info("engine: storage version - ", this->version());
    logger_info("engine: strategy - ", _settings->strategy.value);
    bool is_exists = false;
    _stoped = false;
    if (!dariadb::utils::fs::path_exists(_settings->path)) {
      dariadb::utils::fs::mkdir(_settings->path);
    } else {
      lockfile_lock_or_die();
      is_exists = true;
    }
    _subscribe_notify.start();
    ThreadManager::Params tpm_params(_settings->thread_pools_params());
    ThreadManager::start(tpm_params);
	
	_lock_manager = LockManager_ptr{ new  LockManager(LockManager::Params()) };
	_engine_env->addResource(EngineEnvironment::Resource::LOCK_MANAGER, _lock_manager.get());
    
	_manifest = Manifest_ptr{ new Manifest{ utils::fs::append_path(_settings->path, MANIFEST_FILE_NAME) } };
	_engine_env->addResource(EngineEnvironment::Resource::MANIFEST, _manifest.get());

    if (is_exists) {
      Dropper::cleanStorage(_settings->path);
    }

	_page_manager = PageManager_ptr{ new PageManager(_engine_env) };

    if (utils::fs::path_exists(utils::fs::append_path(settings->path, MANIFEST_FILE_NAME))) {
        _manifest->set_version(std::to_string(this->version()));
    } else {
      check_storage_version();
    }

	if (_settings->load_min_max) {
		_min_max_map = _page_manager->loadMinMax();
	}

    if (_strategy != STRATEGY::MEMORY) {
		_aof_manager = AOFManager_ptr{ new AOFManager(_engine_env) };

		_dropper = std::make_unique<Dropper>(_engine_env, _page_manager, _aof_manager);
		_aof_manager->set_downlevel(_dropper.get());
		this->_top_level_storage = _aof_manager;
	}

	if(_strategy ==STRATEGY::CACHE || _strategy == STRATEGY::MEMORY){
        _memstorage = MemStorage_ptr{ new MemStorage(_settings,_min_max_map.size()) };
		if (_strategy != STRATEGY::CACHE) {
			_memstorage->setDownLevel(_page_manager.get());
		}
		_top_level_storage = _memstorage;
		if (_strategy == STRATEGY::CACHE) {
			_memstorage->setDiskStorage(_aof_manager.get());
		}
	}

    if(_strategy == STRATEGY::FAST_WRITE){
		if (_settings->load_min_max) {
			auto amm = _top_level_storage->loadMinMax();
			minmax_append(_min_max_map,amm);
		}
    }

	logger_info("engine: start - OK ");
  }
  ~Private() { this->stop(); }

  void stop() {
    if (!_stoped) {
	  _top_level_storage = nullptr;
      _subscribe_notify.stop();

      this->flush();
	  if (_memstorage != nullptr) {
		  _memstorage = nullptr;
	  }
	  _aof_manager = nullptr;
	  _page_manager = nullptr;
	  _manifest = nullptr;
	  _lock_manager = nullptr;
	  _dropper = nullptr;
      _stoped = true;

	  ThreadManager::stop();
      lockfile_unlock();
    }
  }

  std::string lockfile_path(){
      return utils::fs::append_path(_settings->path, "lockfile");
  }

  void lockfile_lock_or_die(){
      auto lfile=lockfile_path();
      if(utils::fs::path_exists(lfile)){
         throw_lock_error( _settings->path);
      }
      std::ofstream ofs;
      ofs.open(lfile, std::ios_base::out | std::ios_base::binary);
      if(!ofs.is_open()){
         throw_lock_error( _settings->path);
      }
      ofs<<"locked.";
      ofs.close();
  }

  void lockfile_unlock(){
      auto lfile=lockfile_path();
      utils::fs::rm(lfile);
  }

  [[noreturn]]
  void throw_lock_error(const std::string&lock_file) {
       logger_fatal("engine: storage ",lock_file," is locked.");
       std::exit(1);
  }

  void check_storage_version() {
    auto current_version = this->version();
    auto storage_version = std::stoi(_manifest->get_version());
    if (storage_version != current_version) {
      logger_info("engine: openning storage with version - ", storage_version);
      THROW_EXCEPTION("engine: openning storage with greater version.");
    }
  }

  void lock_storage() {
    switch (_strategy) {
    case STRATEGY::COMPRESSED:
    case STRATEGY::FAST_WRITE:
      _lock_manager->lock(LOCK_KIND::READ, {LOCK_OBJECTS::PAGE, LOCK_OBJECTS::AOF});
      break;
    case STRATEGY::MEMORY: {
      _memstorage->lock_drop();
      _lock_manager->lock(LOCK_KIND::READ, {LOCK_OBJECTS::PAGE});
      break;
    }
    case STRATEGY::CACHE: {
      _memstorage->lock_drop();
      _lock_manager->lock(LOCK_KIND::READ, {LOCK_OBJECTS::PAGE, LOCK_OBJECTS::AOF});
      break;
    }
    }
  }

  void unlock_storage() {
    switch (_strategy) {
    case STRATEGY::COMPRESSED:
    case STRATEGY::FAST_WRITE:
      _lock_manager->unlock({LOCK_OBJECTS::PAGE, LOCK_OBJECTS::AOF});
      break;
    case STRATEGY::MEMORY: {
      _memstorage->unlock_drop();
      _lock_manager->unlock({LOCK_OBJECTS::PAGE});
      break;
    }
    case STRATEGY::CACHE: {
      _memstorage->unlock_drop();
      _lock_manager->unlock({LOCK_OBJECTS::PAGE, LOCK_OBJECTS::AOF});
      break;
    }
    }
  }

  Time minTime() {
	lock_storage();

	auto pmin = _page_manager->minTime();
	if (_strategy == STRATEGY::CACHE) {
		auto amin = this->_aof_manager->minTime();
		pmin = std::min(pmin, amin);
	}
	Time amin= _top_level_storage->minTime();

	unlock_storage();
    return std::min(pmin, amin);
  }

  Time maxTime() {
	lock_storage();

    auto pmax = _page_manager->maxTime();
	if (_strategy == STRATEGY::CACHE) {
		auto amax = this->_aof_manager->maxTime();
		pmax = std::max(pmax, amax);
	}
	Time amax= _top_level_storage->maxTime();

	unlock_storage();
    return std::max(pmax, amax);
  }

  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult, dariadb::Time *maxResult) {
    TIMECODE_METRICS(ctmd, "minMaxTime", "Engine::minMaxTime");
    dariadb::Time subMin1 = dariadb::MAX_TIME, subMax1 = dariadb::MIN_TIME;
    dariadb::Time subMin3 = dariadb::MAX_TIME, subMax3 = dariadb::MIN_TIME;
    bool pr, ar;
    pr = ar = false;
	auto pm = _page_manager.get();
    AsyncTask pm_at = [&pr, &subMin1, &subMax1, id, pm](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::COMMON, ti.kind);
      pr = pm->minMaxTime(id, &subMin1, &subMax1);
	  return false;
    };
	auto am = _top_level_storage.get();
    AsyncTask am_at = [&ar, &subMin3, &subMax3, id, am](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::COMMON, ti.kind);
	  ar = am->minMaxTime(id, &subMin3, &subMax3);
	  return false;
	};

	lock_storage();

    auto pm_async = ThreadManager::instance()->post(THREAD_COMMON_KINDS::COMMON, AT(pm_at));
    auto am_async = ThreadManager::instance()->post(THREAD_COMMON_KINDS::COMMON, AT(am_at));

    pm_async->wait();
    am_async->wait();

    unlock_storage();

    *minResult = dariadb::MAX_TIME;
    *maxResult = dariadb::MIN_TIME;

    *minResult = std::min(subMin1, subMin3);
    *maxResult = std::max(subMax1, subMax3);
    return pr  || ar;
  }

  Id2MinMax loadMinMax(){
      this->lock_storage();

      auto p_mm=this->_page_manager->loadMinMax();
	  if (_strategy == STRATEGY::CACHE) {
		  auto a_mm = this->_aof_manager->loadMinMax();
		  minmax_append(p_mm, a_mm);
	  }
      auto t_mm=this->_top_level_storage->loadMinMax();

      this->unlock_storage();

      minmax_append(p_mm,t_mm);
      return p_mm;
  }

  Status  append(const Meas &value) {
    Status  result{};
	_min_max_locker.lock_shared();
      auto fres=_min_max_map.find(value.id);
      if(fres!=_min_max_map.end()){
		  _min_max_locker.unlock_shared();
          auto max_v=fres->second;
          if(max_v.max.time>value.time){
            result.ignored=1;
            std::stringstream ss;
            ss<<"Id:"<<value.id;
            ss<<", writing to past";
            auto err_message=ss.str();
            result.error_message=err_message;
            logger_info("engine: ",err_message);
            return result;
          }
	  }
	  else {
		  _min_max_locker.unlock_shared();
	  }

	result=_top_level_storage->append(value);

    if (result.writed == 1) {
      _subscribe_notify.on_append(value);
	  _min_max_locker.lock();
	  auto insert_fres = _min_max_map.find(value.id);
	  if (insert_fres == _min_max_map.end()) {
		  _min_max_map[value.id].max = value;
	  }
	  else {
		  insert_fres->second.updateMax(value);
	  }
	  _min_max_locker.unlock();
    }

    return result;
  }

  void subscribe(const IdArray &ids, const Flag &flag, const ReaderClb_ptr &clbk) {
    auto new_s = std::make_shared<SubscribeInfo>(ids, flag, clbk);
    _subscribe_notify.add(new_s);
  }

  Id2Meas currentValue(const IdArray &ids, const Flag &flag) {
	lock_storage();

	Id2Meas a_result;
	for (auto kv : _min_max_map) {
		bool ids_check = false;
		if (ids.size() == 0) {
			ids_check = true;
		}
		else {
			if (std::find(ids.begin(), ids.end(), kv.first) != ids.end()) {
				ids_check = true;
			}
		}
		if (ids_check) {
			if (kv.second.max.inFlag(flag)) {
				a_result.emplace(std::make_pair(kv.first, kv.second.max));
			}
		}
	}
	unlock_storage();
    return a_result;
  }

  void flush() {
    TIMECODE_METRICS(ctmd, "flush", "Engine::flush");
    std::lock_guard<std::mutex> lg(_flush_locker);

	if (_aof_manager != nullptr) {
		_aof_manager->flush();
		_dropper->flush();
		_dropper->flush();
	}
	if (_memstorage != nullptr) {
		_memstorage->flush();
	}
	_page_manager->flush();
  }

  void wait_all_asyncs() { ThreadManager::instance()->flush(); }

  Engine::Description description() const {
    Engine::Description result;
    memset(&result, 0, sizeof(Description));
    result.aofs_count = _aof_manager==nullptr?0:_aof_manager->files_count();
    result.pages_count = _page_manager->files_count();
    result.active_works = ThreadManager::instance()->active_works();
	if (_dropper != nullptr) {
        result.dropper = _dropper->description();
	}
	if (_memstorage != nullptr) {
		result.memstorage = _memstorage->description();
	}
    return result;
  }

  /// when strategy=CACHE
  void foreach_internal_cache(const QueryInterval &q, IReaderClb *p_clbk,
                              IReaderClb *a_clbk) {
    auto pm = _page_manager.get();
    auto mm = _memstorage.get();
    auto am = _aof_manager.get();

    auto memory_mm = mm->loadMinMax();
	auto sync_map=mm->getSyncMap();
    auto local_q = q;
    local_q.ids.resize(1);
    for (auto id : q.ids) {
		local_q.from = q.from;
		local_q.to = q.to;
      local_q.ids[0] = id;
      auto id_mm = memory_mm.find(id);
      if (id_mm == memory_mm.end()) {
        pm->foreach (local_q, p_clbk);
        am->foreach (local_q, a_clbk);
      } else {
        if ((id_mm->second.min.time) > local_q.from) {
			auto min_mem_time = sync_map[id];
			local_q.to = min_mem_time;
          pm->foreach (local_q, p_clbk);
          am->foreach (local_q, a_clbk);
		  
		 
		  if (min_mem_time < q.to) {
			  if (min_mem_time != MIN_TIME) {//to read value after min_mem_time;
				  min_mem_time += 1;
			  }
			  local_q.from = min_mem_time;
			  local_q.to = q.to;
			  mm->foreach(local_q, a_clbk);
		  }
        } else {
          mm->foreach (local_q, a_clbk);
        }
        if (a_clbk->is_canceled()) {
          break;
        }
      }
    }
  }

  /// when strategy!=CACHEs
  void foreach_internal_two_level(const QueryInterval &q, IReaderClb *p_clbk,
                                  IReaderClb *a_clbk) {
    auto pm = _page_manager.get();
    auto tm = _top_level_storage.get();
    if (!p_clbk->is_canceled()) {
      pm->foreach (q, p_clbk);
    }
    if (!a_clbk->is_canceled()) {
      tm->foreach (q, a_clbk);
    }
  }

  void foreach_internal(const QueryInterval &q, IReaderClb *p_clbk, IReaderClb *a_clbk) {
    TIMECODE_METRICS(ctmd, "foreach", "Engine::internal_foreach");
    AsyncTask pm_at = [p_clbk, a_clbk, q, this](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::COMMON, ti.kind);
      this->lock_storage();
      if (this->strategy() == STRATEGY::CACHE) {
        foreach_internal_cache(q, p_clbk, a_clbk);
      } else {
        foreach_internal_two_level(q, p_clbk, a_clbk);
      }
      this->unlock_storage();
      a_clbk->is_end();
	  return false;
    };

    ThreadManager::instance()->post(THREAD_COMMON_KINDS::COMMON, AT(pm_at));
  }

  void foreach (const QueryInterval &q, IReaderClb * clbk) {
	  lock_storage();
      foreach_internal(q, clbk, clbk);
	  unlock_storage();
  }

  void foreach (const QueryTimePoint &q, IReaderClb * clbk) {
    auto values = this->readTimePoint(q);
    for (auto &kv : values) {
		if (clbk->is_canceled()) {
			break;
		}
      clbk->call(kv.second);
    }
    clbk->is_end();
  }

  MeasList readInterval(const QueryInterval &q) {
    TIMECODE_METRICS(ctmd, "readInterval", "Engine::readInterval");
	auto p_clbk = std::make_unique<MList_ReaderClb>();
	auto a_clbk = std::make_unique<MList_ReaderClb>();
    this->foreach_internal(q, p_clbk.get(), a_clbk.get());
	a_clbk->wait();
    Id2MSet sub_result;
	MeasList result;

    mlist2mset(p_clbk->mlist, sub_result);
    mlist2mset(a_clbk->mlist, sub_result);
	
    for (auto id : q.ids) {
      auto sublist = sub_result.find(id);
      if (sublist == sub_result.end()) {
        continue;
      }
      for (auto v : sublist->second) {
        result.push_back(v);
      }
    }
	
    return result;
  }

  Id2Meas readTimePoint(const QueryTimePoint &q) {
    TIMECODE_METRICS(ctmd, "readTimePoint", "Engine::readTimePoint");
    Id2Meas result;
    result.reserve(q.ids.size());
    for (auto id : q.ids) {
      result[id].flag = Flags::_NO_DATA;
    }

    auto pm = _page_manager.get();
    auto mm = _top_level_storage.get();
	auto am = _aof_manager.get();
    AsyncTask pm_at = [&result, &q, this, pm, mm, am](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::COMMON, ti.kind);

      lock_storage();

      for (auto id : q.ids) {
        dariadb::Time minT, maxT;
        QueryTimePoint local_q = q;
        local_q.ids.clear();
        local_q.ids.push_back(id);

        if (mm->minMaxTime(id, &minT, &maxT) &&
            (minT < q.time_point || maxT < q.time_point)) {
          auto subres = mm->readTimePoint(local_q);
          result[id] = subres[id];
          continue;
        } else {
			bool in_aof_level = false;
			if (this->strategy() == STRATEGY::CACHE) {
				auto subres = am->readTimePoint(local_q);
				auto value= subres[id];
				result[id] = value;
				in_aof_level = value.flag != Flags::_NO_DATA;
			}
			if (!in_aof_level) {
				auto subres = _page_manager->valuesBeforeTimePoint(local_q);
				result[id] = subres[id];
			}
        }
      }

      unlock_storage();
	  return false;
    };

    auto pm_async =
        ThreadManager::instance()->post(THREAD_COMMON_KINDS::COMMON, AT(pm_at));
    pm_async->wait();
    return result;
  }

  void drop_part_aofs(size_t count) { 
	  if (_aof_manager != nullptr) {
          logger_info("engine: drop_part_aofs ",count);
		  _aof_manager->drop_closed_files(count);
	  }
  }

  void compress_all() {
    if (_aof_manager != nullptr) {
      logger_info("engine: compress_all");
      _aof_manager->drop_all();
      this->flush();
    }
  }
  void fsck() {
    logger_info("engine: fsck ", _settings->path);
    _page_manager->fsck();
  }

  void eraseOld(const Time &t) {
    logger_info("engine: eraseOld to ", timeutil::to_string(t));
    _lock_manager->lock(LOCK_KIND::EXCLUSIVE, {LOCK_OBJECTS::PAGE});
    _page_manager->eraseOld(t);
    _lock_manager->unlock(LOCK_OBJECTS::PAGE);
  }

  uint16_t version() {
      return STORAGE_VERSION;
  }

  STRATEGY strategy()const{
	  assert(_strategy == _settings->strategy.value);
      return this->_strategy;
  }

  void compactTo(uint32_t pagesCount) {
    _lock_manager->lock(LOCK_KIND::EXCLUSIVE, {LOCK_OBJECTS::PAGE});
    logger_info("engine: compacting to ", pagesCount + 1);
    _page_manager->compactTo(pagesCount);
    _lock_manager->unlock(LOCK_OBJECTS::PAGE);
  }

  void compactbyTime(Time from, Time to) {
    _lock_manager->lock(LOCK_KIND::EXCLUSIVE, {LOCK_OBJECTS::PAGE});
    logger_info("engine: compacting by time ", timeutil::to_string(from), "-",
                timeutil::to_string(to));
    _page_manager->compactbyTime(from, to);
    _lock_manager->unlock(LOCK_OBJECTS::PAGE);
  }

protected:
  mutable std::mutex _flush_locker;
  SubscribeNotificator _subscribe_notify;

  std::unique_ptr<Dropper> _dropper;
  LockManager_ptr _lock_manager;
  PageManager_ptr _page_manager;
  AOFManager_ptr _aof_manager;
  MemStorage_ptr _memstorage;

  EngineEnvironment_ptr _engine_env;
  Settings_ptr _settings;
  STRATEGY     _strategy;
  Manifest_ptr _manifest;
  bool _stoped;

  IMeasStorage_ptr _top_level_storage; //aof or memory storage.
  
  Id2MinMax _min_max_map;
  std::shared_mutex _min_max_locker;
};

Engine::Engine(Settings_ptr settings) : _impl{new Engine::Private(settings)} {}

Engine::~Engine() {
  _impl = nullptr;
}

Time Engine::minTime() {
  return _impl->minTime();
}

Time Engine::maxTime() {
  return _impl->maxTime();
}
bool Engine::minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                        dariadb::Time *maxResult) {
  return _impl->minMaxTime(id, minResult, maxResult);
}

Status  Engine::append(const Meas &value) {
  return _impl->append(value);
}

void Engine::subscribe(const IdArray &ids, const Flag &flag, const ReaderClb_ptr &clbk) {
  _impl->subscribe(ids, flag, clbk);
}

Id2Meas Engine::currentValue(const IdArray &ids, const Flag &flag) {
  return _impl->currentValue(ids, flag);
}

void Engine::flush() {
  _impl->flush();
}

void Engine::stop() {
  _impl->stop();
}
Engine::Description Engine::description() const {
  return _impl->description();
}

void Engine::foreach (const QueryInterval &q, IReaderClb * clbk) {
  return _impl->foreach (q, clbk);
}

void Engine::foreach (const QueryTimePoint &q, IReaderClb * clbk) {
  return _impl->foreach (q, clbk);
}

MeasList Engine::readInterval(const QueryInterval &q) {
  return _impl->readInterval(q);
}

Id2Meas Engine::readTimePoint(const QueryTimePoint &q) {
  return _impl->readTimePoint(q);
}

void Engine::drop_part_aofs(size_t count) {
  return _impl->drop_part_aofs(count);
}

void Engine::compress_all(){
  return _impl->compress_all();
}

void Engine::wait_all_asyncs() {
  return _impl->wait_all_asyncs();
}

void Engine::fsck() {
  _impl->fsck();
}

void Engine::eraseOld(const Time&t) {
	return _impl->eraseOld(t);
}

void Engine::compactTo(uint32_t pagesCount) {
	_impl->compactTo(pagesCount);
}

void Engine::compactbyTime(Time from, Time to) {
	_impl->compactbyTime(from,to);
}
uint16_t Engine::version() {
	return _impl->version();
}

STRATEGY Engine::strategy()const{
    return _impl->strategy();
}

Id2MinMax Engine::loadMinMax(){
    return _impl->loadMinMax();
}
