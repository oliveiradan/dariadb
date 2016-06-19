#include "aofile.h"
#include "../flags.h"
#include "../utils/fs.h"
#include "inner_readers.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <mutex>

using namespace dariadb;
using namespace dariadb::storage;

class AOFile::Private {
public:
  Private(const AOFile::Params &params) : _params(params){
    _is_readonly = false;
  }

  Private(const AOFile::Params &params, const std::string &fname, bool readonly)
      : _params(params) {
    _is_readonly = readonly;
  }

  ~Private() {
    this->flush();
  }

  append_result append(const Meas &value) {
    assert(!_is_readonly);
    std::lock_guard<std::mutex> lock(_mutex);
    auto file=std::fopen(_params.path.c_str(), "ab");
    if(file!=nullptr){
        std::fwrite(&value,sizeof(Meas),size_t(1),file);
        std::fclose(file);
        return append_result(1, 0);
    }else{
        throw MAKE_EXCEPTION("aofile: append error.");
    }
  }

  append_result append(const Meas::MeasArray &ma){
      assert(!_is_readonly);
      std::lock_guard<std::mutex> lock(_mutex);
      auto file=std::fopen(_params.path.c_str(), "ab");
      if(file!=nullptr){
          std::fwrite(ma.data(),sizeof(Meas),ma.size(),file);
          std::fclose(file);
          return append_result(ma.size(), 0);
      }else{
          throw MAKE_EXCEPTION("aofile: append error.");
      }
  }

  append_result append(const Meas::MeasList &ml){
      assert(!_is_readonly);
      std::lock_guard<std::mutex> lock(_mutex);
      auto file=std::fopen(_params.path.c_str(), "ab");
      if(file!=nullptr){
          Meas::MeasArray ma{ml.begin(),ml.end()};
          std::fwrite(ma.data(),sizeof(Meas),ma.size(),file);
          std::fclose(file);
          return append_result(ma.size(), 0);
      }else{
          throw MAKE_EXCEPTION("aofile: append error.");
      }
  }

  Reader_ptr readInterval(const QueryInterval &q) {
    std::lock_guard<std::mutex> lock(_mutex);
    TP_Reader *raw = new TP_Reader;
    auto file=std::fopen(_params.path.c_str(), "rb");
    if(file==nullptr){
        throw MAKE_EXCEPTION("aof: file open error");
    }
    std::map<dariadb::Id, std::set<Meas, meas_time_compare_less>> sub_result;

    while(1){
        Meas val=Meas::empty();
        if(fread(&val,sizeof(Meas),size_t(1),file)==0){
            break;
        }
        if(val.inQuery(q.ids,q.flag,q.from,q.to)){
            sub_result[val.id].insert(val);
        }
    }
    std::fclose(file);

    for (auto &kv : sub_result) {
      raw->_ids.push_back(kv.first);
      for (auto &m : kv.second) {
        raw->_values.push_back(m);
      }
    }
    raw->reset();
    return Reader_ptr(raw);
  }

  Reader_ptr readInTimePoint(const QueryTimePoint &q) {
    std::lock_guard<std::mutex> lock(_mutex);
    dariadb::IdSet readed_ids;
    dariadb::Meas::Id2Meas sub_res;

    auto file=std::fopen(_params.path.c_str(), "rb");
    if(file==nullptr){
        throw MAKE_EXCEPTION("aof: file open error");
    }
     while(1){
        Meas val=Meas::empty();
        if(fread(&val,sizeof(Meas),size_t(1),file)==0){
            break;
        }
        if (val.inQuery(q.ids, q.flag) && (val.time <= q.time_point)) {
          replace_if_older(sub_res, val);
          readed_ids.insert(val.id);
        }
    }
    std::fclose(file);

    if (!q.ids.empty() && readed_ids.size() != q.ids.size()) {
      for (auto id : q.ids) {
        if (readed_ids.find(id) == readed_ids.end()) {
          auto e = Meas::empty(id);
          e.flag = Flags::_NO_DATA;
          e.time = q.time_point;
          sub_res[id] = e;
        }
      }
    }

    TP_Reader *raw = new TP_Reader;
    for (auto kv : sub_res) {
      raw->_values.push_back(kv.second);
      raw->_ids.push_back(kv.first);
    }

    raw->reset();
    return Reader_ptr(raw);

  }

  void replace_if_older(dariadb::Meas::Id2Meas &s, const dariadb::Meas &m) const {
    auto fres = s.find(m.id);
    if (fres == s.end()) {
      s.insert(std::make_pair(m.id, m));
    } else {
      if (fres->second.time < m.time) {
        s.insert(std::make_pair(m.id, m));
      }
    }
  }

  Reader_ptr currentValue(const IdArray &ids, const Flag &flag) {
    std::lock_guard<std::mutex> lock(_mutex);
    dariadb::Meas::Id2Meas sub_res;
    dariadb::IdSet readed_ids;
    auto file=std::fopen(_params.path.c_str(), "rb");
    if(file==nullptr){
        throw MAKE_EXCEPTION("aof: file open error");
    }
     while(1){
        Meas val=Meas::empty();
        if(fread(&val,sizeof(Meas),size_t(1),file)==0){
            break;
        }
        replace_if_older(sub_res, val);
        readed_ids.insert(val.id);
    }
    std::fclose(file);

    if (!ids.empty() && readed_ids.size() != ids.size()) {
      for (auto id : ids) {
        if (readed_ids.find(id) == readed_ids.end()) {
          auto e = Meas::empty(id);
          e.flag = Flags::_NO_DATA;
          e.time = dariadb::Time(0);
          sub_res[id] = e;
        }
      }
    }

    TP_Reader *raw = new TP_Reader;
    for (auto kv : sub_res) {
      raw->_values.push_back(kv.second);
      raw->_ids.push_back(kv.first);
    }

    raw->reset();
    return Reader_ptr(raw);

  }

  dariadb::Time minTime() const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto file=std::fopen(_params.path.c_str(), "rb");
    if(file==nullptr){
        throw MAKE_EXCEPTION("aof: file open error");
    }

    dariadb::Time result=dariadb::MAX_TIME;

    while(1){
        Meas val=Meas::empty();
        if(fread(&val,sizeof(Meas),size_t(1),file)==0){
            break;
        }
       result=std::min(val.time,result);
    }
    std::fclose(file);
    return result;
  }

  dariadb::Time maxTime() const {
    auto file=std::fopen(_params.path.c_str(), "rb");
    if(file==nullptr){
        throw MAKE_EXCEPTION("aof: file open error");
    }

    dariadb::Time result=dariadb::MIN_TIME;

    while(1){
        Meas val=Meas::empty();
        if(fread(&val,sizeof(Meas),size_t(1),file)==0){
            break;
        }
       result=std::max(val.time,result);
    }
    std::fclose(file);
     return result;
  }

  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                  dariadb::Time *maxResult) {
    auto file=std::fopen(_params.path.c_str(), "rb");
    if(file==nullptr){
        throw MAKE_EXCEPTION("aof: file open error");
    }

    *minResult=dariadb::MAX_TIME;
    *maxResult=dariadb::MIN_TIME;
    bool result=false;
    while(1){
        Meas val=Meas::empty();
        if(fread(&val,sizeof(Meas),size_t(1),file)==0){
            break;
        }
       if(val.id==id){
           result=true;
           *minResult=std::min(*minResult,val.time);
           *maxResult=std::max(*maxResult,val.time);
       }
    }
    std::fclose(file);
    return result;
  }

  void flush() {
    std::lock_guard<std::mutex> lock(_mutex);
    // if (drop_future.valid()) {
    //  drop_future.wait();
    //}
  }


  void drop_to_stor(MeasWriter *stor) {
  }

protected:
  AOFile::Params _params;

//  dariadb::utils::fs::MappedFile::MapperFile_ptr mmap;
//  AOFile::Header *_header;
//  uint8_t *_raw_data;

  mutable std::mutex _mutex;
  bool _is_readonly;
};

AOFile::~AOFile() {}

AOFile::AOFile(const Params &params) : _Impl(new AOFile::Private(params)) {}

AOFile::AOFile(const AOFile::Params &params, const std::string &fname,
               bool readonly)
    : _Impl(new AOFile::Private(params, fname, readonly)) {}

//AOFile::Header AOFile::readHeader(std::string file_name) {
//  std::ifstream istream;
//  istream.open(file_name, std::fstream::in | std::fstream::binary);
//  if (!istream.is_open()) {
//    std::stringstream ss;
//    ss << "can't open file. filename=" << file_name;
//    throw MAKE_EXCEPTION(ss.str());
//  }
//  AOFile::Header result;
//  memset(&result, 0, sizeof(AOFile::Header));
//  istream.read((char *)&result, sizeof(AOFile::Header));
//  istream.close();
//  return result;
//}
dariadb::Time AOFile::minTime() { return _Impl->minTime(); }

dariadb::Time AOFile::maxTime() { return _Impl->maxTime(); }

bool AOFile::minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                        dariadb::Time *maxResult) {
  return _Impl->minMaxTime(id, minResult, maxResult);
}
void AOFile::flush() { // write all to storage;
  _Impl->flush();
}

append_result AOFile::append(const Meas &value) { return _Impl->append(value); }
append_result AOFile::append(const Meas::MeasArray &ma) { return _Impl->append(ma); }
append_result AOFile::append(const Meas::MeasList &ml) { return _Impl->append(ml); }

Reader_ptr AOFile::readInterval(const QueryInterval &q) {
  return _Impl->readInterval(q);
}

Reader_ptr AOFile::readInTimePoint(const QueryTimePoint &q) {
  return _Impl->readInTimePoint(q);
}

Reader_ptr AOFile::currentValue(const IdArray &ids, const Flag &flag) {
  return _Impl->currentValue(ids, flag);
}

void AOFile::drop_to_stor(MeasWriter *stor) { _Impl->drop_to_stor(stor); }