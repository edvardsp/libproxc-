//          Copyright Oliver Kowalke 2009.
//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#include <proxc/config.hpp>

#include <proxc/detail/cpu_relax.hpp>
#include <proxc/detail/xorshift.hpp>

#if defined(PROXC_COMP_CLANG)
#   pragma clang diagnostic push
#   pragma clang diagnostic ignored "-Wunused-private-field"
#endif

PROXC_NAMESPACE_BEGIN
namespace detail {

class Spinlock
{
private:
    enum class State {
        Locked,
        Unlocked,
    };

    enum {
        MAX_TESTS = 100,
    };

    alignas(cache_alignment) std::atomic< State >          state_{ State::Unlocked };
    alignas(cache_alignment) std::atomic< std::size_t >    prev_tests_{ 0 };

    alignas(cache_alignment) XorShift< std::size_t >    rng{};

public:
    Spinlock() = default;
    ~Spinlock() noexcept {}

    // make non-copyable
    Spinlock(Spinlock const &) = delete;
    Spinlock & operator = (Spinlock const &) = delete;

    void lock() noexcept
    {
        std::size_t n_collisions = 0;
        for (;;) {
            std::size_t n_tests = 0;

            const std::size_t prev_tests = prev_tests_.load( std::memory_order_relaxed );
            const std::size_t max_tests = std::min(
                static_cast< std::size_t >( MAX_TESTS ),
                2 * prev_tests + 10
            );

            while ( state_.load( std::memory_order_relaxed ) == State::Locked ) {
                if ( n_tests < max_tests ) {
                    ++n_tests;
                    cpu_relax();

                } else {
                    ++n_tests;
                    static constexpr std::chrono::microseconds us0{ 0 };
                    std::this_thread::sleep_for( us0 );
                }
            }

            if ( State::Locked == state_.exchange( State::Locked, std::memory_order_acquire ) ) {
                // this way of storing thread local random number generators causes segmentation
                // faults spuriously when running on multi-core. No idea why.
                // Fix: let the random number generator be class member instead. Not ideal,
                // but at least it doesn't cause segmentation faults.
                /* static thread_local XorShift<std::size_t> rng{}; */
                static std::uniform_int_distribution< std::size_t > distr
                    { 0, static_cast< std::size_t >( 1 ) << n_collisions };
                const std::size_t z = distr( rng );
                ++n_collisions;
                for ( std::size_t i = 0; i < z; ++i ) {
                    cpu_relax();
                }

            } else {
                prev_tests_.store( prev_tests + ( n_tests - prev_tests ) / 8, std::memory_order_relaxed );
                break;
            }
        }
    }

    bool try_lock() noexcept
    {
        State expected = State::Unlocked;
        return state_.compare_exchange_strong(
            expected,
            State::Locked,
            std::memory_order_acq_rel,
            std::memory_order_relaxed
        );
    }

    void unlock() noexcept
    {
        state_.store( State::Unlocked, std::memory_order_release );
    }
};

} // namespace detail
PROXC_NAMESPACE_END

#if defined(PROXC_COMP_CLANG)
#   pragma clang diagnostic pop
#endif

