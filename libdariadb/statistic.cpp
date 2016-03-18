#include "statistic.h"
#include <cassert>

using namespace  dariadb::statistic::integral;

BaseIntegral::BaseIntegral() {
	_is_first = true;
	_result = 0;
}

void BaseIntegral::call(const dariadb::Meas&m){
    if(_is_first){
        _last=m;
        _is_first=false;
    }else{
        this->calc(_last,m);
        _last=m;
    }
}

dariadb::Value BaseIntegral::result()const {
	return _result;
}

RectangleMethod::RectangleMethod(const RectangleMethod::Kind k): 
	BaseIntegral(),
	_kind(k)
{}



void RectangleMethod::calc(const dariadb::Meas&a, const dariadb::Meas&b){
	switch (_kind)
	{
	case Kind::LEFT:
		_result += a.value*(b.time - a.time);
		break;
	case Kind::RIGHT:
		_result += b.value*(b.time - a.time);
		break;
	case Kind::MIDLE:
		_result += ((a.value + b.value) / 2.0)*(b.time - a.time);
		break;
	default:
		assert(false);
	}
}