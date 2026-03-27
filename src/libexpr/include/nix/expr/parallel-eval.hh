#pragma once
///@file

/**
 * Thread pool and synchronization primitives for parallel Nix evaluation.
 *
 * The main parallelization point is `forceValueDeep`, which recursively
 * forces all thunks in a value tree.  When it encounters a large attribute
 * set (>= kMinParallelAttrs attributes), the forcing of each attribute
 * subtree is distributed across a pool of worker threads.
 *
 * Thread safety for thunk forcing is achieved with a striped spinlock
 * array: before mutating a Value from thunk→blackhole→result, the
 * forcing thread acquires the spinlock for that Value's stripe.  Other
 * threads that encounter a blackhole (thunk being forced by someone
 * else) spin-wait until the result is available.  Infinite-recursion
 * detection (same thread re-entering the same thunk) uses a
 * thread_local stack of currently-forcing Value pointers.
 */

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "nix/expr/eval-gc.hh"

#if NIX_USE_BOEHMGC
#  include <gc/gc.h>
#endif

namespace nix {

/* ── Striped spinlock for thread-safe thunk claiming ────────────── */

/**
 * Number of lock stripes.  Must be a power of two so the modulo
 * compiles to a bitwise AND.  512 stripes × 64 bytes (cache-line
 * padded) = 32 KiB — fits comfortably in L1.
 */
inline constexpr size_t kNumThunkLockStripes = 512;

/**
 * Cache-line-padded spinlock to avoid false sharing between stripes.
 */
struct alignas(64) PaddedSpinLock
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    void lock() noexcept
    {
        while (flag.test_and_set(std::memory_order_acquire))
            /* On x86 PAUSE reduces power and improves performance of
               spin-wait loops by hinting the CPU. */
#if defined(__x86_64__)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
    }

    void unlock() noexcept
    {
        flag.clear(std::memory_order_release);
    }
};

/**
 * Global striped spinlock array for thunk claiming.
 * Indexed by (Value-pointer >> 4) & (kNumThunkLockStripes - 1).
 */
inline std::array<PaddedSpinLock, kNumThunkLockStripes> & thunkLocks()
{
    static std::array<PaddedSpinLock, kNumThunkLockStripes> locks{};
    return locks;
}

inline PaddedSpinLock & thunkLockFor(const void * v) noexcept
{
    auto idx = (reinterpret_cast<uintptr_t>(v) >> 4) & (kNumThunkLockStripes - 1);
    return thunkLocks()[idx];
}

/* ── Thread-local "currently forcing" stack ─────────────────────── */

/**
 * Each thread keeps a small stack of Value pointers it is currently
 * forcing.  When we encounter a blackhole we check this stack: if the
 * Value is present it is genuine infinite recursion; otherwise another
 * thread owns it and we spin-wait.
 */
inline thread_local std::vector<const void *> tl_currentlyForcing;

inline bool isBeingForcedByCurrentThread(const void * v) noexcept
{
    for (auto * p : tl_currentlyForcing)
        if (p == v)
            return true;
    return false;
}

/* ── Parallel-eval active flag ──────────────────────────────────
 *
 * When no worker threads are running (the common case for simple
 * `nix eval .#foo.drvPath`), forceValue skips all locking and uses
 * the original single-threaded code path.  The flag is set by
 * forceValueDeep just before submitting work to the thread pool
 * and cleared when all workers have joined.
 */
inline std::atomic<int> parallelEvalActive{0};

/* ── Thread-local call depth (replaces EvalState::callDepth) ───── */

/**
 * Nix call-stack depth counter, used to enforce max-call-depth.
 * Must be thread_local so that worker threads get their own counter.
 */
inline thread_local size_t tl_callDepth = 0;

/* ── Minimum attrs/list size to trigger parallel forcing ────────── */

inline constexpr size_t kMinParallelAttrs = 64;
inline constexpr size_t kMinParallelListElems = 128;

/* ── RAII guard that increments/decrements parallelEvalActive ──── */
struct ParallelEvalGuard
{
    ParallelEvalGuard() { parallelEvalActive.fetch_add(1, std::memory_order_release); }
    ~ParallelEvalGuard() { parallelEvalActive.fetch_sub(1, std::memory_order_release); }
    ParallelEvalGuard(const ParallelEvalGuard &) = delete;
    ParallelEvalGuard & operator=(const ParallelEvalGuard &) = delete;
};

/* ── Simple thread pool with Boehm-GC registration ─────────────── */

class EvalThreadPool
{
public:
    explicit EvalThreadPool(size_t numThreads = 0)
    {
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads <= 1)
                numThreads = 2; // at least 2 workers
            else
                numThreads = numThreads - 1; // leave one core for the main thread
        }
        numThreads_ = numThreads;

        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~EvalThreadPool()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto & w : workers_)
            w.join();
    }

    EvalThreadPool(const EvalThreadPool &) = delete;
    EvalThreadPool & operator=(const EvalThreadPool &) = delete;

    /**
     * Submit a task and get a future for its result.
     */
    template<typename F>
    auto submit(F && f) -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    size_t size() const noexcept { return numThreads_; }

private:
    void workerLoop()
    {
#if NIX_USE_BOEHMGC
        GC_stack_base sb;
        GC_get_stack_base(&sb);
        GC_register_my_thread(&sb);
#endif

        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty())
                    break;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }

#if NIX_USE_BOEHMGC
        GC_unregister_my_thread();
#endif
    }

    size_t numThreads_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace nix
