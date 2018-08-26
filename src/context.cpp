//          Copyright Oliver Kowalke 2009.
//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <mutex>

#include <proxc/config.hpp>

#include <proxc/runtime/context.hpp>
#include <proxc/runtime/scheduler.hpp>

#include <proxc/exceptions.hpp>

#include <proxc/detail/spinlock.hpp>

#include <boost/context/execution_context_v1.hpp>

PROXC_NAMESPACE_BEGIN
namespace runtime {

// intrusive_ptr friend methods
void intrusive_ptr_add_ref( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    ctx->use_count_.fetch_add( 1, std::memory_order_relaxed );
}

void intrusive_ptr_release( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    if ( 1 != ctx->use_count_.fetch_sub( 1, std::memory_order_release ) ) { return; }
    std::atomic_thread_fence( std::memory_order_acquire );

    delete ctx;
}

// Context methods
Context::Context( context::MainType )
    : type_{ Type::Main }
    , ctx_{ boost::context::execution_context::current() }
{
}

Context::Context( context::SchedulerType, EntryFn && fn )
    : type_{ Type::Scheduler }
    , entry_fn_{ std::move( fn ) }
    , ctx_{ [this]( void * vp ) { trampoline_( vp ); } }
{
}

Context::Context( context::WorkType, EntryFn && fn )
    : type_{ Type::Work }
    , entry_fn_{ std::move( fn ) }
    , ctx_{ [this]( void * vp ) { trampoline_( vp ); } }
    , use_count_{ 1 }
{
}

Context::~Context() noexcept
{
    BOOST_ASSERT( ! is_linked< hook::Ready >() );
    BOOST_ASSERT( ! is_linked< hook::Wait >() );
    BOOST_ASSERT( ! is_linked< hook::Sleep >() );
    BOOST_ASSERT( wait_queue_.empty() );
}

Context::Id Context::get_id() const noexcept
{
    return Id{ const_cast< Context * >( this ) };
}

void * Context::resume( void * vp ) noexcept
{
    return ctx_( vp );
}

bool Context::is_type( Type type ) const noexcept
{
    return ( static_cast< int >( type ) & static_cast< int >( type_ ) ) != 0;
}

void Context::terminate() noexcept
{
    terminated_flag_.store( true, std::memory_order_release );
}

bool Context::has_terminated() const noexcept
{
    return terminated_flag_.load( std::memory_order_acquire );
}

void Context::print_debug() noexcept
{
    std::cout << "    Context id : " << get_id() << std::endl;
    std::cout << "      -> type  : ";
    switch ( type_ ) {
    case Type::None:      std::cout << "None"; break;
    case Type::Main:      std::cout << "Main"; break;
    case Type::Scheduler: std::cout << "Scheduler"; break;
    case Type::Work:      std::cout << "Work"; break;
    case Type::Process: case Type::Static:
                          std::cout << "(invalid)"; break;
    }
    std::cout << std::endl;
    std::cout << "      -> Links :" << std::endl;
    if ( is_linked< hook::Work >() )       std::cout << "         | Work" << std::endl;
    if ( is_linked< hook::Ready >() )      std::cout << "         | Ready" << std::endl;
    if ( is_linked< hook::Wait >() )       std::cout << "         | Wait" << std::endl;
    if ( is_linked< hook::Sleep >() )      std::cout << "         | Sleep" << std::endl;
    if ( is_linked< hook::Terminated >() ) std::cout << "         | Terminated" << std::endl;
    if ( mpsc_next_.load( std::memory_order_relaxed ) )
                                           std::cout << "         | RemoteReady" << std::endl;
    std::cout << "      -> wait queue:" << std::endl;
    for ( auto& ctx : wait_queue_ ) {
        std::cout << "         | " << ctx.get_id() << std::endl;
    }
}

void Context::trampoline_( void * vp )
{
    BOOST_ASSERT( entry_fn_ != nullptr );
    entry_fn_( vp );
    BOOST_ASSERT_MSG( false, "unreachable: Context should not return from entry_func_( ).");
    throw UnreachableError{ std::make_error_code( std::errc::state_not_recoverable ), "unreachable" };
}

void Context::wait_for( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );

    link( ctx->wait_queue_ );
}

template<> detail::hook::Ready      & Context::get_hook_() noexcept { return ready_; }
template<> detail::hook::Work       & Context::get_hook_() noexcept { return work_; }
template<> detail::hook::Wait       & Context::get_hook_() noexcept { return wait_; }
template<> detail::hook::Sleep      & Context::get_hook_() noexcept { return sleep_; }
template<> detail::hook::Terminated & Context::get_hook_() noexcept { return terminated_; }

} // namespace runtime
PROXC_NAMESPACE_END

