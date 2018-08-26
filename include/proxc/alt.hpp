//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include <proxc/config.hpp>

#include <proxc/channel.hpp>
#include <proxc/timer.hpp>
#include <proxc/exceptions.hpp>
#include <proxc/runtime/context.hpp>
#include <proxc/alt/sync.hpp>
#include <proxc/alt/state.hpp>
#include <proxc/alt/choice_base.hpp>
#include <proxc/alt/choice_send.hpp>
#include <proxc/alt/choice_recv.hpp>
#include <proxc/detail/delegate.hpp>
#include <proxc/detail/spinlock.hpp>

#include <boost/assert.hpp>

PROXC_NAMESPACE_BEGIN

class Alt
{
private:
    enum class Winner {
        Choice,
        Timeout,
        Skip,
    };

    using ChoiceT   = alt::ChoiceBase;
    using ChoicePtr = std::unique_ptr< ChoiceT >;

    using SyncT = alt::Sync;
    using LockT = detail::Spinlock;

    using ChannelId = channel::detail::ChannelId;

    template<typename EndT>
    using ItemT = typename EndT::ItemT;
    template<typename EndIt>
    using EndT = typename std::iterator_traits< EndIt >::value_type;

    template<typename Tx>
    using TxFn = detail::delegate< void( void ) >;
    template<typename Rx>
    using RxFn = detail::delegate< void( ItemT< Rx > ) >;

    using TimePointT = runtime::Context::TimePointT;
    using TimerFn = detail::delegate< void( void ) >;

    using SkipFn = detail::delegate< void( void ) >;

    std::atomic< alt::State >    state_{ alt::State::Checking };

    std::vector< ChoicePtr >    choices_{};

    TimePointT      tp_start_{ runtime::ClockT::now() };
    TimePointT      time_point_{ TimePointT::max() };
    TimerFn         timer_fn_{};

    std::atomic< bool >    has_skip_{ false };
    SkipFn                 skip_fn_{};

    runtime::Context *    ctx_;
    LockT                 splk_;

    alignas(cache_alignment) std::atomic_flag            select_flag_;
    alignas(cache_alignment) std::atomic< ChoiceT * >    selected_{ nullptr };

    template<typename T, std::size_t N>
    using SmallVec = boost::container::small_vector< T, N >;

    struct ChoiceAudit
    {
        // number of audits which will be allocated on the stack.
        // more than N will be allocated on the heap.
        constexpr static std::size_t N = 4;
        enum class State {
            Tx,
            Rx,
            Clash,
        };
        State                       state_;
        SmallVec< ChoiceT *, N >    vec_;
        ChoiceAudit() = default;
        ChoiceAudit( State state, ChoiceT * choice )
            : state_{ state }, vec_{ choice }
        {}
    };
    std::map< ChannelId, ChoiceAudit >    ch_audit_;

    template<typename U>
    friend class channel::detail::ChannelImpl;
    friend class alt::ChoiceBase;
    friend class runtime::Scheduler;

public:
    Alt();
    ~Alt() {}

    // make non-copyable
    Alt( Alt const & ) = delete;
    Alt & operator = ( Alt const & ) = delete;

    // make non-movable
    Alt( Alt && ) = delete;
    Alt & operator = ( Alt && ) = delete;

    // send choice without guard
    template<typename Tx>
    PROXC_WARN_UNUSED
    Alt & send( Tx & tx,
                ItemT< Tx > && item,
                TxFn< Tx > fn = TxFn< Tx >{} ) noexcept;

    template<typename Tx>
    PROXC_WARN_UNUSED
    Alt & send( Tx & tx,
                ItemT< Tx > const & item,
                TxFn< Tx > fn = TxFn< Tx >{} ) noexcept;

    // send choice with guard
    template<typename Tx>
    PROXC_WARN_UNUSED
    Alt & send_if( bool guard,
                   Tx & tx,
                   ItemT< Tx > && item,
                   TxFn< Tx > fn = TxFn< Tx >{} ) noexcept;

    template<typename Tx>
    PROXC_WARN_UNUSED
    Alt & send_if( bool guard,
                   Tx & tx,
                   ItemT< Tx > const & item,
                   TxFn< Tx > fn = TxFn< Tx >{} ) noexcept;

    // replicated send choice over item iterator
    template< typename TxIt, typename ItemIt>
    PROXC_WARN_UNUSED
    Alt & send_for( TxIt tx_first,     TxIt tx_last,
                    ItemIt item_first, ItemIt item_last,
                    TxFn< EndT< TxIt > > fn = TxFn< EndT< TxIt > >{} ) noexcept;

    // replicated send choice over single item
    template<typename TxIt>
    PROXC_WARN_UNUSED
    Alt & send_for( TxIt tx_first, TxIt tx_last,
                    ItemT< EndT< TxIt > > item,
                    TxFn< EndT< TxIt > > fn = TxFn< EndT< TxIt > >{} ) noexcept;

    // recv choice without guard
    template<typename Rx>
    PROXC_WARN_UNUSED
    Alt & recv( Rx & rx,
                RxFn< Rx > fn = RxFn< Rx >{} ) noexcept;

    // recv choice with guard
    template<typename Rx>
    PROXC_WARN_UNUSED
    Alt & recv_if( bool guard,
                   Rx & rx,
                   RxFn< Rx > fn = RxFn< Rx >{} ) noexcept;

    // replicated recv choice
    template<typename RxIt>
    PROXC_WARN_UNUSED
    Alt & recv_for( RxIt rx_first, RxIt rx_last,
                    RxFn< EndT< RxIt > > fn = RxFn< EndT< RxIt > >{} ) noexcept;

    // replicated recv choice with guard
    template<typename RxIt>
    PROXC_WARN_UNUSED
    Alt & recv_for_if( bool guard,
                       RxIt rx_first, RxIt rx_last,
                       RxFn< EndT< RxIt > > fn = RxFn< EndT< RxIt > >{} ) noexcept;

    // timeout without guard
    template<typename Timer>
    PROXC_WARN_UNUSED
    Alt & timeout( Timer const & timer,
                   TimerFn fn = TimerFn{} ) noexcept;

    // timeout with guard
    template<typename Timer>
    PROXC_WARN_UNUSED
    Alt & timeout_if( bool guard,
                      Timer const & timer,
                      TimerFn fn = TimerFn{} ) noexcept;

    // skip without guard
    PROXC_WARN_UNUSED
    Alt & skip( SkipFn fn = SkipFn{} ) noexcept;

    // skip with guard
    PROXC_WARN_UNUSED
    Alt & skip_if( bool guard,
                   SkipFn fn = SkipFn{} ) noexcept;

    // consumes alt and determines which choice to select.
    // the chosen choice completes the operation, and an
    // optional corresponding closure is executed.
    void select();

private:
    template< typename Tx
            , typename = std::enable_if_t<
                detail::traits::is_tx< Tx >::value
            > >
    void send_impl( Tx &,
                    ItemT< Tx >,
                    TxFn< Tx > ) noexcept;

    template< typename TxIt
            , typename ItemIt
            , typename = std::enable_if_t<
                detail::traits::is_tx_iterator< TxIt >::value &&
                detail::traits::is_inputiterator< ItemIt >::value
            > >
    void send_for_impl( TxIt,   TxIt,
                        ItemIt, ItemIt,
                        TxFn< EndT< TxIt > > = TxFn< EndT< TxIt > >{} ) noexcept;

    template< typename TxIt
            , typename = std::enable_if_t<
                detail::traits::is_tx_iterator< TxIt >::value
            > >
    void send_for_impl( TxIt, TxIt,
                        ItemT< EndT< TxIt > >,
                        TxFn< EndT< TxIt > > = TxFn< EndT< TxIt > >{} ) noexcept;

    template< typename Rx
            , typename = std::enable_if_t<
                detail::traits::is_rx< Rx >::value
            > >
    void recv_impl( Rx & rx,
                    RxFn< Rx > ) noexcept;

    template< typename RxIt
            , typename = std::enable_if_t<
                detail::traits::is_rx_iterator< RxIt >::value
            > >
    void recv_for_impl( RxIt,
                        RxIt,
                        RxFn< EndT< RxIt > > = RxFn< EndT< RxIt > >{} ) noexcept;

    template< typename Timer
            , typename = std::enable_if_t<
                detail::traits::is_timer< Timer >::value
            > >
    void timeout_impl( Timer const &,
                       TimerFn ) noexcept;

    Winner select_0( bool skip );
    Winner select_1( bool skip, ChoiceT * ) noexcept;
    Winner select_n( bool skip, std::vector< ChoiceT * > & ) noexcept;

    bool try_select( ChoiceT * ) noexcept;
    bool try_alt_select( ChoiceT * ) noexcept;
    bool try_timeout() noexcept;
    bool sync( Alt *, SyncT * ) noexcept;
};

template<typename Tx, typename>
void Alt::send_impl(
    Tx & tx,
    ItemT< Tx > item,
    TxFn< Tx > fn
) noexcept
{
    if ( ! tx.is_closed() ) {
        ChannelId id = tx.get_id();
        auto audit_it = ch_audit_.find( id );
        if ( audit_it == ch_audit_.end()
            || audit_it->second.state_ == ChoiceAudit::State::Tx ) {
            auto pc = std::make_unique< alt::ChoiceSend< ItemT< Tx > > >(
                this,
                ctx_,
                tx,
                std::move( item ),
                std::move( fn )
            );
            if ( audit_it == ch_audit_.end() ) {
                ch_audit_[ id ] = ChoiceAudit{
                    ChoiceAudit::State::Tx,
                    pc.get()
                };
            } else { // state == State::Tx;
                ch_audit_[ id ].vec_.push_back( pc.get() );
            }
            choices_.push_back( std::move( pc ) );
        } else {
            ch_audit_[ id ].state_ = ChoiceAudit::State::Clash;
        }
    }
}

template<typename TxIt, typename ItemIt, typename>
void Alt::send_for_impl(
    TxIt tx_first,
    TxIt tx_last,
    ItemIt item_first,
    ItemIt,
    TxFn< EndT< TxIt > > fn
) noexcept
{
    for ( auto tx_it = tx_first;
          tx_it != tx_last;
          ++tx_it ) {
        send_impl( *tx_it,
                   *item_first++,
                   fn );
    }
}

template<typename TxIt, typename>
void Alt::send_for_impl(
    TxIt tx_first,
    TxIt tx_last,
    ItemT< EndT< TxIt > > item,
    TxFn< EndT< TxIt > > fn
) noexcept
{
    for ( auto tx_it = tx_first;
          tx_it != tx_last;
          ++tx_it ) {
        send_impl( *tx_it,
                   item,
                   fn );
    }
}

template<typename Rx, typename>
void Alt::recv_impl(
    Rx & rx,
    RxFn< Rx > fn
) noexcept
{
    if ( ! rx.is_closed() ) {
        ChannelId id = rx.get_id();
        auto audit_it = ch_audit_.find( id );
        if ( audit_it == ch_audit_.end()
            || audit_it->second.state_ == ChoiceAudit::State::Rx ) {
            auto pc = std::make_unique< alt::ChoiceRecv< ItemT< Rx > > >(
                this,
                ctx_,
                rx,
                std::move( fn )
            );
            if ( audit_it == ch_audit_.end() ) {
                ch_audit_[ id ] = ChoiceAudit{
                    ChoiceAudit::State::Rx,
                    pc.get()
                };
            } else { // state == State::Rx;
                ch_audit_[ id ].vec_.push_back( pc.get() );
            }
            choices_.push_back( std::move( pc ) );
        } else {
            ch_audit_[ id ].state_ = ChoiceAudit::State::Clash;
        }
    }
}

template<typename RxIt, typename>
void Alt::recv_for_impl(
    RxIt rx_first,
    RxIt rx_last,
    RxFn< EndT< RxIt > > fn
) noexcept
{
    for ( auto rx_it = rx_first;
          rx_it != rx_last;
          ++rx_it ) {
        recv_impl( *rx_it, fn );
    }
}

template<typename Timer, typename>
void Alt::timeout_impl(
    Timer const & timer,
    TimerFn fn
) noexcept
{
    Timer new_timer{ timer };
    new_timer.reset();
    if ( new_timer.get() < time_point_ ) {
        time_point_ = new_timer.get();
        timer_fn_ = std::move( fn );
    }
}

// send choice without guard
template<typename Tx>
Alt & Alt::send(
    Tx & tx,
    ItemT< Tx > && item,
    TxFn< Tx > fn
) noexcept
{
    static_assert( detail::traits::is_tx< Tx >::value,
        "Supplied channel end is not a Tx type");

    send_impl( tx, std::move( item ), std::move( fn ) );
    return *this;
}

template<typename Tx>
Alt & Alt::send(
    Tx & tx,
    ItemT< Tx > const & item,
    TxFn< Tx > fn
) noexcept
{
    static_assert( detail::traits::is_tx< Tx >::value,
        "Supplied channel end is not a Tx type");

    send_impl( tx, item, std::move( fn ) );
    return *this;
}

// send choice with guard
template<typename Tx>
Alt & Alt::send_if(
    bool guard,
    Tx & tx,
    ItemT< Tx > && item,
    TxFn< Tx > fn
) noexcept
{
    static_assert( detail::traits::is_tx< Tx >::value,
        "Supplied channel end is not a Tx type");

    if ( guard ) {
        send_impl( tx, std::move( item ), std::move( fn ) );
    }
    return *this;
}

template<typename Tx>
Alt & Alt::send_if(
    bool guard,
    Tx & tx,
    ItemT< Tx > const & item,
    TxFn< Tx > fn
) noexcept
{
    static_assert( detail::traits::is_tx< Tx >::value,
        "Supplied channel end is not a Tx type");

    if ( guard ) {
        send_impl( tx, item, std::move( fn ) );
    }
    return *this;
}

// replicated send choice over item iterator
template<typename TxIt, typename ItemIt>
Alt & Alt::send_for(
    TxIt   tx_first,   TxIt   tx_last,
    ItemIt item_first, ItemIt item_last,
    TxFn< EndT< TxIt > > fn
) noexcept
{
    static_assert( detail::traits::is_tx_iterator< TxIt >::value,
        "Supplied channel end iterator is not a Tx channel end iterator");
    static_assert( detail::traits::is_inputiterator< ItemIt >::value,
        "Supplied item iterator is not an input iterator");

    send_for_impl( tx_first, tx_last, item_first, item_last, std::move( fn ) );
    return *this;
}

// replicated send choice over single item
template<typename TxIt>
Alt & Alt::send_for(
    TxIt tx_first, TxIt tx_last,
    ItemT< EndT< TxIt > > item,
    TxFn< EndT< TxIt > > fn
) noexcept
{
    static_assert( detail::traits::is_tx_iterator< TxIt >::value,
        "Supplied channel end iterator is not a Tx channel end iterator");

    send_for_impl( tx_first, tx_last, std::move( item ), std::move( fn ) );
    return *this;
}

// recv choice without guard
template<typename Rx>
Alt & Alt::recv(
    Rx & rx,
    RxFn< Rx > fn
) noexcept
{
    static_assert( detail::traits::is_rx< Rx >::value,
        "Supplied channel end is not a Rx type");

    recv_impl( rx, std::move( fn ) );
    return *this;
}

// recv choice with guard
template<typename Rx>
Alt & Alt::recv_if(
    bool guard,
    Rx & rx,
    RxFn< Rx > fn
) noexcept
{
    static_assert( detail::traits::is_rx< Rx >::value,
        "Supplied channel end is not a Rx type");

    if ( guard ) {
        recv_impl( rx, std::move( fn ) );
    }
    return *this;
}

// replicated recv choice
template<typename RxIt>
Alt & Alt::recv_for(
    RxIt rx_first,
    RxIt rx_last,
    RxFn< EndT< RxIt > > fn
) noexcept
{
    static_assert( detail::traits::is_rx_iterator< RxIt >::value,
        "Supplied channel end iterator is not a Rx channel end iterator");

    recv_for_impl( rx_first, rx_last, std::move( fn ) );
    return *this;
}

// replicated recv choice with guard
template<typename RxIt>
Alt & Alt::recv_for_if(
    bool guard,
    RxIt rx_first,
    RxIt rx_last,
    RxFn< EndT< RxIt > > fn
) noexcept
{
    static_assert( detail::traits::is_rx_iterator< RxIt >::value,
        "Supplied channel end iterator is not a Rx channel end iterator");

    if ( guard ) {
        recv_for_impl( rx_first, rx_last, std::move( fn ) );
    }
    return *this;
}

// timeout without guard
template<typename Timer>
Alt & Alt::timeout(
    Timer const & timer,
    TimerFn fn
) noexcept
{
    static_assert( detail::traits::is_timer< Timer >::value,
        "Supplied timer is not of a timer type.");

    timeout_impl( timer, std::move( fn ) );
    return *this;
}

// timeout with guard
template<typename Timer>
Alt & Alt::timeout_if(
    bool guard,
    Timer const & timer,
    TimerFn fn
) noexcept
{
    static_assert( detail::traits::is_timer< Timer >::value,
        "Supplied timer is not of a timer type.");

    if ( guard ) {
        timeout_impl( timer, std::move( fn ) );
    }
    return *this;
}

PROXC_NAMESPACE_END

