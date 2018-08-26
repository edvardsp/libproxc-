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
namespace hook {

namespace intrusive = boost::intrusive;

template<typename Tag>
struct ListHook
{
private:
    using MemberHook = intrusive::list_member_hook<
        intrusive::tag< Tag >,
        intrusive::link_mode< intrusive::auto_unlink >
    >;
public:
    using Type = MemberHook;
};

template<typename Tag>
struct SetHook
{
private:
    using MemberHook = intrusive::set_member_hook<
        intrusive::tag< Tag >,
        intrusive::link_mode< intrusive::auto_unlink >
    >;
public:
    using Type = MemberHook;
};

struct ReadyTag;
struct WorkTag;
struct WaitTag;
struct SleepTag;
struct TerminatedTag;

using Ready      = ListHook< ReadyTag >::Type;
using Work       = ListHook< WorkTag >::Type;
using Wait       = ListHook< WaitTag >::Type;
using Sleep      = SetHook< SleepTag >::Type;
using Terminated = ListHook< TerminatedTag >::Type;

} // namespace hook
} // namespace detail
PROXC_NAMESPACE_END

