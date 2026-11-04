#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <chrono>
#include <cstdint>
#include "decklink_audio_write_stats_test.hpp"
#include "utils/decklink_audio_write_stats.hpp"

using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( decklink_audio_write_stats_test );

using clock_type = DecklinkAudioWriteStats::clock;

static const clock_type::time_point T0{seconds(100)};
static const microseconds FRAME{41667};

static void write(DecklinkAudioWriteStats &stats, clock_type::time_point entered, clock_type::duration call)
{
        stats.write_entered(entered);
        stats.write_call_started(entered);
        stats.write_finished(2700, entered + call);
}

decklink_audio_write_stats_test::decklink_audio_write_stats_test()
{
}

decklink_audio_write_stats_test::~decklink_audio_write_stats_test()
{
}

void
decklink_audio_write_stats_test::setUp()
{
}

void
decklink_audio_write_stats_test::tearDown()
{
}

void
decklink_audio_write_stats_test::test_steady_writes_are_not_slow()
{
        DecklinkAudioWriteStats stats;
        for (int i = 0; i < 48; ++i) {
                write(stats, T0 + i * FRAME, microseconds(200));
        }

        CPPUNIT_ASSERT_EQUAL(uint64_t{48}, stats.get_writes());
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_slow_writes());
}

void
decklink_audio_write_stats_test::test_long_gap_is_slow()
{
        DecklinkAudioWriteStats stats;
        write(stats, T0, microseconds(200));
        write(stats, T0 + FRAME, microseconds(200));
        write(stats, T0 + FRAME + milliseconds(170), microseconds(200));

        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_slow_writes());
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, stats.get_slow_writes_logged());
}

void
decklink_audio_write_stats_test::test_video_calls_during_write()
{
        DecklinkAudioWriteStats stats;
        stats.video_call_started();
        stats.write_entered(T0);
        stats.write_call_started(T0);
        stats.video_call_ended(milliseconds(1));
        stats.video_call_started();
        stats.video_call_ended(milliseconds(1));
        stats.write_finished(2700, T0 + milliseconds(50));

        CPPUNIT_ASSERT_EQUAL(uint64_t{2}, stats.get_video_calls_during_write());

        stats.video_call_started();
        stats.video_call_ended(milliseconds(1));
        write(stats, T0 + FRAME, microseconds(200));

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, stats.get_video_calls_during_write());
}

void
decklink_audio_write_stats_test::test_slow_write_logging_capped_per_report()
{
        DecklinkAudioWriteStats stats;
        auto t = T0;
        write(stats, t, microseconds(200));
        for (uint64_t i = 0; i < DecklinkAudioWriteStats::MAX_LOGGED_PER_REPORT + 6; ++i) {
                t += milliseconds(100);
                write(stats, t, microseconds(200));
        }

        CPPUNIT_ASSERT_EQUAL(DecklinkAudioWriteStats::MAX_LOGGED_PER_REPORT + 6, stats.get_slow_writes());
        CPPUNIT_ASSERT_EQUAL(DecklinkAudioWriteStats::MAX_LOGGED_PER_REPORT, stats.get_slow_writes_logged());

        t += seconds(11);
        write(stats, t, microseconds(200));
        t += milliseconds(100);
        write(stats, t, microseconds(200));

        CPPUNIT_ASSERT_EQUAL(DecklinkAudioWriteStats::MAX_LOGGED_PER_REPORT + 1, stats.get_slow_writes_logged());
}

#endif // defined HAVE_CPPUNIT
