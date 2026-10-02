#ifndef FRAME_INTERVAL_STATS_TEST_HPP_3F1D6B52_8C0A_4E1F_A2D7_6E9B4C5A7F10
#define FRAME_INTERVAL_STATS_TEST_HPP_3F1D6B52_8C0A_4E1F_A2D7_6E9B4C5A7F10

#include <cppunit/extensions/HelperMacros.h>

class frame_interval_stats_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( frame_interval_stats_test );
  CPPUNIT_TEST( test_nominal_cadence );
  CPPUNIT_TEST( test_long_and_short_intervals );
  CPPUNIT_TEST( test_threshold_boundaries );
  CPPUNIT_TEST( test_reconfigure_skips_interval );
  CPPUNIT_TEST( test_record_follows_stream_fps );
  CPPUNIT_TEST( test_unknown_fps_is_not_classified );
  CPPUNIT_TEST( test_report_resets_period );
  CPPUNIT_TEST_SUITE_END();

public:
  frame_interval_stats_test();
  ~frame_interval_stats_test();
  void setUp();
  void tearDown();

  void test_nominal_cadence();
  void test_long_and_short_intervals();
  void test_threshold_boundaries();
  void test_reconfigure_skips_interval();
  void test_record_follows_stream_fps();
  void test_unknown_fps_is_not_classified();
  void test_report_resets_period();
};

#endif // !defined FRAME_INTERVAL_STATS_TEST_HPP_3F1D6B52_8C0A_4E1F_A2D7_6E9B4C5A7F10
