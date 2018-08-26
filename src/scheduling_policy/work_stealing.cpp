//          Copyright Oliver Kowalke 2009.
//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <algorithm>
#include <mutex>
#include <random>
#include <thread>

#include <proxc/config.hpp>

#include <proxc/runtime/context.hpp>
#include <proxc/runtime/scheduler.hpp>

#include <proxc/scheduling_policy/work_stealing.hpp>

#include <proxc/detail/num_cpus.hpp>

#include <boost/assert.hpp>

PROXC_NAMESPACE_BEGIN
namespace scheduling_policy {
namespace detail {

template<>
std::size_t WorkStealing::num_workers_{ 0 };

template<>
std::vector<WorkStealing *> WorkStealing::work_stealers_{};

/* template<> */
/* Barrier WorkStealing::barrier_{}; */

template<>
void WorkStealing::init_()
{
    num_workers_ = ::proxc::detail::num_cpus();
    work_stealers_.resize( num_workers_ );
}

template<>
WorkStealing::WorkStealingPolicy()
{
    static std::once_flag flag;
    std::call_once( flag, & WorkStealing::init_ );
    static std::atomic<std::size_t> n_id{ 0 };
    id_ = n_id.fetch_add(1, std::memory_order_acq_rel);
    work_stealers_[id_] = this;
    BOOST_ASSERT(id_ < num_workers_);
}

template<>
runtime::Context * WorkStealing::steal() noexcept
{
    return deque_.steal();
}

template<>
void WorkStealing::reserve( std::size_t capacity ) noexcept
{
    deque_.reserve( capacity );
}


template<>
void WorkStealing::suspend_until(TimePointT const & time_point) noexcept
{
    if ( time_point == TimePointT::max() ) {
        barrier_.wait();
    } else {
        barrier_.wait_until( time_point );
    }
}

template<>
void WorkStealing::notify() noexcept
{
    barrier_.notify();
}

template<>
std::size_t WorkStealing::take_id() noexcept
{
    static thread_local std::minstd_rand rng{ std::random_device{}() };
    static std::uniform_int_distribution< std::size_t > distr{ 0, num_workers_ - 1 };
    std::size_t id{};
    do {
        id = distr( rng );
    } while ( id == id_ );
    return id;
}

template<>
void WorkStealing::signal_stealing() noexcept
{
    work_stealers_[ take_id() ]->barrier_.notify();
}

template<>
void WorkStealing::enqueue( runtime::Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    if ( ctx->is_type( runtime::Context::Type::Dynamic ) ) {
        // can be stolen
        runtime::Scheduler::self()->detach( ctx );
        deque_.push( ctx );

    } else {
        // cannot by stolen
        ctx->link( ready_queue_ );
    }
}

template<>
runtime::Context * WorkStealing::pick_next() noexcept
{
    auto ctx = deque_.pop();
    if ( ctx != nullptr ) {
        /* if ( ! deque_.is_empty() ) { */
        /*     signal_stealing(); */
        /* } */
        runtime::Scheduler::self()->attach( ctx );

    } else if ( ! ready_queue_.empty() ) {
        ctx = std::addressof( ready_queue_.front() );
        ready_queue_.pop_front();

    } else if ( ctx == nullptr ) {
        ctx = work_stealers_[ take_id() ]->steal();
        if ( ctx != nullptr ) {
            runtime::Scheduler::self()->attach( ctx );
        }
    }
    return ctx;
}

template<>
bool WorkStealing::is_ready() const noexcept
{
    return ! deque_.is_empty();
}

} // namespace detail
} // namespace scheduling_policy
PROXC_NAMESPACE_END

