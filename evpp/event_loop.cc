#include "evpp/inner_pre.h"

#include "evpp/libevent_headers.h"
#include "evpp/libevent_watcher.h"
#include "evpp/event_loop.h"
#include "evpp/invoke_timer.h"

namespace evpp {
EventLoop::EventLoop()
    : create_evbase_myself_(true), pending_functor_count_(0) {
#if LIBEVENT_VERSION_NUMBER >= 0x02001500
    struct event_config* cfg = event_config_new();
    if (cfg) {
        // Does not cache time to get a preciser timer
        event_config_set_flag(cfg, EVENT_BASE_FLAG_NO_CACHE_TIME);
        evbase_ = event_base_new_with_config(cfg);
        event_config_free(cfg);
    }
#else
    evbase_ = event_base_new();
#endif
    Init();
}

EventLoop::EventLoop(struct event_base* base)
    : evbase_(base), create_evbase_myself_(false) {

    Init();

    // 当从一个已有的 event_base 创建EventLoop对象时，就不会调用 EventLoop::Run 方法，所以需要在这里调用 watcher_ 的初始化工作。
    InitEventWatcher();
}

void EventLoop::InitEventWatcher() {
    watcher_.reset(new PipeEventWatcher(this, std::bind(&EventLoop::DoPendingFunctors, this)));
    int rc = watcher_->Init();
    assert(rc);
    rc = rc && watcher_->AsyncWait();
    assert(rc);
    if (!rc) {
        LOG_FATAL << "PipeEventWatcher init failed.";
    }
}

EventLoop::~EventLoop() {
    if (!create_evbase_myself_) {
        assert(watcher_);
        watcher_.reset();
    }
    assert(!watcher_.get());

    if (evbase_ != NULL && create_evbase_myself_) {
        event_base_free(evbase_);
        evbase_ = NULL;
    }

    delete pending_functors_;
    pending_functors_ = NULL;
}

void EventLoop::Init() {
#ifdef H_HAVE_BOOST
    enum { kPendingFunctorCount = 1024 * 16 };
    this->pending_functors_ = new boost::lockfree::queue<Functor*>(kPendingFunctorCount);
#else
    this->pending_functors_ = new std::vector<Functor>();
#endif

    running_ = false;
    tid_ = std::this_thread::get_id(); // The default thread id
    calling_pending_functors_ = false;
}

void EventLoop::Run() {
    tid_ = std::this_thread::get_id(); // The actual thread id

    // 在当前的EventLoop线程中初始化
    InitEventWatcher();

    // 所有的事情都准备好之后，才置标记为true
    running_ = true;

    int rc = event_base_dispatch(evbase_);
    if (rc == 1) {
        LOG_ERROR << "event_base_dispatch error: no event registered";
    } else if (rc == -1) {
        int serrno = errno;
        LOG_ERROR << "event_base_dispatch error " << serrno << " " << strerror(serrno);
    }

    watcher_.reset(); // 确保在同一个线程构造、初始化和析构
    running_ = false;
    LOG_TRACE << "EventLoop stopped, tid: " << std::this_thread::get_id();
}


void EventLoop::Stop() {
    assert(running_);
    RunInLoop(std::bind(&EventLoop::StopInLoop, this));
}

void EventLoop::StopInLoop() {
    LOG_TRACE << "EventLoop is stopping now, tid=" << std::this_thread::get_id();
    assert(running_);
    for (;;) {
        DoPendingFunctors();

        std::lock_guard<std::mutex> lock(mutex_);

        if (pending_functors_->empty()) {
            break;
        }
    }

    timeval tv = Duration(0.5).TimeVal(); // 0.5 second
    event_base_loopexit(evbase_, &tv);
    running_ = false;
}

void EventLoop::AfterFork() {
    int rc = event_reinit(evbase_);
    assert(rc == 0);

    if (rc != 0) {
        fprintf(stderr, "event_reinit failed!\n");
        abort();
    }
}

InvokeTimerPtr EventLoop::RunAfter(double delay_ms, const Functor& f) {
    return RunAfter(Duration(delay_ms / 1000.0), f);
}

InvokeTimerPtr EventLoop::RunAfter(Duration delay, const Functor& f) {
    std::shared_ptr<InvokeTimer> t = InvokeTimer::Create(this, delay, f, false);
    t->Start();
    return t;
}

evpp::InvokeTimerPtr EventLoop::RunEvery(Duration interval, const Functor& f) {
    std::shared_ptr<InvokeTimer> t = InvokeTimer::Create(this, interval, f, true);
    t->Start();
    return t;
}

void EventLoop::RunInLoop(const Functor& functor) {
    if (IsInLoopThread()) {
        functor();
    } else {
        QueueInLoop(functor);
    }
}

void EventLoop::QueueInLoop(const Functor& cb) {
    {
#ifdef H_HAVE_BOOST
        auto f = new Functor(cb);
        while (!pending_functors_->push(f)) {
        }
#else
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_->emplace_back(cb);
#endif
    }
    ++pending_functor_count_;

    if (calling_pending_functors_ || !IsInLoopThread()) {
        watcher_->Notify();
    }
}

void EventLoop::DoPendingFunctors() {
    calling_pending_functors_ = true;

#ifdef H_HAVE_BOOST
    Functor* f = NULL;
    while (pending_functors_->pop(f)) {
        (*f)();
        delete f;
        --pending_functor_count_;
    }
#else
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_->swap(functors);
    }

    for (size_t i = 0; i < functors.size(); ++i) {
        functors[i]();
        --pending_functor_count_;
    }
#endif

    calling_pending_functors_ = false;
}

void EventLoop::AssertInLoopThread() const {
    assert(IsInLoopThread());
}
}
