#include "capacitor_manager.h"
#include "manifest.h"
#include "../utils/exception.h"
#include "../utils/fs.h"
#include <cassert>

#include <boost/thread/locks.hpp>

using namespace dariadb::storage;
using namespace dariadb;

CapacitorManager *CapacitorManager::_instance = nullptr;

CapacitorManager::~CapacitorManager(){
}

CapacitorManager::CapacitorManager(const Params & param):_params(param){
    if (!dariadb::utils::fs::path_exists(_params.path)) {
      dariadb::utils::fs::mkdir(_params.path);
    }

    auto files=Manifest::instance()->cola_list();
    for(auto f:files){
        auto full_path=utils::fs::append_path(_params.path,f);
        auto hdr=Capacitor::readHeader(full_path);
        if(!hdr.is_full){
             auto p=Capacitor::Params(_params.B,_params.path);
            _cap=Capacitor_Ptr{new Capacitor(p,full_path)};
            break;
        }
    }
    if(_cap==nullptr){
        create_new();
    }
}

void CapacitorManager::start(const Params & param){
	if (CapacitorManager::_instance == nullptr) {
		CapacitorManager::_instance = new CapacitorManager(param);
	}
	else {
		throw MAKE_EXCEPTION("CapacitorManager::start started twice.");
	}
}

void CapacitorManager::stop(){
	delete CapacitorManager::_instance;
	CapacitorManager::_instance = nullptr;
}

CapacitorManager * dariadb::storage::CapacitorManager::instance(){
	return CapacitorManager::_instance;
}

void CapacitorManager::create_new(){
    _cap=nullptr;
     auto p=Capacitor::Params(_params.B,_params.path);
    _cap=Capacitor_Ptr{new Capacitor(p)};
}

dariadb::Time CapacitorManager::minTime(){
	return Time();
}

dariadb::Time CapacitorManager::maxTime()
{
	return Time();
}

bool CapacitorManager::minMaxTime(dariadb::Id id, dariadb::Time * minResult, dariadb::Time * maxResult){
    boost::shared_lock<boost::shared_mutex> lg(_locker);
    auto files=Manifest::instance()->cola_list();
    auto p=Capacitor::Params(_params.B,_params.path);

    bool res=false;
    *minResult=dariadb::MAX_TIME;
    *maxResult=dariadb::MIN_TIME;

    for(auto f:files){
        auto filename=utils::fs::append_path(_params.path,f);
        auto raw=new Capacitor(p,filename,true);
        Capacitor_Ptr cptr{raw};
        dariadb::Time lmin=dariadb::MAX_TIME, lmax=dariadb::MIN_TIME;
        if(cptr->minMaxTime(id,&lmin,&lmax)){
            res=true;
            *minResult=std::min(lmin,*minResult);
            *maxResult=std::max(lmax,*maxResult);
        }
    }
    return res;
}

Reader_ptr CapacitorManager::readInterval(const QueryInterval & q){
	return Reader_ptr();
}

Reader_ptr CapacitorManager::readInTimePoint(const QueryTimePoint & q){
	return Reader_ptr();
}

Reader_ptr CapacitorManager::currentValue(const IdArray & ids, const Flag & flag){
	return Reader_ptr();
}

dariadb::append_result CapacitorManager::append(const Meas & value){
    boost::upgrade_lock<boost::shared_mutex> lg(_locker);
    auto res=_cap->append(value);
    if(res.writed!=1){
        create_new();
        return _cap->append(value);
    }else{
        return res;
    }
}

void CapacitorManager::flush(){
}

void CapacitorManager::subscribe(const IdArray & ids, const Flag & flag, const ReaderClb_ptr & clbk){
	NOT_IMPLEMENTED;
}
