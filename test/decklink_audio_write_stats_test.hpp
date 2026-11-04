#ifndef DECKLINK_AUDIO_WRITE_STATS_TEST_HPP_4C7B1E93_2A6F_4D58_B0E3_9F1A5D2C8E76
#define DECKLINK_AUDIO_WRITE_STATS_TEST_HPP_4C7B1E93_2A6F_4D58_B0E3_9F1A5D2C8E76

#include <cppunit/extensions/HelperMacros.h>

class decklink_audio_write_stats_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( decklink_audio_write_stats_test );
  CPPUNIT_TEST( test_steady_writes_are_not_slow );
  CPPUNIT_TEST( test_long_gap_is_slow );
  CPPUNIT_TEST( test_video_calls_during_write );
  CPPUNIT_TEST( test_slow_write_logging_capped_per_report );
  CPPUNIT_TEST_SUITE_END();

public:
  decklink_audio_write_stats_test();
  ~decklink_audio_write_stats_test();
  void setUp();
  void tearDown();

  void test_steady_writes_are_not_slow();
  void test_long_gap_is_slow();
  void test_video_calls_during_write();
  void test_slow_write_logging_capped_per_report();
};

#endif // !defined DECKLINK_AUDIO_WRITE_STATS_TEST_HPP_4C7B1E93_2A6F_4D58_B0E3_9F1A5D2C8E76
