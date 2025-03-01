/*
 * Copyright (C) 2009-2019 Codership Oy <info@codership.com>
 */

/*!
 * @file GComm GCS Backend implementation
 *
 * @todo Figure out if there is lock-free way to handle RecvBuf
 *       push/pop operations.
 *
 */


#include "gcs_gcomm.hpp"

// We access data comp msg struct directly
#define GCS_COMP_MSG_ACCESS 1
#include "gcs_comp_msg.hpp"

#include <gcomm/transport.hpp>
#include <gcomm/util.hpp>
#include <gcomm/conf.hpp>

#include <gu_backtrace.hpp>
#include <gu_throw.hpp>
#include <gu_logger.hpp>
#include <gu_barrier.hpp>
#include <gu_thread.hpp>

#include <deque>

using namespace std;
using namespace gu;
using namespace gu::datetime;
using namespace gcomm;

static const std::string gcomm_thread_schedparam_opt("gcomm.thread_prio");

class RecvBufData
{
public:
    RecvBufData(const size_t source_idx,
                const Datagram& dgram,
                const ProtoUpMeta& um) :
        source_idx_(source_idx),
        dgram_     (dgram),
        um_        (um)
    { }

    size_t get_source_idx() const { return source_idx_; }
    const Datagram& get_dgram() const { return dgram_; }
    const ProtoUpMeta& get_um() const { return um_; }

private:
    size_t source_idx_;
    Datagram dgram_;
    ProtoUpMeta um_;
};

#if defined(GALERA_USE_BOOST_POOL_ALLOC)

#include <boost/pool/pool_alloc.hpp>

typedef deque<RecvBufData,
              boost::fast_pool_allocator<
                  RecvBufData,
                  boost::default_user_allocator_new_delete,
                  boost::details::pool::null_mutex
                  >
              >
#else

typedef deque<RecvBufData>

#endif /* GALERA_USE_BOOST_POOL_ALLOC */

RecvBufQueue;

class RecvBuf
{
private:

    class Waiting
    {
    public:
        Waiting (bool& w) : w_(w) { w_ = true;  }
        ~Waiting()                { w_ = false; }
    private:
        bool& w_;
    };

public:

    RecvBuf()
        :
#ifdef HAVE_PSI_INTERFACE
        mutex_(WSREP_PFS_INSTR_TAG_RECVBUF_MUTEX),
        cond_(WSREP_PFS_INSTR_TAG_RECVBUF_CONDVAR),
#else
        mutex_(),
        cond_(),
#endif /* HAVE_PSI_INTERFACE */
        queue_(), waiting_(false) { }

    void push_back(const RecvBufData& p)
    {
        Lock lock(mutex_);

        queue_.push_back(p);

        if (waiting_ == true) { cond_.signal(); }
    }

    const RecvBufData& front(const Date& timeout)
    {
        Lock lock(mutex_);

        while (queue_.empty())
        {
            Waiting w(waiting_);
            if (gu_likely (timeout == GU_TIME_ETERNITY))
            {
                lock.wait(cond_);
            }
            else
            {
                lock.wait(cond_, timeout);
            }
        }
        assert (false == waiting_);

        return queue_.front();
    }

    void pop_front()
    {
        Lock lock(mutex_);
        assert(queue_.empty() == false);
        queue_.pop_front();
    }

private:

#ifdef HAVE_PSI_INTERFACE
    gu::MutexWithPFS mutex_;
    gu::CondWithPFS cond_;
#else
    gu::Mutex mutex_;
    gu::Cond cond_;
#endif /* HAVE_PSI_INTERFACE */
    RecvBufQueue queue_;
    bool waiting_;
};

class GCommConn : public Toplay
{
public:

    GCommConn(const URI& u, gu::Config& cnf) :
        Toplay(cnf),
        conf_(cnf),
        uuid_(),
        thd_(),
        schedparam_(conf_.get(gcomm_thread_schedparam_opt)),
        barrier_(2),
        uri_(u),
        net_(Protonet::create(conf_)),
        tp_(0),
#ifdef HAVE_PSI_INTERFACE
        mutex_(WSREP_PFS_INSTR_TAG_GCOMMCONN_MUTEX),
#else
        mutex_(),
#endif /* HAVE_PSI_INTERFACE */
        refcnt_(0),
        terminated_(false),
        error_(0),
        recv_buf_(),
        current_view_()
    {
        log_debug << "backend: " << net_->type();
    }

    ~GCommConn()
    {
        // We cannot call gcs_close() to perform cleanup if there is an
        // exception. So, we need to explicity free the objects in the
        // destructor.
        if (tp_ != NULL)
        {
            delete tp_;
        }

        bool unlock_needed = true;
        mutex_.lock();
        if (!terminated_)
        {
            {
                gcomm::Critical<Protonet> crit(*net_);
                log_info << "gcomm: terminating thread";
                terminated_ = true;
                notify();
            }
            mutex_.unlock();
            unlock_needed = false;
            log_info << "gcomm: joining thread";
            gu_thread_join(thd_, 0);
        }
        if (unlock_needed)
        {
            mutex_.unlock();
        }
        delete net_;
    }

    const gcomm::UUID& get_uuid() const { return uuid_; }

    void connect(bool) { }

    void connect(const string& channel, bool const bootstrap);

    void close(bool force = false)
    {
        if (tp_ == 0)
        {
            log_warn << "gcomm: backend already closed";
            return;
        }
        {
            gcomm::Critical<Protonet> crit(*net_);
            log_info << "gcomm: terminating thread";
            terminate();
        }
        log_info << "gcomm: joining thread";
        pthread_join(thd_, 0);
        {
            gcomm::Critical<Protonet> crit(*net_);
            if (tp_ == 0)
            {
                log_info << "gcomm: backend closed already";
            }
            else
            {
                log_info << "gcomm: closing backend";
                tp_->close(error_ != 0 || force == true);
                gcomm::disconnect(tp_, this);
                delete tp_;
                tp_ = 0;
            }
        }
        log_info << "gcomm: closed";
    }

    void run();

    void notify() { net_->interrupt(); }

    void terminate()
    {
        Lock lock(mutex_);
        terminated_ = true;
        net_->interrupt();
    }

    void handle_up     (const void*        id,
                        const Datagram&    dg,
                        const ProtoUpMeta& um);

    RecvBuf&    get_recv_buf()            { return recv_buf_; }
    size_t      get_mtu()           const
    {
        if (tp_ == 0)
        {
            gu_throw_fatal << "GCommConn::get_mtu(): "
                           << "backend connection not open";
        }
        return tp_->mtu();
    }

    Protonet&   get_pnet()                { return *net_; }
    gu::Config& get_conf()                { return conf_; }
    int         get_error() const         { return error_; }

    void        get_status(gu::Status& status) const
    {
        if (tp_ != 0) tp_->get_status(status);
    }

    gu::ThreadSchedparam schedparam() const { return schedparam_; }

    class Ref
    {
    public:

        Ref(gcs_backend_t* ptr, bool unset = false) : conn_(0)
        {
            if (ptr->conn != 0)
            {
                conn_ = reinterpret_cast<GCommConn*>(ptr->conn)->ref(unset);

                if (unset == true)
                {
                    ptr->conn = 0;
                }
            }
        }

        ~Ref()
        {
            if (conn_ != 0)
            {
                conn_->unref();
            }
        }

        GCommConn* get() { return conn_; }

    private:

        Ref(const Ref&);
        void operator=(const Ref&);

        GCommConn* conn_;
    };

private:

    GCommConn(const GCommConn&);
    void operator=(const GCommConn&);

    GCommConn* ref(const bool unsetting)
    {
        return this;
    }

    void unref() { }

    gu::Config&       conf_;
    gcomm::UUID       uuid_;
    pthread_t         thd_;
    ThreadSchedparam  schedparam_;
    Barrier           barrier_;
    URI               uri_;
    Protonet*         net_;
    Transport*        tp_;
#ifdef HAVE_PSI_INTERFACE
    gu::MutexWithPFS  mutex_;
#else
    gu::Mutex         mutex_;
#endif /* HAVE_PSI_INTERFACE */
    size_t            refcnt_;
    bool              terminated_;
    int               error_;
    RecvBuf           recv_buf_;
    View              current_view_;
};

extern "C"
void* run_fn(void* arg)
{
#ifdef HAVE_PSI_INTERFACE
    pfs_instr_callback(WSREP_PFS_INSTR_TYPE_THREAD,
                       WSREP_PFS_INSTR_OPS_INIT,
                       WSREP_PFS_INSTR_TAG_GCOMMCONN_THREAD,
                       NULL, NULL, NULL);
#endif /* HAVE_PSI_INTERFACE */

    static_cast<GCommConn*>(arg)->run();

#ifdef HAVE_PSI_INTERFACE
    pfs_instr_callback(WSREP_PFS_INSTR_TYPE_THREAD,
                       WSREP_PFS_INSTR_OPS_DESTROY,
                       WSREP_PFS_INSTR_TAG_GCOMMCONN_THREAD,
                       NULL, NULL, NULL);
#endif /* HAVE_PSI_INTERFACE */
    gu_thread_exit(0);
}

void GCommConn::connect(const string& channel, bool const bootstrap)
{
    if (tp_ != 0)
    {
        gu_throw_fatal << "backend connection already open";
    }


    error_ = ENOTCONN;
    int err;
    if ((err = gu_thread_create(
             &thd_, 0, run_fn, this)) != 0)
    {
        gu_throw_error(err) << "Failed to create thread";
    }

    // Helper to call barrier_.wait() when goes out of scope
    class StartBarrier
    {
    public:
        StartBarrier(Barrier& barrier) : barrier_(barrier) { }
        ~StartBarrier()
        {
            barrier_.wait();
        }
    private:
        Barrier& barrier_;
    } start_barrier(barrier_);

    thread_set_schedparam(thd_, schedparam_);
    log_info << "gcomm thread scheduling priority set to "
             << thread_get_schedparam(thd_) << " ";

    uri_.set_option("gmcast.group", channel);
    tp_ = Transport::create(*net_, uri_);
    gcomm::connect(tp_, this);

    if (bootstrap)
    {
        log_info << "gcomm: bootstrapping new group '" << channel << '\'';
    }
    else
    {
        string peer;
        URI::AuthorityList::const_iterator i, i_next;
        for (i = uri_.get_authority_list().begin();
             i != uri_.get_authority_list().end(); ++i)
        {
            i_next = i;
            ++i_next;
            string host;
            string port;
            try { host = i->host(); } catch (NotSet&) { }
            try { port = i->port(); } catch (NotSet&) { }
            peer += host != "" ? host + ":" + port : "";
            if (i_next != uri_.get_authority_list().end())
            {
                peer += ",";
            }
        }
        log_info << "gcomm: connecting to group '" << channel
                 << "', peer '" << peer << "'";
    }

    tp_->connect(bootstrap);

    uuid_ = tp_->uuid();

    error_ = 0;

    log_info << "gcomm: connected";
}

void
GCommConn::handle_up(const void* id, const Datagram& dg, const ProtoUpMeta& um)
{
    if (um.err_no() != 0)
    {
        error_ = um.err_no();
        // force backend close
        close(true);
        recv_buf_.push_back(RecvBufData(numeric_limits<size_t>::max(), dg, um));
    }
    else if (um.has_view() == true)
    {
        current_view_ = um.view();
        recv_buf_.push_back(RecvBufData(numeric_limits<size_t>::max(), dg, um));
        if (current_view_.is_empty())
        {
            log_debug << "handle_up: self leave";
        }
    }
    else
    {
        size_t idx(0);
        for (NodeList::const_iterator i = current_view_.members().begin();
             i != current_view_.members().end(); ++i)
        {
            if (NodeList::key(i) == um.source())
            {
                recv_buf_.push_back(RecvBufData(idx, dg, um));
                break;
            }
            ++idx;
        }
        assert(idx < current_view_.members().size());
    }
}

void GCommConn::run()
{
    barrier_.wait();
    if (error_ != 0) return;

    while (true)
    {
        {
            Lock lock(mutex_);

            if (terminated_ == true)
            {
                break;
            }
        }

        try
        {
            net_->event_loop(Sec);
        }
        catch (gu::Exception& e)
        {
            log_error << "exception from gcomm, backend must be restarted: "
                      << e.what();
            // Commented out due to Backtrace() not producing proper
            // backtraces.
            // log_info << "attempting to get backtrace:";
            // Backtrace().print(std::cerr);
            gcomm::Critical<Protonet> crit(get_pnet());
            handle_up(0, Datagram(),
                      ProtoUpMeta(gcomm::UUID::nil(),
                                  ViewId(V_NON_PRIM),
                                  0,
                                  0xff,
                                  O_DROP,
                                  -1,
                                  e.get_errno()));
            break;
        }
#if 0
        // Disabled catching unknown exceptions due to Backtrace() not
        // producing proper backtraces. We let the application crash
        // and deal with diagnostics.
        catch (...)
        {
            log_error
                << "unknow exception from gcomm, backend must be restarted";
            log_info << "attempting to get backtrace:";
            Backtrace().print(std::cerr);

            gcomm::Critical<Protonet> crit(get_pnet());
            handle_up(0, Datagram(),
                      ProtoUpMeta(gcomm::UUID::nil(),
                                  ViewId(V_NON_PRIM),
                                  0,
                                  0xff,
                                  O_DROP,
                                  -1,
                                  gu::Exception::E_UNSPEC));
            break;
        }
#endif
    }
}


////////////////////////////////////////////////////////////////////////////
//
//                  Backend interface implementation
//
////////////////////////////////////////////////////////////////////////////


static GCS_BACKEND_MSG_SIZE_FN(gcomm_msg_size)
{
    GCommConn::Ref ref(backend);
    if (ref.get() == 0)
    {
        return -1;
    }
    return ref.get()->get_mtu();
}


static GCS_BACKEND_SEND_FN(gcomm_send)
{
    GCommConn::Ref ref(backend);

    if (gu_unlikely(ref.get() == 0))
    {
        return -EBADFD;
    }

    GCommConn& conn(*ref.get());

    Datagram dg(
        SharedBuffer(
            new Buffer(reinterpret_cast<const byte_t*>(buf),
                       reinterpret_cast<const byte_t*>(buf) + len)));

    int err;
    // Set thread scheduling params if gcomm thread runs with
    // non-default params
    gu::ThreadSchedparam orig_sp;
    if (conn.schedparam() != gu::ThreadSchedparam::system_default)
    {
        try
        {
            orig_sp = gu::thread_get_schedparam(pthread_self());
            gu::thread_set_schedparam(pthread_self(), conn.schedparam());
        }
        catch (gu::Exception& e)
        {
            err = e.get_errno();
        }
    }


    {
        gcomm::Critical<Protonet> crit(conn.get_pnet());
        if (gu_unlikely(conn.get_error() != 0))
        {
            err = ECONNABORTED;
        }
        else
        {
            err = conn.send_down(
                dg,
                ProtoDownMeta(msg_type, msg_type == GCS_MSG_CAUSAL ?
                              O_LOCAL_CAUSAL : O_SAFE));
        }
    }

    if (conn.schedparam() != gu::ThreadSchedparam::system_default)
    {
        try
        {
            gu::thread_set_schedparam(pthread_self(), orig_sp);
        }
        catch (gu::Exception& e)
        {
            err = e.get_errno();
        }
    }

    return (err == 0 ? len : -err);
}


static void fill_cmp_msg(const View& view, const gcomm::UUID& my_uuid,
                         gcs_comp_msg_t* cm)
{
    size_t n(0);

    for (NodeList::const_iterator i = view.members().begin();
         i != view.members().end(); ++i)
    {
        const gcomm::UUID& uuid(NodeList::key(i));

        log_debug << "member: " << n << " uuid: " << uuid
                  << " segment: " << static_cast<int>(i->second.segment());

//        (void)snprintf(cm->memb[n].id, GCS_COMP_MEMB_ID_MAX_LEN, "%s",
//                       uuid._str().c_str());
        long ret = gcs_comp_msg_add (cm, uuid.full_str().c_str(),
                                     i->second.segment());
        if (ret < 0) {
            gu_throw_error(-ret) << "Failed to add member '" << uuid
                                 << "' to component message.";
        }

        if (uuid == my_uuid)
        {
            log_debug << "my index " << n;
            cm->my_idx = n;
        }

        ++n;
    }
}

static GCS_BACKEND_RECV_FN(gcomm_recv)
{
    GCommConn::Ref ref(backend);

    if (gu_unlikely(ref.get() == 0)) return -EBADFD;

    try
    {
        GCommConn& conn(*ref.get());

        RecvBuf& recv_buf(conn.get_recv_buf());

        const RecvBufData& d(recv_buf.front(timeout));

        msg->sender_idx = d.get_source_idx();

        const Datagram&    dg(d.get_dgram());
        const ProtoUpMeta& um(d.get_um());

        if (gu_likely(dg.len() != 0))
        {
            assert(dg.len() > dg.offset());

            const byte_t* b(gcomm::begin(dg));
            const ssize_t pload_len(gcomm::available(dg));

            msg->size = pload_len;

            if (gu_likely(pload_len <= msg->buf_len))
            {
                memcpy(msg->buf, b, pload_len);
                msg->type = static_cast<gcs_msg_type_t>(um.user_type());
                recv_buf.pop_front();
            }
            else
            {
                msg->type = GCS_MSG_ERROR;
            }
        }
        else if (um.err_no() != 0)
        {
            gcs_comp_msg_t* cm(gcs_comp_msg_leave(ECONNABORTED));
            const ssize_t cm_size(gcs_comp_msg_size(cm));
            if (cm_size <= msg->buf_len)
            {
                memcpy(msg->buf, cm, cm_size);
                msg->size = cm_size;
                recv_buf.pop_front();
                msg->type = GCS_MSG_COMPONENT;
            }
            else
            {
                msg->type = GCS_MSG_ERROR;
            }
            gcs_comp_msg_delete(cm);
        }
        else
        {
            assert(um.has_view() == true);

            const View& view(um.view());

            assert(view.type() == V_PRIM || view.type() == V_NON_PRIM);

            gcs_comp_msg_t* cm(gcs_comp_msg_new(view.type() == V_PRIM,
                                                view.is_bootstrap(),
                                                view.is_empty() ? -1 : 0,
                                                view.members().size(), 0));

            const ssize_t cm_size(gcs_comp_msg_size(cm));

            if (cm->my_idx == -1)
            {
                log_debug << "gcomm recv: self leave";
            }

            msg->size = cm_size;

            if (gu_likely(cm_size <= msg->buf_len))
            {
                fill_cmp_msg(view, conn.get_uuid(), cm);
                memcpy(msg->buf, cm, cm_size);
                recv_buf.pop_front();
                msg->type = GCS_MSG_COMPONENT;
            }
            else
            {
                msg->type = GCS_MSG_ERROR;
            }

            gcs_comp_msg_delete(cm);
        }

        return msg->size;
    }
    catch (Exception& e)
    {
        long err = e.get_errno();

        if (ETIMEDOUT != err) { log_error << e.what(); }

        return -err;
    }
}


static GCS_BACKEND_NAME_FN(gcomm_name)
{
    static const char *name = "gcomm";
    return name;
}

static GCS_BACKEND_OPEN_FN(gcomm_open)
{
    GCommConn::Ref ref(backend);

    if (ref.get() == 0)
    {
        return -EBADFD;
    }

    GCommConn& conn(*ref.get());

    try
    {
        gcomm::Critical<Protonet> crit(conn.get_pnet());
        conn.connect(channel, bootstrap);
    }
    catch (Exception& e)
    {
        log_error << "failed to open gcomm backend connection: "
                  << e.get_errno() << ": "
                  << e.what();
        return -e.get_errno();
    }
    return 0;
}


static GCS_BACKEND_CLOSE_FN(gcomm_close)
{
    GCommConn::Ref ref(backend);

    if (ref.get() == 0)
    {
        return -EBADFD;
    }

    GCommConn& conn(*ref.get());
    try
    {
        // Critical section is entered inside close() call.
        // gcomm::Critical<Protonet> crit(conn.get_pnet());
        conn.close();
    }
    catch (Exception& e)
    {
        log_error << "failed to close gcomm backend connection: "
                  << e.get_errno() << ": " << e.what();
        gcomm::Critical<Protonet> crit(conn.get_pnet());
        conn.handle_up(0, Datagram(),
                       ProtoUpMeta(gcomm::UUID::nil(),
                                   ViewId(V_NON_PRIM),
                                   0,
                                   0xff,
                                   O_DROP,
                                   -1,
                                   e.get_errno()));
        // #661: Pretend that closing was successful, backend should be
        // in unusable state anyway. This allows gcs to finish shutdown
        // sequence properly.
    }

    return 0;
}


static GCS_BACKEND_DESTROY_FN(gcomm_destroy)
{
    GCommConn::Ref ref(backend, true);

    if (ref.get() == 0)
    {
        log_warn << "could not get reference to backend conn";
        return -EBADFD;
    }

    GCommConn* conn(ref.get());
    try
    {
        delete conn;
    }
    catch (Exception& e)
    {
        log_warn << "conn destroy failed: " << e.get_errno();
        return -e.get_errno();
    }

    return 0;
}


static
GCS_BACKEND_PARAM_SET_FN(gcomm_param_set)
{
    GCommConn::Ref ref(backend);
    if (ref.get() == 0)
    {
        return -EBADFD;
    }

    Protolay::sync_param_cb_t sync_param_cb;

    GCommConn& conn(*ref.get());
    try
    {
        gcomm::Critical<Protonet> crit(conn.get_pnet());
        if (gu_unlikely(conn.get_error() != 0))
        {
            return -ECONNABORTED;
        }

        if (conn.get_pnet().set_param(key, value, sync_param_cb) == false)
        {
            log_debug << "param " << key << " not recognized";
            return 1;
        }
    }
    catch (gu::Exception& e)
    {
        log_warn << "error setting param " << key << " to value " << value
                 << ": " << e.what();
        return -e.get_errno();
    }
    catch (gu::NotFound& nf)
    {
        log_warn << "error setting param " << key << " to value " << value;
        return -EINVAL;
    }
    catch (gu::NotSet& nf)
    {
        log_warn << "error setting param " << key << " to value " << value;
        return -EINVAL;
    }
    catch (...)
    {
        log_fatal << "gcomm param set: caught unknown exception";
        return -ENOTRECOVERABLE;
    }

    if (!sync_param_cb.empty()) 
    {
        sync_param_cb();
    }
  
    return 0;
}


static
GCS_BACKEND_PARAM_GET_FN(gcomm_param_get)
{
    return NULL;
}

static
GCS_BACKEND_STATUS_GET_FN(gcomm_status_get)
{
    GCommConn::Ref ref(backend);
    if (ref.get() == 0)
    {
        gu_throw_error(-EBADFD);
    }

    GCommConn& conn(*ref.get());
    gcomm::Critical<Protonet> crit(conn.get_pnet());
    conn.get_status(status);

}


GCS_BACKEND_REGISTER_FN(gcs_gcomm_register)
{
    try
    {
        reinterpret_cast<gu::Config*>(cnf)->add(gcomm_thread_schedparam_opt, "");
        gcomm::Conf::register_params(*reinterpret_cast<gu::Config*>(cnf));
        return false;
    }
    catch (...)
    {
        return true;
    }
}


GCS_BACKEND_CREATE_FN(gcs_gcomm_create)
{
    GCommConn* conn(0);

    if (!cnf)
    {
        log_error << "Null config object passed to constructor.";
        return -EINVAL;
    }

    try
    {
        gu::URI uri(std::string("pc://") + addr);
        gu::Config& conf(*reinterpret_cast<gu::Config*>(cnf));
        conn = new GCommConn(uri, conf);
    }
    catch (Exception& e)
    {
        log_error << "failed to create gcomm backend connection: "
                  << e.get_errno() << ": "
                  << e.what();
        return -e.get_errno();
    }

    backend->open      = gcomm_open;
    backend->close     = gcomm_close;
    backend->destroy   = gcomm_destroy;
    backend->send      = gcomm_send;
    backend->recv      = gcomm_recv;
    backend->name      = gcomm_name;
    backend->msg_size  = gcomm_msg_size;
    backend->param_set = gcomm_param_set;
    backend->param_get = gcomm_param_get;
    backend->status_get = gcomm_status_get;

    backend->conn      = reinterpret_cast<gcs_backend_conn_t*>(conn);

    return 0;
}
