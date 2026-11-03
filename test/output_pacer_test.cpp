#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include "output_pacer_test.hpp"
#include "utils/output_pacer.hpp"

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( output_pacer_test );

using clock_type = OutputPacer::clock;

static const clock_type::time_point T0{seconds(100)};
static const double FPS = 24;
static const clock_type::duration FRAME = duration_cast<clock_type::duration>(std::chrono::duration<double>(1.0 / FPS));

namespace {
/// The display thread's loop: frames are decoded at `arrivals`, copied out and handed to the display.
struct Simulation {
        static constexpr clock_type::duration COPY = milliseconds(3);
        static constexpr clock_type::duration PUTF = milliseconds(12);

        std::deque<clock_type::time_point> arrivals;
        size_t queued = 0;
        clock_type::time_point now = T0;
        std::vector<clock_type::time_point> handoffs;

        void decode_every(clock_type::duration interval, int frames, clock_type::time_point from) {
                for (int i = 0; i < frames; ++i) {
                        arrivals.push_back(from + i * interval);
                }
        }

        void advance(clock_type::time_point t) {
                now = std::max(now, t);
                while (!arrivals.empty() && arrivals.front() <= now) {
                        arrivals.pop_front();
                        queued++;
                }
        }

        void run(OutputPacer &pacer) {
                while (true) {
                        advance(now);
                        while (pacer.is_priming(queued)) {
                                if (arrivals.empty()) {
                                        return;
                                }
                                advance(arrivals.front());
                        }
                        queued -= pacer.excess(queued);
                        if (queued == 0) {
                                if (arrivals.empty()) {
                                        return;
                                }
                                advance(arrivals.front());
                        }
                        queued--;
                        advance(now + COPY);
                        advance(pacer.due(queued, now));
                        handoffs.push_back(now);
                        advance(now + PUTF);
                }
        }

        /// Largest distance of any hand-off interval from one frame time.
        clock_type::duration worst_interval_error(size_t from = 1) const {
                clock_type::duration worst{};
                for (size_t i = std::max<size_t>(from, 1); i < handoffs.size(); ++i) {
                        auto err = handoffs[i] - handoffs[i - 1] - FRAME;
                        worst = std::max(worst, err < clock_type::duration::zero() ? -err : err);
                }
                return worst;
        }
};
}

output_pacer_test::output_pacer_test()
{
}

output_pacer_test::~output_pacer_test()
{
}

void
output_pacer_test::setUp()
{
}

void
output_pacer_test::tearDown()
{
}

void
output_pacer_test::test_steady_stream()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        Simulation sim;
        sim.decode_every(FRAME, 24 * 60, T0);
        sim.run(pacer);

        CPPUNIT_ASSERT_EQUAL(uint64_t{24 * 60}, pacer.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_underruns());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_dropped());
        CPPUNIT_ASSERT(sim.worst_interval_error() < microseconds(10));
}

void
output_pacer_test::test_slow_decode_keeps_cadence()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        Simulation sim;
        sim.decode_every(FRAME, 24 * 60, T0);
        sim.arrivals[600] += milliseconds(47);
        sim.arrivals[601] = std::max(sim.arrivals[601], sim.arrivals[600]);
        sim.run(pacer);

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_underruns());
        CPPUNIT_ASSERT(sim.worst_interval_error() < microseconds(10));
}

void
output_pacer_test::test_stall_primes_again()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        Simulation sim;
        sim.decode_every(FRAME, 24 * 30, T0);
        sim.decode_every(FRAME, 24 * 30, sim.arrivals.back() + seconds(2));
        sim.run(pacer);

        CPPUNIT_ASSERT_EQUAL(uint64_t{24 * 60}, pacer.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, pacer.get_underruns());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_dropped());
        CPPUNIT_ASSERT(sim.worst_interval_error(24 * 30 + 2) < microseconds(10));
}

void
output_pacer_test::test_burst_drops_excess()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        Simulation sim;
        sim.decode_every(FRAME, 24 * 10, T0);
        sim.decode_every(milliseconds(1), 8, sim.arrivals.back() + milliseconds(30));
        sim.decode_every(FRAME, 24 * 10, sim.arrivals.back() + FRAME);
        sim.run(pacer);

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_underruns());
        CPPUNIT_ASSERT(pacer.get_dropped() > 0);
        CPPUNIT_ASSERT_EQUAL(uint64_t{24 * 20 + 8}, pacer.get_frames() + pacer.get_dropped());
}

void
output_pacer_test::test_slow_stream_clock()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        Simulation sim;
        sim.decode_every(duration_cast<clock_type::duration>(FRAME * (1 + 50e-6)), 24 * 3600, T0);
        sim.run(pacer);

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_underruns());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_dropped());
}

void
output_pacer_test::test_fast_stream_clock()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        Simulation sim;
        sim.decode_every(duration_cast<clock_type::duration>(FRAME * (1 - 50e-6)), 24 * 3600, T0);
        sim.run(pacer);

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_underruns());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, pacer.get_dropped());
}

void
output_pacer_test::test_reset_primes_again()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);
        CPPUNIT_ASSERT(!pacer.is_priming(2));

        pacer.reset(FPS);
        CPPUNIT_ASSERT(pacer.is_priming(1));
        CPPUNIT_ASSERT(!pacer.is_priming(2));
}

void
output_pacer_test::test_delay()
{
        OutputPacer pacer{2};
        pacer.reset(FPS);

        CPPUNIT_ASSERT(milliseconds(83) == pacer.delay());
}

#endif // defined HAVE_CPPUNIT
