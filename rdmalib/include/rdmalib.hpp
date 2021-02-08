
#include <cstdint>
#include <string>
#include <array>
#include <vector>
#include <optional>

// mmap
#include <sys/mman.h>

#include <rdma/rdma_cma.h>

namespace {

  void traceback();

  template<typename U>
  void expect_zero(U && u)
  {
    if(u) {
      spdlog::error("Expected zero, found: {}", u);
      traceback();
    }
    assert(!u);
  }

  template<typename U>
  void expect_nonzero(U && u)
  {
    if(!u) {
      spdlog::error("Expected non-zero, found: {}", u);
      traceback();
    }
    assert(u);
  }

  template<typename U>
  void expect_nonnull(U* ptr)
  {
    if(!ptr) {
      spdlog::error("Expected nonnull ptr");
      traceback();
    }
    assert(ptr);
  }

}

namespace rdmalib {

  namespace impl {

    // move non-template methods from header
    struct Buffer {
    protected:
      size_t _size;
      size_t _bytes;
      ibv_mr* _mr;
      void* _ptr;

      Buffer(size_t size, size_t byte_size);
      ~Buffer();
    public:
      uintptr_t ptr() const;
      size_t size() const;
      void register_memory(ibv_pd *pd, int access);
      uint32_t lkey() const;
      uint32_t rkey() const;
    };

  }

  template<typename T>
  struct Buffer : impl::Buffer{

    Buffer(size_t size):
      impl::Buffer(size, sizeof(T))
    {}

    T* data() const
    {
      return static_cast<T*>(this->_ptr);
    }
  };

  // Implemented as IPV4
  struct Address {
    rdma_addrinfo *addrinfo;
    rdma_addrinfo hints;
    uint16_t _port;

    Address(const std::string & ip, int port, bool passive);
    ~Address();
  };

  struct ConnectionConfiguration {
    // Configuration of QP
    ibv_qp_init_attr attr;
    rdma_conn_param conn_param;

    ConnectionConfiguration();
  };

  // State of a communication:
  // a) communication ID
  // b) Queue Pair
  struct Connection {
    rdma_cm_id* id;
    ibv_qp* qp; 

    Connection();
  };

  struct ScatterGatherElement {
    // smallvector in practice
    std::vector<ibv_sge> _sges;

    ScatterGatherElement();

    template<typename T>
    ScatterGatherElement(const Buffer<T> & buf)
    {
      add(buf);
    }

    template<typename T>
    void add(const Buffer<T> & buf)
    {
      //emplace_back for structs will be supported in C++20
      _sges.push_back({buf.ptr(), buf.size(), buf.lkey()});
    }

    ibv_sge * array();
    size_t size();
  };

  enum class QueueType{
    SEND,
    RECV
  };

  struct RDMAActive {
    ConnectionConfiguration _cfg;
    Connection _conn;
    Address _addr;
    rdma_event_channel * _ec;
    ibv_pd* _pd;
    int32_t _req_count;

    RDMAActive(const std::string & ip, int port);
    ~RDMAActive();
    void allocate();
    bool connect();
    ibv_qp* qp() const;
    ibv_pd* pd() const;

    int32_t post_recv(ScatterGatherElement && elem);
    int32_t _post_write(ScatterGatherElement && elems, ibv_send_wr wr);
    int32_t post_write(ScatterGatherElement && elems, uintptr_t addr, int rkey);
    int32_t post_write(ScatterGatherElement && elems, uintptr_t addr, int rkey, uint32_t immediate);
    int32_t post_atomics(ScatterGatherElement && elems, uintptr_t addr, int rkey, uint64_t add);
    // Blocking, no timeout
    ibv_wc poll_wc(QueueType);
  };

  struct RDMAPassive {
    ConnectionConfiguration _cfg;
    Address _addr;
    rdma_event_channel * _ec;
    rdma_cm_id* _listen_id;
    ibv_pd* _pd;
    std::vector<Connection> _connections;
    int32_t _req_count;

    RDMAPassive(const std::string & ip, int port);
    ~RDMAPassive();
    void allocate();
    ibv_pd* pd() const;
    std::optional<Connection> poll_events();

    // initializer list is not move aware but that shouldn't be a problem
    int32_t post_send(const Connection & conn, ScatterGatherElement && elem);
    int32_t post_recv(const Connection & conn, ScatterGatherElement && elem);
    ibv_wc poll_wc(const Connection & conn, QueueType);
  };

  //struct RDMAState {
  //  rdma_event_channel * _ec;
  //  rdma_cm_id * _id;

  //  RDMAState();
  //  ~RDMAState();

  //  RDMAListen listen(int port = 0) const;
  //  RDMAConnect connect() const;
  //};
}
