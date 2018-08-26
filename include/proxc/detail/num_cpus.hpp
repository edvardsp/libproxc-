//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <thread>

#include <proxc/config.hpp>

PROXC_NAMESPACE_BEGIN
namespace detail {

inline std::size_t num_cpus()
{
    static const std::size_t num = []{
        std::size_t n = static_cast< std::size_t >( std::thread::hardware_concurrency() );
        return ( n > 1 ) ? n : 1;
    }();
    return num;
}

} // namespace detail
PROXC_NAMESPACE_END

