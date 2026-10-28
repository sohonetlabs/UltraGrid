#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>
#include "stage_latency_stats_test.hpp"
#include "utils/stage_latency_stats.hpp"

using std::chrono::microseconds;
using std::chrono::seconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( stage_latency_stats_test );

static const StageLatencyStats::clock::time_point T0{seconds(100)};

stage_latency_stats_test::stage_latency_stats_test()
{
}

stage_latency_stats_test::~stage_latency_stats_test()
{
}

void
stage_latency_stats_test::setUp()
{
}

void
stage_latency_stats_test::tearDown()
{
}

void
stage_latency_stats_test::test_min_mean_max()
{
        StageLatencyStats stats{"Test stats"};
        stats.record(microseconds(10000), 25, T0);
        stats.record(microseconds(30000), 25, T0 + microseconds(40000));
        stats.record(microseconds(20000), 25, T0 + microseconds(80000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{3}, stats.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_over_frame_time());
        CPPUNIT_ASSERT(microseconds(30000) == stats.get_latency_max());
        CPPUNIT_ASSERT(microseconds(20000) == stats.get_latency_mean());
        CPPUNIT_ASSERT(microseconds(10000) == stats.get_latency_min());
}

void
stage_latency_stats_test::test_over_frame_time_boundary()
{
        StageLatencyStats stats{"Test stats"};
        stats.record(microseconds(40000), 25, T0);
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_over_frame_time());

        stats.record(microseconds(40001), 25, T0 + microseconds(40000));
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_over_frame_time());
}

void
stage_latency_stats_test::test_record_follows_stream_fps()
{
        StageLatencyStats stats{"Test stats"};
        stats.record(microseconds(30000), 25, T0);
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_over_frame_time());

        stats.record(microseconds(30000), 50, T0 + seconds(2));
        CPPUNIT_ASSERT_EQUAL(uint64_t{2}, stats.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_over_frame_time());
}

void
stage_latency_stats_test::test_unknown_fps_is_not_classified()
{
        StageLatencyStats stats{"Test stats"};
        stats.record(seconds(1), 0, T0);
        stats.record(microseconds(1000), 0, T0 + microseconds(40000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{2}, stats.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_over_frame_time());
        CPPUNIT_ASSERT(microseconds(1000000) == stats.get_latency_max());
        CPPUNIT_ASSERT(microseconds(1000) == stats.get_latency_min());
}

void
stage_latency_stats_test::test_report_resets_period()
{
        StageLatencyStats stats{"Test stats"};
        stats.record(microseconds(70000), 25, T0);
        auto last = T0;
        while (last - T0 <= StageLatencyStats::REPORT_INTERVAL) {
                last += microseconds(40000);
                stats.record(microseconds(10000), 25, last);
        }
        stats.record(microseconds(20000), 25, last + microseconds(40000));

        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_over_frame_time());
        CPPUNIT_ASSERT(microseconds(20000) == stats.get_latency_max());
        CPPUNIT_ASSERT(microseconds(20000) == stats.get_latency_mean());
        CPPUNIT_ASSERT(microseconds(20000) == stats.get_latency_min());
}

void
stage_latency_stats_test::test_concurrent_records()
{
        constexpr int THREADS = 4;
        constexpr int RECORDS = 1000;
        StageLatencyStats stats{"Test stats"};
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&stats] {
                        for (int i = 0; i < RECORDS; ++i) {
                                stats.record(microseconds(50000), 25, T0);
                        }
                });
        }
        for (auto &thread : threads) {
                thread.join();
        }

        CPPUNIT_ASSERT_EQUAL(uint64_t{THREADS * RECORDS}, stats.get_frames());
        CPPUNIT_ASSERT_EQUAL(uint64_t{THREADS * RECORDS}, stats.get_over_frame_time());
}

#endif // defined HAVE_CPPUNIT
