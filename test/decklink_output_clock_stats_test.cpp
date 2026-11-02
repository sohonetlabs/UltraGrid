#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include "decklink_output_clock_stats_test.hpp"
#include "video_display/decklink_output_clock_stats.hpp"

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( decklink_output_clock_stats_test );

using Completion = DecklinkOutputClockStats::Completion;

static const DecklinkOutputClockStats::clock::time_point T0{seconds(100)};
static const int64_t TIMESCALE = 24000;
static const int64_t TICKS_PER_FRAME = 1000;
static const double FRAME_S = 1.0 / 24;

namespace {
/// A card running at 24p on its own clock and a stream handing it frames on another.
struct Simulation {
        double stream_ppm = 0;
        double card_ppm = 0;
        int64_t card_offset_ticks = 500;
        double host_s = 0;
        std::function<int64_t(int)> jitter_ticks = [](int) { return 0; };

        void run(DecklinkOutputClockStats &stats, int frames) {
                for (int i = 0; i < frames; ++i) {
                        handoff(stats, i);
                        host_s += FRAME_S * (1 - stream_ppm * 1e-6);
                }
        }

        void handoff(DecklinkOutputClockStats &stats, int i) {
                int64_t card = std::llround(host_s * TIMESCALE * (1 + card_ppm * 1e-6)) + card_offset_ticks
                        + jitter_ticks(i);
                DecklinkHardwareClock hw{card, card % TICKS_PER_FRAME, TICKS_PER_FRAME, TIMESCALE};
                auto now = T0 + duration_cast<nanoseconds>(DecklinkOutputClockStats::fractional_seconds(host_s));
                stats.record_handoff(hw, microseconds(500), now);
        }
};
}

decklink_output_clock_stats_test::decklink_output_clock_stats_test()
{
}

decklink_output_clock_stats_test::~decklink_output_clock_stats_test()
{
}

void
decklink_output_clock_stats_test::setUp()
{
}

void
decklink_output_clock_stats_test::tearDown()
{
}

void
decklink_output_clock_stats_test::test_matched_clocks()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation{}.run(stats, 2000);

        CPPUNIT_ASSERT_EQUAL(uint64_t{2000}, stats.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_stalls());
        CPPUNIT_ASSERT_DOUBLES_EQUAL(0.0, stats.get_stream_vs_card_ppm(), 1.0);
}

void
decklink_output_clock_stats_test::test_slow_stream_drift()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.stream_ppm = -1000;
        sim.run(stats, 3000);

        CPPUNIT_ASSERT_DOUBLES_EQUAL(-1000.0, stats.get_stream_vs_card_ppm(), 10.0);
}

void
decklink_output_clock_stats_test::test_fast_stream_drift()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.stream_ppm = 1000;
        sim.run(stats, 3000);

        CPPUNIT_ASSERT_DOUBLES_EQUAL(1000.0, stats.get_stream_vs_card_ppm(), 10.0);
}

void
decklink_output_clock_stats_test::test_jitter_across_boundary_keeps_drift()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.card_offset_ticks = 995;
        sim.jitter_ticks = [](int i) { return i % 2 == 0 ? 200 : -200; };
        sim.run(stats, 24 * 120);

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_stalls());
        CPPUNIT_ASSERT_DOUBLES_EQUAL(0.0, stats.get_stream_vs_card_ppm(), 2.0);
}

void
decklink_output_clock_stats_test::test_stall_is_counted()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.run(stats, 100);
        sim.host_s += 2;
        sim.run(stats, 100);

        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_stalls());
}

void
decklink_output_clock_stats_test::test_stall_restarts_drift()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.stream_ppm = -1000;
        sim.run(stats, 24 * 60);

        sim.host_s += 2;
        sim.card_offset_ticks += 120;
        sim.run(stats, 24 * 10);
        CPPUNIT_ASSERT(std::isnan(stats.get_stream_vs_card_ppm()));

        sim.run(stats, 24 * 30);
        CPPUNIT_ASSERT_DOUBLES_EQUAL(-1000.0, stats.get_stream_vs_card_ppm(), 10.0);
}

void
decklink_output_clock_stats_test::test_lasting_jump_reseeds_drift()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.stream_ppm = -1000;
        sim.run(stats, 24 * 60);

        sim.card_offset_ticks += 360;
        sim.run(stats, 24 * 10);
        CPPUNIT_ASSERT(std::isnan(stats.get_stream_vs_card_ppm()));

        sim.run(stats, 24 * 30);
        CPPUNIT_ASSERT_DOUBLES_EQUAL(-1000.0, stats.get_stream_vs_card_ppm(), 10.0);
}

void
decklink_output_clock_stats_test::test_reset_forgets_slot()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.run(stats, 100);

        stats.reset(true);
        sim.card_offset_ticks = -50000;
        sim.run(stats, 100);

        CPPUNIT_ASSERT_EQUAL(uint64_t{200}, stats.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_stalls());
}

void
decklink_output_clock_stats_test::test_scheduled_mode()
{
        DecklinkOutputClockStats stats;
        stats.reset(false);
        Simulation sim;
        sim.stream_ppm = -1000;
        sim.run(stats, 3000);
        stats.record_completion(Completion::completed);
        stats.record_completion(Completion::late);
        stats.record_completion(Completion::late);
        stats.record_completion(Completion::dropped);
        stats.record_completion(Completion::flushed);
        stats.record_handoff_drop(T0);
        stats.record_queue_depth(3);

        CPPUNIT_ASSERT_EQUAL(uint64_t{2}, stats.get_late());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_dropped());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_flushed());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_handoff_drops());
        CPPUNIT_ASSERT_DOUBLES_EQUAL(-1000.0, stats.get_stream_vs_card_ppm(), 10.0);
}

void
decklink_output_clock_stats_test::test_card_vs_host()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.card_ppm = 50;
        sim.run(stats, 24 * 60);

        CPPUNIT_ASSERT_DOUBLES_EQUAL(50.0, stats.get_card_vs_host_ppm(), 1.0);
}

void
decklink_output_clock_stats_test::test_drift_needs_minimum_span()
{
        DecklinkOutputClockStats stats;
        stats.reset(true);
        Simulation sim;
        sim.stream_ppm = -1000;
        sim.run(stats, 24 * 20);

        CPPUNIT_ASSERT(std::isnan(stats.get_stream_vs_card_ppm()));
        CPPUNIT_ASSERT(std::isnan(stats.get_card_vs_host_ppm()));
}

#endif // defined HAVE_CPPUNIT
