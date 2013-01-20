#include "mutex.hh"
#include "sched.hh"
#include "signal.hh"
#include <pthread.h>
#include <errno.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <string.h>
#include <list>
#include "debug.hh"

namespace pthread_private {

    const unsigned tsd_nkeys = 100;

    __thread void* tsd[tsd_nkeys];
    pthread_t current_pthread;

    mutex tsd_key_mutex;
    std::vector<bool> tsd_used_keys(tsd_nkeys);
    std::vector<void (*)(void*)> tsd_dtor(tsd_nkeys);

    struct pmutex {
        pmutex() : initialized(true) {}
        // FIXME: use a data structure which supports zero-init natively
        bool initialized; // for PTHREAD_MUTEX_INITIALIZER
        mutex mtx;
    };

    class pthread {
    public:
        explicit pthread(void *(*start)(void *arg), void *arg, sigset_t sigset);
        // FIXME: deallocate stack
        static pthread* from_libc(pthread_t p);
        pthread_t to_libc();
        void* _retval;
        sched::thread::stack_info _stack;
        // must be initialized last
        sched::thread _thread;
    private:
        sched::thread::stack_info allocate_stack();
    };

    struct thread_attr {
        void* stack_begin;
        size_t stack_size;
    };

    pthread::pthread(void *(*start)(void *arg), void *arg, sigset_t sigset)
        : _stack(allocate_stack())
        , _thread([=] {
                current_pthread = to_libc();
                sigprocmask(SIG_SETMASK, &sigset, nullptr);
                _retval = start(arg);
            }, _stack)
    {
    }

    sched::thread::stack_info pthread::allocate_stack()
    {
        size_t size = 1024*1024;
        return { new char[size], size };
    }

    pthread* pthread::from_libc(pthread_t p)
    {
        return reinterpret_cast<pthread*>(p);
    }

    static_assert(sizeof(thread_attr) <= sizeof(pthread_attr_t),
            "thread_attr too big");

    pthread_t pthread::to_libc()
    {
        return reinterpret_cast<pthread_t>(this);
    }

    thread_attr* from_libc(pthread_attr_t* a)
    {
        return reinterpret_cast<thread_attr*>(a);
    }

    const thread_attr* from_libc(const pthread_attr_t* a)
    {
        return reinterpret_cast<const thread_attr*>(a);
    }
}

using namespace pthread_private;

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine) (void *), void *arg)
{
    sigset_t sigset;
    sigprocmask(SIG_SETMASK, nullptr, &sigset);
    auto t = new pthread(start_routine, arg, sigset);
    *thread = t->to_libc();
    return 0;
}

int pthread_key_create(pthread_key_t* key, void (*dtor)(void*))
{
    std::lock_guard<mutex> guard(tsd_key_mutex);
    auto p = std::find(tsd_used_keys.begin(), tsd_used_keys.end(), false);
    if (p == tsd_used_keys.end()) {
        return ENOMEM;
    }
    *p = true;
    *key = p - tsd_used_keys.begin();
    tsd_dtor[*key] = dtor;
    return 0;
}

void* pthread_getspecific(pthread_key_t key)
{
    return tsd[key];
}

int pthread_setspecific(pthread_key_t key, const void* value)
{
    tsd[key] = const_cast<void*>(value);
    return 0;
}

pthread_t pthread_self()
{
    return current_pthread;
}

mutex* from_libc(pthread_mutex_t* m)
{
    auto p = reinterpret_cast<pmutex*>(m);
    if (!p->initialized) {
        new (p) pmutex;
    }
    return &p->mtx;
}

int pthread_mutex_init(pthread_mutex_t* __restrict m,
        const pthread_mutexattr_t* __restrict attr)
{
    // FIXME: respect attr
    new (m) pmutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    from_libc(m)->lock();
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    if (!from_libc(m)->try_lock()) {
        return -EBUSY;
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    from_libc(m)->unlock();
    return 0;
}

int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset)
{
    return sigprocmask(how, set, oldset);
}

class cond_var {
public:
    int wait(mutex* user_mutex, sched::timer* tmr = nullptr);
    void wake_one();
    void wake_all();
private:
    struct wait_record {
        explicit wait_record() : t(sched::thread::current()) {}
        sched::thread* t;
    };
private:
    mutex _mutex;
    std::list<wait_record*> _waiters;
};

cond_var* from_libc(pthread_cond_t* cond)
{
    return reinterpret_cast<cond_var*>(cond);
}

int cond_var::wait(mutex* user_mutex, sched::timer* tmr)
{
    int ret = 0;
    wait_record wr;
    with_lock(_mutex, [&] {
        _waiters.push_back(&wr);
    });
    user_mutex->unlock();
    sched::thread::wait_until([&] {
        return (tmr && tmr->expired())
                || with_lock(_mutex, [&] { return !wr.t; });
    });
    if (tmr && tmr->expired()) {
        with_lock(_mutex, [&] {
            auto p = std::find(_waiters.begin(), _waiters.end(), &wr); // FIXME: O(1)
            if (p != _waiters.end()) {
                _waiters.erase(p);
            }
        });
        ret = ETIMEDOUT;
    }
    user_mutex->lock();
    return ret;
}

void cond_var::wake_one()
{
    with_lock(_mutex, [&] {
        if (!_waiters.empty()) {
            auto wr = _waiters.front();
            _waiters.pop_front();
            auto t = wr->t;
            wr->t = nullptr;
            t->wake();
        }
    });
}

void cond_var::wake_all()
{
    with_lock(_mutex, [&] {
        while (!_waiters.empty()) {
            auto wr = _waiters.front();
            _waiters.pop_front();
            auto t = wr->t;
            wr->t = nullptr;
            t->wake();
        }
    });
}

int pthread_cond_init(pthread_cond_t *__restrict cond,
       const pthread_condattr_t *__restrict attr)
{
    new (cond) cond_var;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond)
{
    from_libc(cond)->~cond_var();
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    from_libc(cond)->wake_all();
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    from_libc(cond)->wake_one();
    return 0;
}

int pthread_cond_wait(pthread_cond_t *__restrict cond,
       pthread_mutex_t *__restrict mutex)
{
    return from_libc(cond)->wait(from_libc(mutex));
}

int pthread_cond_timedwait(pthread_cond_t *__restrict cond,
                           pthread_mutex_t *__restrict mutex,
                           const struct timespec* __restrict ts)
{
    sched::timer tmr(*sched::thread::current());
    tmr.set(u64(ts->tv_sec) * 1000000000 + ts->tv_nsec);
    return from_libc(cond)->wait(from_libc(mutex), &tmr);
}

int pthread_attr_init(pthread_attr_t *attr)
{
    new (attr) thread_attr;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr)
{
    auto t = pthread::from_libc(thread);
    auto a = from_libc(attr);
    a->stack_begin = t->_thread.get_stack_info().begin;
    a->stack_size = t->_thread.get_stack_info().size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    // ignore - we don't have processes, so it makes no difference
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    debug(fmt("pthread_attr_setstacksize(0x%x) stubbed out") % stacksize);
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
    debug(fmt("pthread_attr_setguardsize(0x%x) stubbed out") % guardsize);
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t * __restrict attr,
                                void **stackaddr, size_t *stacksize)
{
    auto a = from_libc(attr);
    *stackaddr = a->stack_begin;
    *stacksize = a->stack_size;
    return 0;
}

int pthread_setcancelstate(int state, int *oldstate)
{
    debug(fmt("pthread_setcancelstate stubbed out"));
    return 0;
}
