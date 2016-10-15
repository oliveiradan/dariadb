#include <libdariadb/utils/exception.h>
#include <libdariadb/utils/locker.h>
#include <libdariadb/utils/logger.h>
#include <common/async_connection.h>
#include <common/net_common.h>
#include <libclient/client.h>
#include <libclient/messages.pb.h>

#include <boost/asio.hpp>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <string>

using namespace std::placeholders;
using namespace boost::asio;

using namespace dariadb;
using namespace dariadb::net;
using namespace dariadb::net::client;

typedef boost::shared_ptr<ip::tcp::socket> socket_ptr;

class Client::Private{
public:
  Private(const Client::Param &p): _params(p) {
    _query_num = 1;
    _state = CLIENT_STATE::CONNECT;
    _pings_answers = 0;
	AsyncConnection::onDataRecvHandler on_d = [this](const NetData_ptr &d, bool &cancel, bool &dont_free_memory) {
		onDataRecv(d, cancel, dont_free_memory);
	};
	AsyncConnection::onNetworkErrorHandler on_n = [this](const boost::system::error_code &err) {
		onNetworkError(err);
	};
	_async_connection = std::shared_ptr<AsyncConnection>{new AsyncConnection(&_pool, on_d, on_n)};
  }

  ~Private() noexcept(false) {
    try {
      if (_state != CLIENT_STATE::DISCONNECTED && _socket != nullptr) {
        this->disconnect();
      }

      _service.stop();
      _thread_handler.join();
    } catch (std::exception &ex) {
      THROW_EXCEPTION("client: #" , _async_connection->id(), ex.what());
    }
  }

  void connect() {
    logger_info("client: connecting to ", _params.host, ':', _params.port);

    _state = CLIENT_STATE::CONNECT;
    auto t=std::thread{&Client::Private::client_thread, this};
    _thread_handler = std::move(t);

    while (this->_state != CLIENT_STATE::WORK) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }

  void disconnect() {
    if (_socket->is_open()) {
      auto nd = _async_connection->get_pool()->construct(
          dariadb::net::messages::DISCONNECT, _async_connection->id());
      this->_async_connection->send(nd);
    }

    while (this->_state != CLIENT_STATE::DISCONNECTED) {
      logger_info("client: #", _async_connection->id(), " disconnect - wait server answer...");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  void client_thread() {
      ip::tcp::resolver resolver(_service);

      ip::tcp::resolver::query query(_params.host, std::to_string(_params.port));
      ip::tcp::resolver::iterator iter = resolver.resolve( query);
      ip::tcp::endpoint ep = *iter;

      auto raw_sock_ptr = new ip::tcp::socket(_service);
      _socket = socket_ptr{raw_sock_ptr};
	  _socket->async_connect(ep, [this](auto ec) {
		  if (ec) {
			  THROW_EXCEPTION("dariadb::client: error on connect - ", ec.message());
		  }
		  this->_async_connection->start(this->_socket);
		  std::lock_guard<utils::Locker> lg(_locker);
		  auto hn = ip::host_name();

		  logger_info("client: send hello ", hn);

		  auto nd = this->_pool.construct();
          nd->size=NetData::MAX_MESSAGE_SIZE-MARKER_SIZE;

          dariadb::net::messages::QueryHeader qhdr;
          qhdr.set_id(0);
          qhdr.set_kind(dariadb::net::messages::HELLO);

          dariadb::net::messages::QueryHello qhm;
          qhm.set_host(hn);
          qhm.set_version(PROTOCOL_VERSION);
          qhdr.set_submessage(qhm.SerializeAsString());

          if(!qhdr.SerializeToArray(nd->data, nd->size)){
              THROW_EXCEPTION("hello message serialize error");
          }

          nd->size=qhdr.ByteSize();


		  this->_async_connection->send(nd);
	  });
      _service.run();
  }

  void onNetworkError(const boost::system::error_code &err) {
    if (this->_state != CLIENT_STATE::DISCONNECTED) {
      THROW_EXCEPTION("client: #" , _async_connection->id() , err.message());
    }
  }

  void onDataRecv(const NetData_ptr &d, bool &cancel, bool &) {
      dariadb::net::messages::QueryHeader qhdr;
      qhdr.ParseFromArray(d->data, d->size);

    dariadb::net::messages::QueryKind kind = qhdr.kind();
    switch (kind) {
    case dariadb::net::messages::QueryKind::OK: {
      auto query_num = qhdr.id();
      logger_info("client: #", _async_connection->id(), " query #", query_num,
                  " accepted.");
      if (this->_state != CLIENT_STATE::WORK) {
        THROW_EXCEPTION("(this->_state != CLIENT_STATE::WORK)", this->_state);
      }

      auto subres_it = this->_query_results.find(query_num);
      if (subres_it != this->_query_results.end()) {
        subres_it->second->is_ok = true;
        if (subres_it->second->kind ==  dariadb::net::messages::QueryKind::SUBSCRIBE) {
          subres_it->second->locker.unlock();
        }
      } else {
        THROW_EXCEPTION("client: query #", query_num, " not found");
      }
      break;
    }
    case dariadb::net::messages::QueryKind::ERR: {
        dariadb::net::messages::QueryError qerr;
        qerr.ParseFromString(qhdr.submessage());
      auto query_num = qhdr.id();
      ERRORS err = (ERRORS)qerr.errpr_code();
      logger_info("client: #", _async_connection->id(), " query #", query_num,
                  " error:", err);
      if (this->state() == CLIENT_STATE::WORK) {
        auto subres = this->_query_results[query_num];
        subres->is_closed = true;
        subres->is_error = true;
        subres->errc = err;
        subres->locker.unlock();
        _query_results.erase(query_num);
      }
      break;
    }
    case dariadb::net::messages::QueryKind::APPEND: {
//      auto qw = reinterpret_cast<QueryAppend_header *>(d->data);
//      logger_info("client: #", _async_connection->id(), " recv ", qw->count,
//                  " values to query #", qw->id);
//      auto subres = this->_query_results[qw->id];
//      assert(subres->is_ok);
//      if (qw->count == 0) {
//        subres->is_closed = true;
//        subres->clbk(subres.get(), Meas::empty());
//        subres->locker.unlock();
//        _query_results.erase(qw->id);
//      } else {
//        MeasArray ma = qw->read_measarray();
//        for (auto &v : ma) {
//          subres->clbk(subres.get(), v);
//        }
//      }
      break;
    }
    case dariadb::net::messages::QueryKind::PING: {
      logger_info("client: #", _async_connection->id(), " ping.");
      auto nd = _async_connection->get_pool()->construct(
          dariadb::net::messages::PONG, _async_connection->id());
      this->_async_connection->send(nd);
      _pings_answers++;
      break;
    }
    case dariadb::net::messages::QueryKind::DISCONNECT: {
      cancel = true;
      logger_info("client: #", _async_connection->id(), " disconnection.");
      try {
        _state = CLIENT_STATE::DISCONNECTED;
        this->_async_connection->full_stop();
        this->_socket->close();
      } catch (...) {
      }
      logger_info("client: #", _async_connection->id(), " disconnected.");
      break;
    }

    // hello id
    case dariadb::net::messages::QueryKind::HELLO: {
      auto id = qhdr.id();
      this->_async_connection->set_id(id);
      this->_state = CLIENT_STATE::WORK;
      logger_info("client: #", id, " ready.");
      break;
    }
    default:
      logger_fatal("client: unknow query kind - ", (int)kind);
      break;
    }
  }

  size_t pings_answers() const { return _pings_answers.load(); }
  CLIENT_STATE state() const { return _state; }

  void append(const MeasArray &ma) {
      if(ma.empty()){
          return;
      }
    this->_locker.lock();
    logger_info("client: send ", ma.size());
    std::list<ReadResult_ptr> results;

    auto cur_id = _query_num;
    _query_num += 1;

    auto byId=splitById(ma);
    dariadb::net::messages::QueryHeader qhdr;
    qhdr.set_id(cur_id);
    qhdr.set_kind(dariadb::net::messages::QueryKind::APPEND);
    //auto sz_hdr=qhdr.ByteSize();

    dariadb::net::messages::QueryAppend *qap=qhdr.MutableExtension(dariadb::net::messages::QueryAppend::qappend);
    size_t total_count=0;
    size_t count_to_write=0;
    for(auto kv:byId){
        auto var=qap->add_values();
        var->set_id(kv.first);
        for(auto v: kv.second){
            count_to_write++;
            auto data=var->add_data();
            //auto d_s=data->ByteSize();
            data->set_flag(v.flag);
            data->set_time(v.time);
            data->set_value(v.value);
            auto total_size=size_t(qhdr.ByteSize());// size_t(sz_hdr+d_s+qap->ByteSize());
            if(total_size >= size_t(NetData::MAX_MESSAGE_SIZE*0.75)){
                logger_info("client: pack count: ", count_to_write);
                //qhdr.set_submessage(qap.SerializeAsString());
                auto nd = this->_pool.construct();
                nd->size =  NetData::MAX_MESSAGE_SIZE-1;
                if(!qhdr.SerializeToArray(nd->data, nd->size)){
                    THROW_EXCEPTION("append message serialize error");
                }

                nd->size=qhdr.ByteSize();

                auto qres = std::make_shared<ReadResult>();
                qres->id = cur_id;
                qres->kind = dariadb::net::messages::QueryKind::APPEND;
                results.push_back(qres);
                this->_query_results[qres->id] = qres;

                 _async_connection->send(nd);
                 qhdr.Clear();

                cur_id = _query_num;
                _query_num += 1;

                qhdr.set_id(cur_id);
                qhdr.set_kind(dariadb::net::messages::QueryKind::APPEND);
                qap=qhdr.MutableExtension(dariadb::net::messages::QueryAppend::qappend);
                total_count+=count_to_write;
                count_to_write=0;
            }
        }
    }

    if(count_to_write>0){
        logger_info("client: pack count: ", count_to_write);
        auto nd = this->_pool.construct();
        nd->size =  NetData::MAX_MESSAGE_SIZE-1;
        if(!qhdr.SerializeToArray(nd->data, nd->size)){
            THROW_EXCEPTION("append message serialize error");
        }
        nd->size=qhdr.ByteSize();
        auto qres = std::make_shared<ReadResult>();
        qres->id = cur_id;
        qres->kind = dariadb::net::messages::QueryKind::APPEND;
        results.push_back(qres);
        this->_query_results[qres->id] = qres;

         _async_connection->send(nd);
         total_count+=count_to_write;
    }

    if(total_count!=ma.size()){
        THROW_EXCEPTION("logic error: total_count-",total_count, " ma-",ma.size());
    }

    this->_locker.unlock();
    for (auto&r : results) {
        while (!r->is_ok && !r->is_error) {
            std::this_thread::yield();
        }
        this->_locker.lock();
        this->_query_results.erase(r->id);
        this->_locker.unlock();
    }
//    while (writed != ma.size()) {
//      auto cur_id = _query_num;
//      _query_num += 1;
//      auto left = (ma.size() - writed);

//      auto qres = std::make_shared<ReadResult>();
//      qres->id = cur_id;
//      qres->kind = DATA_KINDS::APPEND;
//      results.push_back(qres);
//      this->_query_results[qres->id] = qres;

//      auto cur_msg_space = (NetData::MAX_MESSAGE_SIZE - 1 - sizeof(QueryAppend_header));
//      size_t count_to_write =
//          (left * sizeof(Meas)) > cur_msg_space ? cur_msg_space / sizeof(Meas) : left;
//      logger_info("client: pack count: ", count_to_write);

//      auto size_to_write = count_to_write * sizeof(Meas);

//      auto nd = this->_pool.construct(DATA_KINDS::APPEND);
//      nd->size = sizeof(QueryAppend_header);

//      auto hdr = reinterpret_cast<QueryAppend_header *>(&nd->data);
//      hdr->id = cur_id;
//      hdr->count = static_cast<uint32_t>(count_to_write);

//      auto meas_ptr = ((char *)(&hdr->count) + sizeof(hdr->count));
//      memcpy(meas_ptr, ma.data() + writed, size_to_write);
//      nd->size += static_cast<NetData::MessageSize>(size_to_write);

//      _async_connection->send(nd);
//      writed += count_to_write;
//    }
//    this->_locker.unlock();
//    for (auto&r : results) {
//        while (!r->is_ok && !r->is_error) {
//            std::this_thread::yield();
//        }
//        this->_locker.lock();
//        this->_query_results.erase(r->id);
//        this->_locker.unlock();
//    }
  }

  ReadResult_ptr readInterval(const storage::QueryInterval &qi, ReadResult::callback &clbk) {
//    _locker.lock();
//    auto cur_id = _query_num;
//    _query_num += 1;
//    _locker.unlock();

//    auto qres = std::make_shared<ReadResult>();
//    qres->locker.lock();
//    qres->id = cur_id;
//	qres->kind = DATA_KINDS::READ_INTERVAL;

//    auto nd = this->_pool.construct(DATA_KINDS::READ_INTERVAL);

//    auto p_header = reinterpret_cast<QueryInterval_header *>(nd->data);
//    nd->size = sizeof(QueryInterval_header);
//    p_header->id = cur_id;
//    p_header->flag = qi.flag;
//    p_header->from = qi.from;
//    p_header->to = qi.to;

//    auto id_size = sizeof(Id) * qi.ids.size();
//    if ((id_size + nd->size) > NetData::MAX_MESSAGE_SIZE) {
//      _pool.free(nd);
//      THROW_EXCEPTION("client: query to big");
//    }
//    p_header->ids_count = (uint16_t)(qi.ids.size());
//    auto ids_ptr = ((char *)(&p_header->ids_count) + sizeof(p_header->ids_count));
//    memcpy(ids_ptr, qi.ids.data(), id_size);
//    nd->size += static_cast<NetData::MessageSize>(id_size);

//    qres->is_closed = false;
//    qres->clbk = clbk;
//    this->_query_results[qres->id] = qres;

//	_async_connection->send(nd);
//    return qres;
      return nullptr;
  }

  MeasList readInterval(const storage::QueryInterval &qi) {
//    MeasList result{};
//    auto clbk_lambda = [&result](const ReadResult *parent, const Meas &m) {
//      if (!parent->is_closed) {
//        result.push_back(m);
//      }
//    };
//    ReadResult::callback clbk = clbk_lambda;
//    auto qres = readInterval(qi, clbk);
//    qres->wait();
//    return result;
      return MeasList{};
  }

  ReadResult_ptr readTimePoint(const storage::QueryTimePoint &qi, ReadResult::callback &clbk) {
//    _locker.lock();
//    auto cur_id = _query_num;
//    _query_num += 1;
//    _locker.unlock();

//    auto qres = std::make_shared<ReadResult>();
//    qres->locker.lock();
//    qres->id = cur_id;
//	qres->kind = DATA_KINDS::READ_TIMEPOINT;

//    auto nd = this->_pool.construct(DATA_KINDS::READ_TIMEPOINT);

//    auto p_header = reinterpret_cast<QueryTimePoint_header *>(nd->data);
//    nd->size = sizeof(QueryTimePoint_header);
//    p_header->id = cur_id;
//    p_header->flag = qi.flag;
//    p_header->tp = qi.time_point;

//    auto id_size = sizeof(Id) * qi.ids.size();
//    if ((id_size + nd->size) > NetData::MAX_MESSAGE_SIZE) {
//      _pool.free(nd);
//      THROW_EXCEPTION("client: query to big");
//    }
//    p_header->ids_count = (uint16_t)(qi.ids.size());
//    auto ids_ptr = ((char *)(&p_header->ids_count) + sizeof(p_header->ids_count));
//    memcpy(ids_ptr, qi.ids.data(), id_size);
//    nd->size += static_cast<NetData::MessageSize>(id_size);

//    qres->is_closed = false;
//    qres->clbk = clbk;
//    this->_query_results[qres->id] = qres;

//	_async_connection->send(nd);
//    return qres;
      return nullptr;
  }

  Id2Meas readTimePoint(const storage::QueryTimePoint &qi) {
//    Id2Meas result{};
//    auto clbk_lambda = [&result](const ReadResult *parent, const Meas &m) {
//      if (!parent->is_closed) {
//        result[m.id] = m;
//      }
//    };
//    ReadResult::callback clbk = clbk_lambda;
//    auto qres = readTimePoint(qi, clbk);
//    qres->wait();
//    return result;
      return Id2Meas{};
  }

  ReadResult_ptr currentValue(const IdArray &ids, const Flag &flag, ReadResult::callback &clbk) {
//	  _locker.lock();
//	  auto cur_id = _query_num;
//	  _query_num += 1;
//	  _locker.unlock();

//	  auto qres = std::make_shared<ReadResult>();
//	  qres->locker.lock();
//	  qres->id = cur_id;
//	  qres->kind = DATA_KINDS::CURRENT_VALUE;

//	  auto nd = this->_pool.construct(DATA_KINDS::CURRENT_VALUE);

//	  auto p_header = reinterpret_cast<QueryCurrentValue_header *>(nd->data);
//	  nd->size = sizeof(QueryCurrentValue_header);
//	  p_header->id = cur_id;
//	  p_header->flag = flag;

//	  auto id_size = sizeof(Id) * ids.size();
//	  if ((id_size + nd->size) > NetData::MAX_MESSAGE_SIZE) {
//		  _pool.free(nd);
//          THROW_EXCEPTION("client: query to big");
//	  }
//	  p_header->ids_count = (uint16_t)(ids.size());
//	  auto ids_ptr = ((char *)(&p_header->ids_count) + sizeof(p_header->ids_count));
//	  memcpy(ids_ptr, ids.data(), id_size);
//	  nd->size += static_cast<NetData::MessageSize>(id_size);

//	  qres->is_closed = false;
//	  qres->clbk = clbk;
//	  this->_query_results[qres->id] = qres;

//	  _async_connection->send(nd);
//	  return qres;
      return nullptr;
  }
  
  Id2Meas currentValue(const IdArray &ids, const Flag &flag) {
//	  Id2Meas result{};
//	  auto clbk_lambda = [&result](const ReadResult *parent, const Meas &m) {
//		  if (!parent->is_closed) {
//			  result[m.id] = m;
//		  }
//	  };
//	  ReadResult::callback clbk = clbk_lambda;
//	  auto qres = currentValue(ids, flag, clbk);
//	  qres->wait();
//	  return result;
      return Id2Meas{};
  }

  ReadResult_ptr subscribe(const IdArray &ids, const Flag &flag, ReadResult::callback &clbk) {
//	  _locker.lock();
//	  auto cur_id = _query_num;
//	  _query_num += 1;
//	  _locker.unlock();

//	  auto qres = std::make_shared<ReadResult>();
//	  qres->locker.lock();
//	  qres->id = cur_id;
//	  qres->kind = DATA_KINDS::SUBSCRIBE;

//	  auto nd = this->_pool.construct(DATA_KINDS::SUBSCRIBE);

//	  auto p_header = reinterpret_cast<QuerSubscribe_header *>(nd->data);
//	  nd->size = sizeof(QuerSubscribe_header);
//	  p_header->id = cur_id;
//	  p_header->flag = flag;

//	  auto id_size = sizeof(Id) * ids.size();
//	  if ((id_size + nd->size) > NetData::MAX_MESSAGE_SIZE) {
//		  _pool.free(nd);
//          THROW_EXCEPTION("client: query to big");
//	  }
//	  p_header->ids_count = (uint16_t)(ids.size());
//	  auto ids_ptr = ((char *)(&p_header->ids_count) + sizeof(p_header->ids_count));
//	  memcpy(ids_ptr, ids.data(), id_size);
//	  nd->size += static_cast<NetData::MessageSize>(id_size);

//	  qres->is_closed = false;
//	  qres->clbk = clbk;
//	  this->_query_results[qres->id] = qres;

//	  _async_connection->send(nd);
//	  return qres;
      return nullptr;
  }
  io_service _service;
  socket_ptr _socket;
  streambuf buff;

  Client::Param _params;
  utils::Locker _locker;
  std::thread _thread_handler;
  CLIENT_STATE _state;
  std::atomic_size_t _pings_answers;

  QueryNumber _query_num;
  MeasArray in_buffer_values;
  NetData_Pool _pool;
  std::map<QueryNumber, ReadResult_ptr> _query_results;
  std::shared_ptr<AsyncConnection> _async_connection;
};

Client::Client(const Param &p) : _Impl(new Client::Private(p)) {}

Client::~Client() {}

int Client::id() const {
  return _Impl->_async_connection->id();
}

void Client::connect() {
  _Impl->connect();
}

void Client::disconnect() {
  _Impl->disconnect();
}

CLIENT_STATE Client::state() const {
  return _Impl->state();
}

size_t Client::pings_answers() const {
  return _Impl->pings_answers();
}

void Client::append(const MeasArray &ma) {
  _Impl->append(ma);
}

MeasList Client::readInterval(const storage::QueryInterval &qi) {
  return _Impl->readInterval(qi);
}

ReadResult_ptr Client::readInterval(const storage::QueryInterval &qi,
                            ReadResult::callback &clbk) {
  return _Impl->readInterval(qi, clbk);
}

Id2Meas Client::readTimePoint(const storage::QueryTimePoint &qi) {
  return _Impl->readTimePoint(qi);
}

ReadResult_ptr Client::readTimePoint(const storage::QueryTimePoint &qi,
                            ReadResult::callback &clbk) {
  return _Impl->readTimePoint(qi, clbk);
}

ReadResult_ptr Client::currentValue(const IdArray &ids, const Flag &flag, ReadResult::callback &clbk) {
	return _Impl->currentValue(ids, flag,clbk);
}

Id2Meas Client::currentValue(const IdArray &ids, const Flag &flag) {
	return _Impl->currentValue(ids, flag);
}

ReadResult_ptr Client::subscribe(const IdArray &ids, const Flag &flag, ReadResult::callback &clbk) {
	return _Impl->subscribe(ids, flag, clbk);
}
