
#include <chrono>
#include <spdlog/spdlog.h>

#include <rdmalib/connection.hpp>
#include <rdmalib/util.hpp>

namespace rdmalib {

  ConnectionConfiguration::ConnectionConfiguration()
  {
    memset(&attr, 0, sizeof(attr));
    memset(&conn_param, 0 , sizeof(conn_param));
  }

  Connection::Connection(bool passive):
    _id(nullptr),
    _qp(nullptr),
    _channel(nullptr),
    _req_count(0),
    _passive(passive)
  {
    inlining(false);

    for(int i=0; i < _rbatch; i++){
      _batch_wrs[i].wr_id = i;
      _batch_wrs[i].sg_list = 0;
      _batch_wrs[i].num_sge = 0;
      _batch_wrs[i].next=&(_batch_wrs[i+1]);
    }
    _batch_wrs[_rbatch-1].next = NULL;
 
  }

  Connection::~Connection()
  {
    close();
  }
 
  Connection::Connection(Connection&& obj):
    _id(obj._id),
    _qp(obj._qp),
    _channel(obj._channel),
    _req_count(obj._req_count),
    _passive(obj._passive)
  {
    obj._id = nullptr;
    obj._qp = nullptr;
    obj._req_count = 0;

    for(int i=0; i < _rbatch; i++){
      _batch_wrs[i].wr_id = i;
      _batch_wrs[i].sg_list = 0;
      _batch_wrs[i].num_sge = 0;
      _batch_wrs[i].next=&(_batch_wrs[i+1]);
    }
    _batch_wrs[_rbatch-1].next = NULL;
  }

  void Connection::initialize_batched_recv(const ScatterGatherElement & sge, size_t offset)
  {
    for(int i = 0; i < _rbatch; i++){
      _rwc_sges[i] = sge;
      for(auto & sg : _rwc_sges[i]._sges)
        sg.addr += i*offset;
      _batch_wrs[i].sg_list = _rwc_sges[i].array();
      _batch_wrs[i].num_sge = _rwc_sges[i].size();
    }
  }

  void Connection::initialize()
  {
    _channel = _id->recv_cq_channel;
  }

  void Connection::inlining(bool enable)
  {
    if(enable)
      _send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    else
      _send_flags = IBV_SEND_SIGNALED;
  }

  void Connection::close()
  {
    if(_id) {
      // When the connection is allocated on active side
      // We allocated ep, and that's the only thing we need to do
      if(!_passive)
        rdma_destroy_ep(_id);
      // When the connection is allocated on passive side
      // We allocated QP and we need to free an ID
      else {
        rdma_destroy_qp(_id);
        rdma_destroy_id(_id);
      }
      _id = nullptr;
    }
  }

  ibv_qp* Connection::qp() const
  {
    return this->_qp;
  }

  int32_t Connection::post_send(ScatterGatherElement && elems, int32_t id, bool force_inline)
  {
    // FIXME: extend with multiple sges
    struct ibv_send_wr wr, *bad;
    wr.wr_id = id == -1 ? _req_count++ : id;
    wr.next = nullptr;
    wr.sg_list = elems.array();
    wr.num_sge = elems.size();
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = force_inline ? IBV_SEND_SIGNALED | IBV_SEND_INLINE : _send_flags;

    int ret = ibv_post_send(_qp, &wr, &bad);
    if(ret) {
      spdlog::error("Post send unsuccesful, reason {} {}", errno, strerror(errno));
      return -1;
    }
    SPDLOG_DEBUG("Post send succesfull");
    return _req_count - 1;
  }

  int32_t Connection::post_batched_empty_recv(int count)
  {
    struct ibv_recv_wr* bad = nullptr;
    int loops = count / _rbatch;
    int reminder = count % _rbatch;

    int ret = 0;
    for(int i = 0; i < loops; ++i) {
      ret = ibv_post_recv(_qp, &_batch_wrs[0], &bad);
      if(ret)
        break;
    }

    if(ret == 0 && reminder > 0){
      _batch_wrs[reminder-1].next=NULL;
      ret = ibv_post_recv(_qp, _batch_wrs, &bad);
      _batch_wrs[reminder-1].next= &(_batch_wrs[reminder]);
    }

    if(ret) {
      spdlog::error("Batched Post empty recv  unsuccesful, reason {} {}", ret, strerror(ret));
      return -1;
    }

    SPDLOG_DEBUG("Batched Post empty recv succesfull");
    return count; 
  }

  int32_t Connection::post_recv(ScatterGatherElement && elem, int32_t id, int count)
  {
    // FIXME: extend with multiple sges

    struct ibv_recv_wr wr, *bad;
    wr.wr_id = id == -1 ? _req_count++ : id;
    wr.next = nullptr;
    wr.sg_list = elem.array();
    wr.num_sge = elem.size();

    int ret;
    for(int i = 0; i < count; ++i) {
      ret = ibv_post_recv(_qp, &wr, &bad);
      if(ret)
        break;
    }
    if(ret) {
      spdlog::error("Post receive unsuccesful, reason {} {}", ret, strerror(ret));
      return -1;
    }
    SPDLOG_DEBUG("Post recv succesfull");
    return wr.wr_id;
  }

  int32_t Connection::_post_write(ScatterGatherElement && elems, ibv_send_wr wr, bool force_inline)
  {
    ibv_send_wr* bad;
    wr.wr_id = _req_count++;
    wr.next = nullptr;
    wr.sg_list = elems.array();
    wr.num_sge = elems.size();
    wr.send_flags = force_inline ? IBV_SEND_SIGNALED | IBV_SEND_INLINE : _send_flags;

    if(wr.num_sge == 1 && wr.sg_list[0].length == 0)
      wr.num_sge = 0;

    int ret = ibv_post_send(_qp, &wr, &bad);
    if(ret) {
      spdlog::error("Post write unsuccesful, reason {} {}, sges_count {}, wr_id {}, remote addr {}, remote rkey {}, imm data {}",
        ret, strerror(ret), wr.num_sge, wr.wr_id,  wr.wr.rdma.remote_addr, wr.wr.rdma.rkey, ntohl(wr.imm_data)
      );
      return -1;
    }
    if(wr.num_sge > 0)
      SPDLOG_DEBUG(
          "Post write succesfull id: {}, sge size: {}, first lkey {} len {}, remote addr {}, remote rkey {}, imm data {}",
          wr.wr_id, wr.num_sge, wr.sg_list[0].lkey, wr.sg_list[0].length, wr.wr.rdma.remote_addr, wr.wr.rdma.rkey, ntohl(wr.imm_data)
      );
    else
      SPDLOG_DEBUG(
          "Post write succesfull id: {}, remote addr {}, remote rkey {}, imm data {}", wr.wr_id,  wr.wr.rdma.remote_addr, wr.wr.rdma.rkey, ntohl(wr.imm_data)
      );
    return _req_count - 1;

  }

  int32_t Connection::post_write(ScatterGatherElement && elems, const RemoteBuffer & rbuf, bool force_inline)
  {
    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.wr.rdma.remote_addr = rbuf.addr;
    wr.wr.rdma.rkey = rbuf.rkey;
    return _post_write(std::forward<ScatterGatherElement>(elems), wr, force_inline);
  }

  int32_t Connection::post_write(ScatterGatherElement && elems, const RemoteBuffer & rbuf, uint32_t immediate, bool force_inline)
  {
    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data = htonl(immediate);
    wr.wr.rdma.remote_addr = rbuf.addr;
    wr.wr.rdma.rkey = rbuf.rkey;
    return _post_write(std::forward<ScatterGatherElement>(elems), wr, force_inline);
  }

  int32_t Connection::post_cas(ScatterGatherElement && elems, const RemoteBuffer & rbuf, uint64_t compare, uint64_t swap)
  {
    ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = _req_count++;
    wr.next = nullptr;
    wr.sg_list = elems.array();
    wr.num_sge = elems.size();
    wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = rbuf.addr;
    wr.wr.atomic.rkey = rbuf.rkey;
    wr.wr.atomic.compare_add = compare;
    wr.wr.atomic.swap = swap;

    int ret = ibv_post_send(_qp, &wr, &bad);
    if(ret) {
      spdlog::error("Post write unsuccesful, reason {} {}", errno, strerror(errno));
      return -1;
    }
    SPDLOG_DEBUG("Post write succesfull");
    return _req_count - 1;
  }

  std::tuple<ibv_wc*, int> Connection::poll_wc(QueueType type, bool blocking)
  {
    int ret = 0;

    ibv_wc* wcs = (type == QueueType::RECV ? _rwc.data() : _swc.data());

    do {
      ret = ibv_poll_cq(type == QueueType::RECV ? _qp->recv_cq : _qp->send_cq, _wc_size, wcs);
    } while(blocking && ret == 0);
  
    if(ret < 0) {
      spdlog::error("Failure of polling events from: {} queue! Return value {}, errno {}", type == QueueType::RECV ? "recv" : "send", ret, errno);
      return std::make_tuple(nullptr, -1);
    }
    if(ret)
      for(int i = 0; i < ret; ++i)
        SPDLOG_DEBUG("Queue {} Ret {}/{} WC {} Status {}", type == QueueType::RECV ? "recv" : "send", i + 1, ret, wcs[i].wr_id, ibv_wc_status_str(wcs[i].status));
    return std::make_tuple(wcs, ret);
  }

  void Connection::notify_events()
  {
    impl::expect_zero(ibv_req_notify_cq(_qp->recv_cq, 0));
  }

  ibv_cq* Connection::wait_events()
  {
    ibv_cq* ev_cq = nullptr;
    void* ev_ctx = nullptr;
    impl::expect_zero(ibv_get_cq_event(_channel, &ev_cq, &ev_ctx));
    return ev_cq;
  }

  void Connection::ack_events(ibv_cq* cq, int len)
  {
    ibv_ack_cq_events(cq, len);
  }

}
