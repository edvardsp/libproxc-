//          Copyright Edvard Severin Pettersen 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          https://www.boost.org/LICENSE_1_0.txt)

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>

#include <proxc.hpp>

using namespace proxc;

constexpr std::size_t NUM_WORKERS = 8;

constexpr std::size_t ROUNDS = 100;

constexpr std::size_t MAX_ITER = 255;

constexpr double XMIN = -2.1;
constexpr double XMAX =  1.0;
constexpr double YMIN = -1.3;
constexpr double YMAX =  1.3;

struct MandelbrotData
{
    std::size_t line;
    std::vector< double > values;

    MandelbrotData() = default;
    // make non-copyable
    MandelbrotData( MandelbrotData const & ) = delete;
    MandelbrotData & operator = ( MandelbrotData const & ) = delete;
    // make moveable
    MandelbrotData( MandelbrotData && ) = default;
    MandelbrotData & operator = ( MandelbrotData && ) = default;
};

using LineChan = Chan< std::size_t >;
using DataChan = Chan< MandelbrotData >;

inline bool point_predicate( const double x, const double y ) noexcept
{
    return ( x * x + y * y ) < 4.;
}

void mandelbrot( const std::size_t dim, LineChan::Rx line_rx, DataChan::Tx data_tx )
{
    const double integral_x = (XMAX - XMIN) / static_cast< double >( dim );
    const double integral_y = (YMAX - YMIN) / static_cast< double >( dim );

    for ( auto line : line_rx ) {
        MandelbrotData data = { line, std::vector< double >( dim ) };

        double y = YMIN + line * integral_y;
        double x = XMIN;

        for ( std::size_t x_coord = 0; x_coord < dim; ++x_coord ) {
            double x1 = 0., y1 = 0.;
            std::size_t loop_count = 0;
            while ( loop_count < MAX_ITER && point_predicate( x1, y1 ) ) {
                ++loop_count;
                double x1_new = x1 * x1 - y1 * y1 + x;
                y1 = 2 * x1 * y1 + y;
                x1 = x1_new;
            }

            double value = static_cast< double >( loop_count ) / static_cast< double >( MAX_ITER );
            data.values[ x_coord ] = value;
            x += integral_x;
        }

        data_tx << std::move( data );
    }
}

void producer( const std::size_t dim, std::array< LineChan::Tx, NUM_WORKERS > line_txs )
{
    for ( std::size_t line = 0; line < dim; ++line ) {
        Alt()
            .send_for( line_txs.begin(), line_txs.end(), line )
            .select();
    }
}

void consumer( const std::size_t dim, std::array< DataChan::Rx, NUM_WORKERS > data_rxs )
{
    std::vector< std::vector< double > > results( dim );
    for ( std::size_t i = 0; i < dim; ++i ) {
        Alt()
            .recv_for( data_rxs.begin(), data_rxs.end(),
                [&results]( MandelbrotData data ){
                    results[ data.line ] = std::move( data.values );
                } )
            .select();
    }
}

void mandelbrot_program( std::size_t dim )
{
    std::size_t sum = 0;

    for ( std::size_t round = 0; round < ROUNDS; ++round ) {

        ChanArr< std::size_t, NUM_WORKERS >    line_chs;
        ChanArr< MandelbrotData, NUM_WORKERS > data_chs;

        std::vector< Process > workers;
        workers.reserve( NUM_WORKERS );
        for ( std::size_t i = 0; i < NUM_WORKERS; ++i ) {
            workers.emplace_back( mandelbrot, dim,
                line_chs[i].move_rx(),
                data_chs[i].move_tx() );
        }

        auto start = std::chrono::system_clock::now();
        parallel(
            proc_for( workers.begin(), workers.end() ),
            proc( producer, dim, line_chs.collect_tx() ),
            proc( consumer, dim, data_chs.collect_rx() )
        );
        auto stop = std::chrono::system_clock::now();

        std::chrono::duration< std::size_t, std::nano > diff = stop - start;
        sum += diff.count();
    }

    std::cout << dim << ", " << sum / ROUNDS << std::endl;
}

int main()
{
    constexpr std::size_t dim = 100;
    mandelbrot_program( dim );
    return 0;
}

