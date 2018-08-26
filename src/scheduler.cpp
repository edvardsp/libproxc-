//          Copyright Oliver Kowalke 2009.
//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <proxc/config.hpp>

#include <proxc/runtime/context.hpp>
#include <proxc/runtime/scheduler.hpp>

#include <proxc/alt.hpp>
#include <proxc/exceptions.hpp>

#include <proxc/scheduling_policy/policy_base.hpp>
#include <proxc/scheduling_policy/round_robin.hpp>
#include <proxc/scheduling_policy/work_stealing.hpp>

#include <proxc/detail/num_cpus.hpp>
#include <proxc/detail/spinlock.hpp>

#include <boost/assert.hpp>
#include <boost/intrusive_ptr.hpp>

PROXC_NAMESPACE_BEGIN
namespace runtime {

namespace hook = detail::hook;

struct WaitGroup
{
    std::mutex                 mtx_;
    std::condition_variable    cv_;
    std::size_t                count_{ 0 };

    void add( std::size_t count ) noexcept
    { count_ = count; }

    void wait() noexcept
    {
        std::unique_lock< std::mutex > lk{ mtx_ };
        if ( --count_ == 0 ) {
            lk.unlock();
            cv_.notify_all();
        } else {
            cv_.wait( lk, [this]{ return count_ == 0; } );
        }
    }
};

void kernel_thread_fn( WaitGroup & wg )
{
    // this will allocated the scheduler for this thread
    auto self = Scheduler::self();
    // need to wait for all of the threads to finish initialize the scheduler
    wg.wait();
    self->resume();
    // when returned, the scheduler has exited and ready to cleanup
}

static std::atomic< std::size_t >    sched_counter_{ 0 };
static std::vector< std::thread >    thread_vec_;
static std::vector< Scheduler * >    sched_vec_;

thread_local Scheduler * Scheduler::Initializer::self_{ nullptr };
thread_local std::size_t Scheduler::Initializer::counter_{ 0 };

Scheduler::Initializer::Initializer()
{
    if ( counter_++ == 0 ) {
        auto scheduler = new Scheduler{};
        self_ = scheduler;
        BOOST_ASSERT( Scheduler::running()->is_type( Context::Type::Main ) );

        if ( sched_counter_++ == 0 ) {
            static WaitGroup wg;
            auto num_sched = detail::num_cpus();
            sched_vec_.reserve( num_sched - 1 );
            thread_vec_.reserve( num_sched - 1 );

            wg.add( num_sched );
            for ( std::size_t i = 0; i < num_sched - 1; ++i ) {
                thread_vec_.emplace_back( kernel_thread_fn, std::ref( wg ) );
            }
            wg.wait();
            sched_counter_ = 0;

        } else {
            static MtxT splk;
            LockT lk{ splk };
            sched_vec_.push_back( self_ );
        }
    }
}

Scheduler::Initializer::~Initializer()
{
    if ( --counter_ == 0 ) {
        if ( sched_counter_++ == 0 ) {
            std::for_each( sched_vec_.begin(), sched_vec_.end(),
                std::mem_fn( & Scheduler::signal_exit ) );
            std::for_each( thread_vec_.begin(), thread_vec_.end(),
                std::mem_fn( & std::thread::join ) );
        }

        BOOST_ASSERT( Scheduler::running()->is_type( Context::Type::Main ) );

        auto scheduler = self_;
        delete scheduler;
    }
}

Scheduler * Scheduler::self() noexcept
{
    thread_local static Initializer init;
    return Initializer::self_;
}

Context * Scheduler::running() noexcept
{
    BOOST_ASSERT( Scheduler::self() != nullptr );
    BOOST_ASSERT( Scheduler::self()->running_ != nullptr );
    return Scheduler::self()->running_;
}

Scheduler::Scheduler()
    : policy_{ new scheduling_policy::WorkStealing{} }
    , main_ctx_{ new Context{ context::main_type } }
    , scheduler_ctx_{ new Context{ context::scheduler_type,
        [this]( void * vp ) { run_( vp ); } } }
{
    main_ctx_->scheduler_      = this;
    scheduler_ctx_->scheduler_ = this;
    running_                   = main_ctx_.get();

    schedule( scheduler_ctx_.get() );
}

Scheduler::~Scheduler()
{
    BOOST_ASSERT( main_ctx_.get() != nullptr );
    BOOST_ASSERT( scheduler_ctx_.get() != nullptr );
    BOOST_ASSERT( running_ == main_ctx_.get() );

    exit_.store( true, std::memory_order_relaxed );
    join( scheduler_ctx_.get() );

    BOOST_ASSERT( main_ctx_->wait_queue_.empty() );
    BOOST_ASSERT( scheduler_ctx_->wait_queue_.empty() );

    scheduler_ctx_.reset();
    main_ctx_.reset();

    for ( auto et = work_queue_.begin();
          et != work_queue_.end();
          et = work_queue_.erase( et ) ) {
        auto ctx = &( *et );
        intrusive_ptr_release( ctx );
    }
    running_ = nullptr;

    BOOST_ASSERT( work_queue_.empty() );
    BOOST_ASSERT( sleep_queue_.empty() );
    BOOST_ASSERT( terminated_queue_.empty() );
    BOOST_ASSERT( remote_queue_.pop() == nullptr );
}

void Scheduler::resume_( Context * to_ctx, CtxSwitchData * data ) noexcept
{
    BOOST_ASSERT(   to_ctx != nullptr );
    BOOST_ASSERT(   running_ != nullptr );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Terminated >() );

    std::swap( to_ctx, running_ );

    // context switch
    void * vp = static_cast< void * >( data );
    vp = running_->resume( vp );
    data = static_cast< CtxSwitchData * >( vp );
    resolve_ctx_switch_data_( data );
}

void Scheduler::wait() noexcept
{
    resume();
}

void Scheduler::wait( Context * ctx ) noexcept
{
    CtxSwitchData data{ ctx };
    resume( std::addressof( data ) );
}

void Scheduler::wait( LockT & splk ) noexcept
{
    CtxSwitchData data{ std::addressof( splk ) };
    resume( std::addressof( data ) );
}

bool Scheduler::wait_until( TimePointT const & time_point ) noexcept
{
    return sleep_until( time_point );
}

bool Scheduler::wait_until( TimePointT const & time_point, Context * ctx ) noexcept
{
    CtxSwitchData data{ ctx };
    return sleep_until( time_point, std::addressof( data ) );
}

bool Scheduler::wait_until( TimePointT const & time_point, LockT & splk, bool lock ) noexcept
{
    CtxSwitchData data{ std::addressof( splk ) };
    auto ret = sleep_until( time_point, std::addressof( data ) );
    if ( ! ret && lock ) {
        splk.lock();
    }
    return ret;
}

bool Scheduler::alt_wait( Alt * alt, LockT & splk ) noexcept
{
    BOOST_ASSERT(   alt != nullptr );
    BOOST_ASSERT(   alt->ctx_ == Scheduler::running() );

    auto alt_ctx = alt->ctx_;
    BOOST_ASSERT(   alt_ctx->alt_ == nullptr );
    BOOST_ASSERT(   alt_ctx->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! alt_ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! alt_ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! alt_ctx->is_linked< hook::Terminated >() );

    if ( alt->time_point_ < TimePointT::max() ) {
        alt_ctx->alt_        = alt;
        alt_ctx->time_point_ = alt->time_point_;
        alt_ctx->link( sleep_queue_ );
    }

    CtxSwitchData data{ std::addressof( splk ) };
    resume( std::addressof( data ) );

    BOOST_ASSERT( alt_ctx == Scheduler::running() );

    alt_ctx->alt_        = nullptr;
    alt_ctx->time_point_ = TimePointT::max();
    // FIXME: this is not sound, since the context can migrate after
    // the context switch, making the access to the sleep queue erroneous
    alt_ctx->try_unlink< hook::Sleep >();

    return alt->time_point_ <= ClockT::now();
}

void Scheduler::resume( CtxSwitchData * data ) noexcept
{
    resume_( policy_->pick_next(), data );
}

void Scheduler::resume( Context * to_ctx, CtxSwitchData * data ) noexcept
{
    resume_( to_ctx, data );
}

void Scheduler::terminate_( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ctx == Scheduler::running() );

    BOOST_ASSERT(   ctx->is_type( Context::Type::Dynamic ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    LockT lk{ ctx->splk_ };

    ctx->terminate();
    ctx->link( terminated_queue_ );
    ctx->unlink< hook::Work >();

    wakeup_waiting_on_( ctx );

    wait( lk );
}

void Scheduler::schedule_local_( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT( ! ctx->has_terminated() );

    ctx->try_unlink< hook::Sleep >();

    policy_->enqueue( ctx );
}

void Scheduler::schedule_remote_( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_type( Context::Type::Scheduler ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT( ! ctx->has_terminated() );
    BOOST_ASSERT(   ctx->scheduler_ == this );

    remote_queue_.push( ctx );
    policy_->notify();
}

void Scheduler::schedule( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ctx->scheduler_ != nullptr );

    if ( ctx->scheduler_ == this ) {
        schedule_local_( ctx );
    } else {
        ctx->scheduler_->schedule_remote_( ctx );
    }
}

void Scheduler::attach( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Dynamic ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT(   ctx->scheduler_ == nullptr );

    ctx->link( work_queue_ );
    ctx->scheduler_ = this;
}

void Scheduler::detach( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Dynamic ) );
    BOOST_ASSERT(   ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT(   ctx->scheduler_ != nullptr );

    ctx->unlink< hook::Work >();
    ctx->scheduler_ = nullptr;
}

void Scheduler::commit( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Dynamic ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    attach( ctx );
    schedule( ctx );
}

void Scheduler::yield() noexcept
{
    auto ctx = Scheduler::running();
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    auto next = policy_->pick_next();
    if ( next != nullptr ) {
        CtxSwitchData data{ ctx };
        resume( next, std::addressof( data ) );
        BOOST_ASSERT( ctx == Scheduler::running() );
    }
}

void Scheduler::join( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );

    Context * running_ctx = Scheduler::running();

    LockT lk{ ctx->splk_ };
    if ( ! ctx->has_terminated() ) {
        running_ctx->link( ctx->wait_queue_ );
        wait( lk );
        BOOST_ASSERT( Scheduler::running() == running_ctx );
    }
}

bool Scheduler::sleep_until( TimePointT const & time_point, CtxSwitchData * data ) noexcept
{
    auto running_ctx = Scheduler::running();
    BOOST_ASSERT(   running_ctx != nullptr );
    BOOST_ASSERT(   running_ctx->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! running_ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! running_ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! running_ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! running_ctx->is_linked< hook::Terminated >() );

    if ( ClockT::now() < time_point ) {
        running_ctx->time_point_ = time_point;
        running_ctx->link( sleep_queue_ );
        resume( data );
        running_ctx->time_point_ = TimePointT::max();
        return ClockT::now() >= time_point;
    } else {
        return true;
    }
}

void Scheduler::wakeup_sleep_() noexcept
{
    LockT lk{ splk_ };
    auto now = ClockT::now();
    auto sleep_it = sleep_queue_.begin();
    while ( sleep_it != sleep_queue_.end() ) {
        auto ctx = &( *sleep_it );

        BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

        // Keep advancing the queue if deadline is reached,
        // break if not.
        if ( ctx->time_point_ > now ) {
            break;
        }
        sleep_it = sleep_queue_.erase( sleep_it );
        ctx->time_point_ = TimePointT::max();
        if ( ctx->alt_ == nullptr || ctx->alt_->try_timeout() ) {
            schedule( ctx );
        }
    }
}

void Scheduler::wakeup_waiting_on_( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT(   ctx->has_terminated() );

    while ( ! ctx->wait_queue_.empty() ) {
        auto waiting_ctx = & ctx->wait_queue_.front();
        ctx->wait_queue_.pop_front();
        schedule( waiting_ctx );
    }

    BOOST_ASSERT( ctx->wait_queue_.empty() );
}

void Scheduler::transition_remote_() noexcept
{
    for ( Context * ctx = remote_queue_.pop();
          ctx != nullptr;
          ctx = remote_queue_.pop() ) {
        schedule_local_( ctx );
    }
}

void Scheduler::cleanup_terminated_() noexcept
{
    while ( ! terminated_queue_.empty() ) {
        auto ctx = & terminated_queue_.front();
        terminated_queue_.pop_front();

        BOOST_ASSERT(   ctx->is_type( Context::Type::Dynamic ) );
        BOOST_ASSERT( ! ctx->is_type( Context::Type::Static ) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );

        intrusive_ptr_release( ctx );
    }
}

void Scheduler::print_debug() noexcept
{
    std::cout << "Scheduler: " << std::endl;
    std::cout << "  Scheduler Ctx: " << std::endl;
    scheduler_ctx_->print_debug();
    std::cout << "  Main Ctx: " << std::endl;
    main_ctx_->print_debug();
    std::cout << "  Running: " << std::endl;
    running_->print_debug();
    std::cout << "  Work Queue:" << std::endl;
    for ( auto& ctx : work_queue_ ) {
        ctx.print_debug();
    }
    std::cout << "  Sleep Queue:" << std::endl;
    for ( auto& ctx : sleep_queue_ ) {
        std::cout << "    | " << ctx.get_id() << std::endl;
    }
    std::cout << "  Terminated Queue:" << std::endl;
    for ( auto& ctx : terminated_queue_ ) {
        std::cout << "    | " << ctx.get_id() << std::endl;
    }
}

void Scheduler::resolve_ctx_switch_data_( CtxSwitchData * data ) noexcept
{
    if ( data != nullptr ) {
        if ( data->ctx_ != nullptr ) {
            schedule( data->ctx_ );
        }
        if ( data->splk_ != nullptr ) {
            data->splk_->unlock();
        }
    }
}

// called by main ctx in new threads when multi-core
void Scheduler::signal_exit() noexcept
{
    exit_.store( true, std::memory_order_release );
    policy_->notify();
}

// Scheduler context loop
void Scheduler::run_( void * vp )
{
    BOOST_ASSERT( running_ == scheduler_ctx_.get() );

    CtxSwitchData * data = static_cast< CtxSwitchData * >( vp );
    resolve_ctx_switch_data_( data );

    for ( ;; ) {
        if ( exit_.load( std::memory_order_acquire ) ) {
            policy_->notify();
            if ( work_queue_.empty( )) {
                break;
            }
        }

        cleanup_terminated_();
        transition_remote_();
        wakeup_sleep_();

        auto ctx = policy_->pick_next();
        if ( ctx != nullptr ) {
            schedule( scheduler_ctx_.get() );
            resume( ctx );
            BOOST_ASSERT( running_ == scheduler_ctx_.get() );

        } else {
            auto sleep_it = sleep_queue_.begin();
            auto suspend_time = ( sleep_it != sleep_queue_.end() )
                ? sleep_it->time_point_
                : ClockT::now() + std::chrono::milliseconds( 1 );
            policy_->suspend_until( suspend_time );
        }
    }
    cleanup_terminated_();

    scheduler_ctx_->terminate();
    /* wakeup_waiting_on_( scheduler_ctx_.get() ); */

    main_ctx_->try_unlink< hook::Ready >();
    resume( main_ctx_.get() );
    BOOST_ASSERT_MSG( false, "unreachable" );
    throw UnreachableError{};
}

} // namespace runtime
PROXC_NAMESPACE_END

