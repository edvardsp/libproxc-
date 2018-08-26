//          Copyright Oliver Kowalke 2009.
//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <proxc/config.hpp>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

PROXC_NAMESPACE_BEGIN

namespace detail {
namespace queue {

namespace intrusive = boost::intrusive;

template<typename T, typename Hook, Hook T::* Ptr>
using MemberHook = intrusive::member_hook<
    T, Hook, Ptr 
>;

template<typename T, typename Hook, Hook T::* Ptr>
using ListQueue = intrusive::list<
    T,
    MemberHook< T, Hook, Ptr >,
    intrusive::constant_time_size< false > >;

template<typename T, typename Hook, Hook T::* Ptr, typename Fn>
using SetQueue = intrusive::multiset<
    T,
    MemberHook< T, Hook, Ptr >,
    intrusive::constant_time_size< false >,
    intrusive::compare< Fn > >;

} // namespace queue
} // namespace detail

PROXC_NAMESPACE_END

