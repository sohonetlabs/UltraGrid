#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <chrono>
#include <cstdint>
#include "frame_interval_stats_test.hpp"
#include "utils/frame_interval_stats.hpp"

using std::chrono::microseconds;
using std::chrono::seconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( frame_interval_stats_test );

static const FrameIntervalStats::clock::time_point T0{seconds(100)};

frame_interval_stats_test::frame_interval_stats_test()
{
}

frame_interval_stats_test::~frame_interval_stats_test()
{
}

void
frame_interval_stats_test::setUp()
{
}

void
frame_interval_stats_test::tearDown()
{
}

void
frame_interval_stats_test::test_nominal_cadence()
{
        FrameIntervalStats summary{"Test stats"};
        summary.set_fps(25);
        for (int i = 0; i < 5; ++i) {
                summary.record(T0 + i * microseconds(40000));
        }

        CPPUNIT_ASSERT_EQUAL(uint64_t{5}, summary.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_short_intervals());
        CPPUNIT_ASSERT(microseconds(40000) == summary.get_interval_max());
        CPPUNIT_ASSERT(microseconds(40000) == summary.get_interval_min());
}

void
frame_interval_stats_test::test_long_and_short_intervals()
{
        FrameIntervalStats summary{"Test stats"};
        summary.set_fps(25);
        summary.record(T0);
        summary.record(T0 + microseconds(40000));
        summary.record(T0 + microseconds(110000));
        summary.record(T0 + microseconds(120000));
        summary.record(T0 + microseconds(160000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{5}, summary.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_short_intervals());
        CPPUNIT_ASSERT(microseconds(70000) == summary.get_interval_max());
        CPPUNIT_ASSERT(microseconds(10000) == summary.get_interval_min());
}

void
frame_interval_stats_test::test_threshold_boundaries()
{
        FrameIntervalStats summary{"Test stats"};
        summary.set_fps(25);
        summary.record(T0);
        summary.record(T0 + microseconds(60000));
        summary.record(T0 + microseconds(80000));
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_short_intervals());

        summary.record(T0 + microseconds(140001));
        summary.record(T0 + microseconds(160000));
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_short_intervals());
}

void
frame_interval_stats_test::test_reconfigure_skips_interval()
{
        FrameIntervalStats summary{"Test stats"};
        summary.set_fps(25);
        summary.record(T0);
        summary.record(T0 + microseconds(40000));

        summary.set_fps(50);
        summary.record(T0 + seconds(2));
        summary.record(T0 + seconds(2) + microseconds(20000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{4}, summary.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_short_intervals());
        CPPUNIT_ASSERT(microseconds(20000) == summary.get_interval_max());
        CPPUNIT_ASSERT(microseconds(20000) == summary.get_interval_min());
}

void
frame_interval_stats_test::test_record_follows_stream_fps()
{
        FrameIntervalStats summary{"Test stats"};
        summary.record(25, T0);
        summary.record(25, T0 + microseconds(40000));
        summary.record(25, T0 + microseconds(110000));
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_long_intervals());

        summary.record(50, T0 + seconds(2));
        summary.record(50, T0 + seconds(2) + microseconds(20000));
        summary.record(50, T0 + seconds(2) + microseconds(29000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{6}, summary.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_short_intervals());
        CPPUNIT_ASSERT(microseconds(20000) == summary.get_interval_max());
        CPPUNIT_ASSERT(microseconds(9000) == summary.get_interval_min());
}

void
frame_interval_stats_test::test_unknown_fps_is_not_classified()
{
        FrameIntervalStats summary{"Test stats"};
        summary.record(0, T0);
        summary.record(0, T0 + microseconds(1000));
        summary.record(0, T0 + seconds(1));

        CPPUNIT_ASSERT_EQUAL(uint64_t{3}, summary.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_long_intervals());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, summary.get_short_intervals());
        CPPUNIT_ASSERT(microseconds(999000) == summary.get_interval_max());
        CPPUNIT_ASSERT(microseconds(1000) == summary.get_interval_min());
}

void
frame_interval_stats_test::test_report_resets_period()
{
        FrameIntervalStats summary{"Test stats"};
        summary.set_fps(25);
        summary.record(T0);
        summary.record(T0 + microseconds(70000));
        auto last = T0 + microseconds(70000);
        while (last - T0 <= FrameIntervalStats::REPORT_INTERVAL) {
                last += microseconds(40000);
                summary.record(last);
        }
        summary.record(last + microseconds(40000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, summary.get_long_intervals());
        CPPUNIT_ASSERT(microseconds(40000) == summary.get_interval_max());
        CPPUNIT_ASSERT(microseconds(40000) == summary.get_interval_min());
}

#endif // defined HAVE_CPPUNIT
