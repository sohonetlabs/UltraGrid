#ifndef STAGE_LATENCY_STATS_TEST_HPP_C4E7A9B2_1D5F_4B83_9E26_7A0C3F8D5B14
#define STAGE_LATENCY_STATS_TEST_HPP_C4E7A9B2_1D5F_4B83_9E26_7A0C3F8D5B14

#include <cppunit/extensions/HelperMacros.h>

class stage_latency_stats_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( stage_latency_stats_test );
  CPPUNIT_TEST( test_min_mean_max );
  CPPUNIT_TEST( test_over_frame_time_boundary );
  CPPUNIT_TEST( test_record_follows_stream_fps );
  CPPUNIT_TEST( test_unknown_fps_is_not_classified );
  CPPUNIT_TEST( test_report_resets_period );
  CPPUNIT_TEST( test_concurrent_records );
  CPPUNIT_TEST_SUITE_END();

public:
  stage_latency_stats_test();
  ~stage_latency_stats_test();
  void setUp();
  void tearDown();

  void test_min_mean_max();
  void test_over_frame_time_boundary();
  void test_record_follows_stream_fps();
  void test_unknown_fps_is_not_classified();
  void test_report_resets_period();
  void test_concurrent_records();
};

#endif // !defined STAGE_LATENCY_STATS_TEST_HPP_C4E7A9B2_1D5F_4B83_9E26_7A0C3F8D5B14
