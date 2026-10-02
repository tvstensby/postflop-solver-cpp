#include "thread_pool.hpp"

#if PFS_ENABLE_THREADS

namespace pfs {

namespace {
// Slot 0 is the client thread; workers set this in worker_loop.
thread_local unsigned tls_worker_id = 0;
thread_local uint64_t tls_bcast_epoch = 0;
}  // namespace

unsigned ThreadPool::this_worker_id() noexcept { return tls_worker_id; }

ThreadPool::ThreadPool(unsigned num_threads)
    : workers_(std::make_unique<Worker[]>(std::max(num_threads, 1u))),
      num_workers_(std::max(num_threads, 1u)) {
    for (unsigned i = 0; i < num_workers_; ++i) workers_[i].rng = 0x9e3779b97f4a7c15ull * (i + 1);
    // Slot 0 is the calling thread, so only num_workers_ - 1 OS threads are spawned.
    for (unsigned i = 1; i < num_workers_; ++i)
        workers_[i].thread = std::thread([this, i] { worker_loop(i); });
}

ThreadPool::~ThreadPool() {
    shutdown_.store(true, std::memory_order_release);
    for (unsigned i = 1; i < num_workers_; ++i) workers_[i].sem.release();
    for (unsigned i = 1; i < num_workers_; ++i)
        if (workers_[i].thread.joinable()) workers_[i].thread.join();
}

ThreadPool& ThreadPool::global() {
    static ThreadPool pool(std::max(std::thread::hardware_concurrency(), 1u));
    return pool;
}

void ThreadPool::publish(Job& job) noexcept {
    Worker& me = workers_[job.owner];
    me.lock.lock();
    if (me.depth_ < kMaxJobDepth) me.stack_[me.depth_] = &job;
    ++me.depth_;
    me.lock.unlock();
}

void ThreadPool::unpublish(Job& job) noexcept {
    Worker& me = workers_[job.owner];
    me.lock.lock();
    --me.depth_;
    me.lock.unlock();
}

unsigned ThreadPool::pick_victim(Worker& me) noexcept {
    // xorshift64*
    uint64_t x = me.rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    me.rng = x;
    const unsigned self = this_worker_id();
    unsigned v = static_cast<unsigned>((x * 0x2545f4914f6cdd1dull) % num_workers_);
    if (v == self) v = (v + 1) % num_workers_;
    return v;
}

bool ThreadPool::run_one_chunk(Job& job) noexcept {
    const size_t lo = job.next.fetch_add(job.chunk, std::memory_order_acq_rel);
    if (lo >= job.end) return false;
    const size_t hi = std::min(lo + job.chunk, job.end);

    // Read `owner` before the final decrement: once pending hits zero the owner
    // may return and the Job (a stack object) ceases to exist.
    const unsigned owner = job.owner;

    try {
        job.invoke(job.fn, lo, hi);
    } catch (...) {
        bool expected = false;
        if (job.failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            job.eptr = std::current_exception();
    }

    if (job.pending.fetch_sub(1, std::memory_order_release) == 1) workers_[owner].sem.release();
    return true;  // NB: `job` may be dead from here on
}

bool ThreadPool::try_help_once(Worker& me) noexcept {
    Worker& vic = workers_[pick_victim(me)];

    Job* job = nullptr;
    size_t lo = 0, hi = 0;
    {
        // Claim the chunk while holding the victim's lock. That is what makes
        // touching a stack-resident job safe: unpublish takes the same lock and
        // only runs after pending reached zero, so if we got a real chunk here
        // then pending > 0 and the owner's frame is still alive for as long as we
        // need it.
        vic.lock.lock();
        if (vic.depth_ == 0 || vic.depth_ > kMaxJobDepth) {
            vic.lock.unlock();
            return false;
        }
        job = vic.stack_[vic.depth_ - 1];  // innermost: depth-first, best locality
        lo = job->next.fetch_add(job->chunk, std::memory_order_acq_rel);
        if (lo >= job->end) {
            vic.lock.unlock();
            return false;
        }
        hi = std::min(lo + job->chunk, job->end);
        vic.lock.unlock();
    }

    const unsigned owner = job->owner;
    try {
        job->invoke(job->fn, lo, hi);
    } catch (...) {
        bool expected = false;
        if (job->failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            job->eptr = std::current_exception();
    }
    if (job->pending.fetch_sub(1, std::memory_order_release) == 1) workers_[owner].sem.release();
    return true;
}

void ThreadPool::wake_some(size_t n) noexcept {
    if (sleepers_.load(std::memory_order_relaxed) == 0) return;
    size_t woken = 0;
    for (unsigned i = 0; i < num_workers_ && woken < n; ++i) {
        if (i == this_worker_id()) continue;
        bool expected = true;
        if (workers_[i].sleeping.compare_exchange_strong(expected, false,
                                                         std::memory_order_acq_rel)) {
            workers_[i].sem.release();
            ++woken;
        }
    }
}

void ThreadPool::drain_and_wait(Job& job) noexcept {
    Worker& me = workers_[this_worker_id()];
    for (;;) {
        // 1. Always try our own job first. This is the property that makes
        //    nesting deadlock-free: a thread only ever blocks when every chunk of
        //    its job is already claimed and executing somewhere.
        if (run_one_chunk(job)) continue;
        if (job.pending.load(std::memory_order_acquire) == 0) return;

        // 2. Help someone else while we wait.
        if (try_help_once(me)) continue;

        // 3. Spin briefly, then sleep with a timeout so a lost wake-up cannot
        //    stall us indefinitely.
        bool done = false;
        for (int s = 0; s < 64; ++s) {
            PFS_PAUSE();
            if (job.pending.load(std::memory_order_relaxed) == 0) {
                done = true;
                break;
            }
        }
        if (done) return;

        me.sleeping.store(true, std::memory_order_relaxed);
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        if (job.pending.load(std::memory_order_acquire) != 0)
            (void)me.sem.try_acquire_for(std::chrono::microseconds(200));
        sleepers_.fetch_sub(1, std::memory_order_relaxed);
        me.sleeping.store(false, std::memory_order_relaxed);
    }
}

void ThreadPool::worker_loop(unsigned id) noexcept {
    tls_worker_id = id;
    Worker& me = workers_[id];

    while (!shutdown_.load(std::memory_order_acquire)) {
        if (bcast_epoch_.load(std::memory_order_acquire) != tls_bcast_epoch) {
            tls_bcast_epoch = bcast_epoch_.load(std::memory_order_acquire);
            const std::function<void()>* fn = bcast_fn_;
            if (fn) {
                try {
                    (*fn)();
                } catch (...) {
                    // Arena teardown must not kill a worker.
                }
            }
            bcast_acks_.fetch_add(1, std::memory_order_release);
            continue;
        }

        if (try_help_once(me)) continue;

        me.sleeping.store(true, std::memory_order_relaxed);
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        (void)me.sem.try_acquire_for(std::chrono::microseconds(500));
        sleepers_.fetch_sub(1, std::memory_order_relaxed);
        me.sleeping.store(false, std::memory_order_relaxed);
    }
}

void ThreadPool::broadcast(const std::function<void()>& fn) {
    fn();  // the calling thread (slot 0) participates
    if (num_workers_ <= 1) return;

    bcast_fn_ = &fn;
    bcast_acks_.store(0, std::memory_order_relaxed);
    bcast_epoch_.fetch_add(1, std::memory_order_release);
    for (unsigned i = 1; i < num_workers_; ++i) {
        workers_[i].sleeping.store(false, std::memory_order_relaxed);
        workers_[i].sem.release();
    }
    while (bcast_acks_.load(std::memory_order_acquire) < num_workers_ - 1) PFS_PAUSE();
    bcast_fn_ = nullptr;
}

}  // namespace pfs

#endif  // PFS_ENABLE_THREADS
