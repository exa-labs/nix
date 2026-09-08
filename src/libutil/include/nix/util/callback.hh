#pragma once
///@file

#include <future>
#include <functional>

#include "nix/util/fun.hh"
#include "nix/util/util.hh"

namespace nix {

/**
 * A callback is a wrapper around a lambda that accepts a valid of
 * type T or an exception. (We abuse std::future<T> to pass the value or
 * exception.)
 *
 * A callback may be invoked at most once. Any subsequent invocation
 * (value or exception) is a no-op: a callback that has already been
 * fulfilled cannot be un-fulfilled, and aborting the process because a
 * racing error path tried to report a second time is strictly worse
 * than dropping the second report. Exceptions escaping the wrapped
 * lambda are logged and swallowed, since both entry points are
 * `noexcept`.
 */
template<typename T>
class Callback
{
    nix::fun<void(std::future<T>)> fun;
    std::atomic_flag done = ATOMIC_FLAG_INIT;

    void invoke(std::promise<T> && promise) noexcept
    {
        try {
            fun(promise.get_future());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }

public:

    Callback(nix::fun<void(std::future<T>)> fun)
        : fun(fun)
    {
    }

    // NOTE: std::function is noexcept move-constructible since C++20.
    Callback(Callback && callback) noexcept(std::is_nothrow_move_constructible_v<decltype(fun)>)
        : fun(std::move(callback.fun))
    {
        auto prev = callback.done.test_and_set();
        if (prev)
            done.test_and_set();
    }

    void operator()(T && t) noexcept
    {
        if (done.test_and_set())
            return;
        std::promise<T> promise;
        promise.set_value(std::move(t));
        invoke(std::move(promise));
    }

    void rethrow(const std::exception_ptr & exc = std::current_exception()) noexcept
    {
        if (done.test_and_set())
            return;
        std::promise<T> promise;
        promise.set_exception(exc);
        invoke(std::move(promise));
    }
};

} // namespace nix
