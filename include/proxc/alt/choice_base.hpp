//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <proxc/config.hpp>

#include <proxc/runtime/scheduler.hpp>
#include <proxc/alt/state.hpp>
#include <proxc/alt/sync.hpp>

#include <boost/assert.hpp>
#include <boost/container/small_vector.hpp>

PROXC_NAMESPACE_BEGIN

// forward declaration
class Alt;

namespace channel {

struct AltSync;

} // namespace channel

namespace alt {

class ChoiceBase
{
public:
    enum class Result
    {
        Ok,
        TryLater,
        Failed,
    };

    Alt *    alt_;

    ChoiceBase( Alt * ) noexcept;
    virtual ~ChoiceBase() noexcept {}

    // make non-copyable
    ChoiceBase( ChoiceBase const & ) = delete;
    ChoiceBase & operator = ( ChoiceBase const & ) = delete;

    // make non-movable
    ChoiceBase( ChoiceBase && ) = delete;
    ChoiceBase & operator = ( ChoiceBase && ) = delete;

    // base methods
    bool same_alt( Alt * ) const noexcept;
    bool try_select() noexcept;
    bool try_alt_select() noexcept;
    alt::State get_state() const noexcept;
    bool operator < ( ChoiceBase const & ) const noexcept;

    // interface
    virtual void enter() noexcept = 0;
    virtual void leave() noexcept = 0;
    virtual bool is_ready() const noexcept = 0;

    virtual Result try_complete() noexcept = 0;
    virtual void run_func() noexcept = 0;
};

} // namespace alt
PROXC_NAMESPACE_END

