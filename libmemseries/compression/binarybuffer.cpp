#include "binarybuffer.h"
#include "../utils.h"
#include <cassert>

using namespace memseries;
using namespace memseries::compression;

BinaryBuffer::BinaryBuffer(const utils::Range& r):
    _begin(r.begin),
    _end(r.end),
    _bitnum(max_bit_pos)
{
    _cap=(std::distance(_begin,_end));
    _pos=_cap-1;
}


BinaryBuffer::BinaryBuffer(const BinaryBuffer & other) {
    this->_begin = other._begin;
    this->_end = other._end;
    _pos = other._pos;
    _bitnum = other._bitnum;
    _cap = other._cap;
}

BinaryBuffer::BinaryBuffer(BinaryBuffer && other){
    _begin = other._begin;
    _end = other._end;
    _pos = other._pos;
    _cap = other._cap;
    _bitnum = other._bitnum;
    other._pos = 0;
    other._cap = 0;
    other._bitnum = 0;
    other._begin=other._end=nullptr;
}

BinaryBuffer::~BinaryBuffer(){
}

BinaryBuffer& BinaryBuffer::operator=(const BinaryBuffer & other){
    if(this!=&other){
        BinaryBuffer tmp(other);
        tmp.swap(*this);
    }
    return *this;
}

BinaryBuffer &BinaryBuffer::operator=(const BinaryBuffer &&other){
    if(this!=&other){
        BinaryBuffer tmp(other);
        tmp.swap(*this);
    }
    return *this;
}

void BinaryBuffer::swap(BinaryBuffer & other) throw(){
    std::swap(_pos, other._pos);
    std::swap(_cap, other._cap);
    std::swap(_begin, other._begin);
    std::swap(_end, other._end);
    std::swap(_bitnum, other._bitnum);
}

BinaryBuffer& BinaryBuffer::incbit(){
    _bitnum--;
    if (_bitnum < 0) {
        incpos();
        _bitnum = max_bit_pos;
    }
    return *this;
}

BinaryBuffer& BinaryBuffer::incpos(){
    _pos--;
    if (_pos==0){
        throw MAKE_EXCEPTION("BinaryBuffer::incpos");
    }
    return *this;
}

void BinaryBuffer::set_bitnum(size_t num) {
    this->_bitnum = num;
}

void BinaryBuffer::set_pos(size_t pos) {
    this->_pos = pos;
}

void BinaryBuffer::reset_pos() {
    this->set_pos(_cap-1);
    this->set_bitnum(max_bit_pos);
}

uint8_t BinaryBuffer::getbit() const{
    return utils::BitOperations::get(_begin[_pos],_bitnum);
}


BinaryBuffer& BinaryBuffer::setbit() {
    _begin[_pos]=utils::BitOperations::set(_begin[_pos],_bitnum);
    return *this;
}

BinaryBuffer& BinaryBuffer::clrbit() {
    _begin[_pos] =utils::BitOperations::clr(_begin[_pos],_bitnum);
    return *this;
}

void BinaryBuffer::write(uint16_t v,int8_t count){
    const auto bits_in_ui16=sizeof(uint16_t)*8;

    uint32_t *dest = (uint32_t*)(_begin + _pos - 3);
    auto src = uint32_t(v) << (bits_in_ui16 - count - 1);
    src = src << (bits_in_ui16 - (max_bit_pos - _bitnum));
    *dest |= src;
    move_pos(count);
}

void BinaryBuffer::write(uint64_t v, int8_t count) {
    const auto bits_in_ui64=sizeof(uint64_t)*8;
    assert(count < 47);
    uint64_t *dest = (uint64_t*)(_begin + _pos - 7);
    auto shl_cnt=((bits_in_ui64 - count - 1) - (max_bit_pos - _bitnum));
    auto src = uint64_t(v) << shl_cnt;
    *dest |= src;
    move_pos(count);
}

uint64_t  BinaryBuffer::read(int8_t count) {
    uint64_t src = *(uint64_t*)(_begin + _pos - 7);
    src <<= (max_bit_pos - _bitnum);
    src >>= (sizeof(uint64_t) * 8 - count - 1);
    move_pos(count);
    return src;
}

std::ostream&  memseries::compression::operator<< (std::ostream& stream, const BinaryBuffer& b) {
    stream << " pos:" << b.pos() << " cap:" << b.cap() << " bit:" << b.bitnum() << " [";
    for (size_t i = 0; i <= b.pos(); i++) {
        for (int bit = int(max_bit_pos); bit >= 0; bit--) {
            auto is_cur = ((bit == b.bitnum()) && (i == b.pos()));
            if (is_cur)
                stream << "[";
            stream << ((b._begin[i] >> bit) & 1);
            if (is_cur)
                stream << "]";
            if (bit == 4) stream << " ";
        }
        stream << std::endl;
    }
    return stream << "]";
}