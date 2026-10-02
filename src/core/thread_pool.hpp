// Replacement for rayon. The whole parallel surface of the Rust crate is
// `for_each_child` (parallel-for over a node's child actions) and
// `into_par_iter(range)` (flat precompute loops), so this only needs a blocking
// parallel-for plus a broadcast.
//
// Two facts from the Rust drive the design:
//
//  1. NESTING. `PostFlopNode::enable_parallelization()` is `river == NOT_DEALT`,
//     which is true for every flop and turn node -- player nodes included, not
//     just chance nodes. So parallel regions nest to roughly
//     2 * (flop depth + turn depth), i.e. 10-25 levels. A pool whose join parks
//     the owner thread idle deadlocks almost immediately: every worker ends up
//     asleep owning an outer region while the inner work it waits on has nobody
//     to run it. Therefore an owner NEVER waits for work it could run itself --
//     it claims from its own job on every loop iteration. Stealing exists purely
//     for load balance, so deadlock-freedom does not depend on it.
//
//  2. FREQUENCY. parallel_for is entered once per flop/turn interior node, i.e.
//     10^3-10^5 times per CFR iteration. The per-call budget is ~100-300ns, so
//     there is no room for a heap allocation, a std::function, or a queue push
//     per work item. A job is therefore a plain struct on the owner's stack
//     (parallel_for is blocking, so it outlives all its work) and work items are
//     claimed from an atomic counter.
#pragma once

#include <pfs/common.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>

#if PFS_ENABLE_THREADS
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <semaphore>
#include <thread>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#define PFS_PAUSE() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define PFS_PAUSE() _mm_pause()
#else
#define PFS_PAUSE() std::this_thread::yield()
#endif
#endif

namespace pfs {

#if PFS_ENABLE_THREADS

// Not std::hardware_destructive_interference_size: GCC warns about ABI-sensitive
// use of it in headers.
inline constexpr size_t kCacheLine = 64;
inline constexpr unsigned kMaxJobDepth = 64;  // >= observed nesting (~25)

class ThreadPool {
public:
    explicit ThreadPool(unsigned num_threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    static ThreadPool& global();

    unsigned num_threads() const noexcept { return num_workers_; }

    // Slot of the calling thread. Slot 0 is the client thread, bound on first use.
    static unsigned this_worker_id() noexcept;

    // fn(i) for every i in [begin, end). Blocks. Re-entrant: safe to call from a
    // worker, at any nesting depth.
    template <class Fn>
    void parallel_for(size_t begin, size_t end, const Fn& fn) {
        parallel_for_chunked(begin, end, 1, [&fn](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; ++i) fn(i);
        });
    }

    // fn(lo, hi) over consecutive chunks of at most `chunk` indices. This exists
    // because the bunching phases use rayon's `.step_by(100)` semantically, not
    // as a grain hint: the body unranks a combination once per chunk and then
    // Gosper-walks it, so flattening to grain 1 would be a large regression.
    template <class Fn>
    void parallel_for_chunked(size_t begin, size_t end, size_t chunk, const Fn& fn);

    // fn() exactly once on every pool thread, including the caller. Precondition:
    // no parallel_for is in flight, because this is used to free the per-thread
    // solver arenas and doing that under a live frame would be catastrophic.
    void broadcast(const std::function<void()>& fn);

private:
    struct alignas(kCacheLine) Job {
        void (*invoke)(const void*, size_t, size_t);
        const void* fn;
        size_t end;
        size_t chunk;
        std::atomic<size_t> next;     // next index to claim
        std::atomic<size_t> pending;  // chunks not yet completed
        unsigned owner;
        std::atomic<bool> failed;
        std::exception_ptr eptr;  // written once, guarded by the `failed` CAS
    };

    // ~20ns critical sections; std::mutex would take a syscall path under contention.
    class Spinlock {
    public:
        void lock() noexcept {
            while (flag_.test_and_set(std::memory_order_acquire))
                while (flag_.test(std::memory_order_relaxed)) PFS_PAUSE();
        }
        void unlock() noexcept { flag_.clear(std::memory_order_release); }

    private:
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };

    struct alignas(kCacheLine) Worker {
        Spinlock lock;                 // guards stack_ / depth_ only
        Job* stack_[kMaxJobDepth]{};   // innermost last
        unsigned depth_ = 0;
        std::counting_semaphore<1> sem{0};
        std::atomic<bool> sleeping{false};
        uint64_t rng = 0;
        std::thread thread;  // empty for slot 0
    };

    void publish(Job& job) noexcept;
    void unpublish(Job& job) noexcept;
    void drain_and_wait(Job& job) noexcept;
    bool run_one_chunk(Job& job) noexcept;
    bool try_help_once(Worker& me) noexcept;
    void wake_some(size_t n) noexcept;
    unsigned pick_victim(Worker& me) noexcept;
    void worker_loop(unsigned id) noexcept;

    std::unique_ptr<Worker[]> workers_;
    unsigned num_workers_ = 1;
    std::atomic<size_t> sleepers_{0};
    std::atomic<bool> shutdown_{false};

    // broadcast handshake
    const std::function<void()>* bcast_fn_ = nullptr;
    std::atomic<uint64_t> bcast_epoch_{0};
    std::atomic<size_t> bcast_acks_{0};
};

template <class Fn>
void ThreadPool::parallel_for_chunked(size_t begin, size_t end, size_t chunk, const Fn& fn) {
    static_assert(std::is_invocable_v<const Fn&, size_t, size_t>);
    if (begin >= end) return;

    const size_t n = (end - begin + chunk - 1) / chunk;
    if (num_workers_ == 1 || n == 1) {
        // Serial fast path: no atomics, no publish. Ascending order, matching
        // Rust's `Range::for_each`.
        for (size_t lo = begin; lo < end; lo += chunk) fn(lo, std::min(lo + chunk, end));
        return;
    }

    Job job;
    job.invoke = +[](const void* p, size_t lo, size_t hi) {
        (*static_cast<const Fn*>(p))(lo, hi);
    };
    job.fn = std::addressof(fn);
    job.end = end;
    job.chunk = chunk;
    job.next.store(begin, std::memory_order_relaxed);
    job.pending.store(n, std::memory_order_relaxed);
    job.owner = this_worker_id();
    job.failed.store(false, std::memory_order_relaxed);

    publish(job);
    wake_some(n - 1);
    drain_and_wait(job);
    unpublish(job);

    if (job.failed.load(std::memory_order_acquire)) std::rethrow_exception(job.eptr);
}

#else  // !PFS_ENABLE_THREADS

class ThreadPool {
public:
    static ThreadPool& global() {
        static ThreadPool p;
        return p;
    }
    static constexpr unsigned num_threads() noexcept { return 1; }
    static constexpr unsigned this_worker_id() noexcept { return 0; }

    template <class Fn>
    void parallel_for(size_t begin, size_t end, const Fn& fn) {
        for (size_t i = begin; i < end; ++i) fn(i);
    }
    template <class Fn>
    void parallel_for_chunked(size_t begin, size_t end, size_t chunk, const Fn& fn) {
        for (size_t lo = begin; lo < end; lo += chunk) fn(lo, std::min(lo + chunk, end));
    }
    template <class Fn>
    void broadcast(const Fn& fn) {
        fn();
    }
};

#endif

// Rust: `utility.rs::for_each_child`. Templated on the callable so there is no
// indirection or allocation per node -- see note 2 at the top of this file.
// The node is taken by non-const reference because the GameNode accessors are
// non-const (Rust reaches them through MutexLike, which hands out `&mut` from `&`).
template <class Node, class Op>
PFS_ALWAYS_INLINE void for_each_child(Node& node, const Op& op) {
    const size_t n = node.num_actions();
#if PFS_ENABLE_THREADS
    if (node.enable_parallelization() && n > 1) {
        ThreadPool::global().parallel_for(0, n, op);
        return;
    }
#endif
    for (size_t i = 0; i < n; ++i) op(i);
}

// Rust: `utility.rs::into_par_iter` -- the bunching phases and the PostFlopGame
// precompute loops.
template <class Fn>
inline void parallel_for_range(size_t begin, size_t end, const Fn& fn) {
    ThreadPool::global().parallel_for(begin, end, fn);
}

template <class Fn>
inline void parallel_for_chunks(size_t begin, size_t end, size_t chunk, const Fn& fn) {
    ThreadPool::global().parallel_for_chunked(begin, end, chunk, fn);
}

}  // namespace pfs
