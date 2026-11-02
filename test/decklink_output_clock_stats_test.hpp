#ifndef DECKLINK_OUTPUT_CLOCK_STATS_TEST_HPP_2C8E5A17_9D4B_4F06_B1E3_7A0F6C9D2E84
#define DECKLINK_OUTPUT_CLOCK_STATS_TEST_HPP_2C8E5A17_9D4B_4F06_B1E3_7A0F6C9D2E84

#include <cppunit/extensions/HelperMacros.h>

class decklink_output_clock_stats_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( decklink_output_clock_stats_test );
  CPPUNIT_TEST( test_matched_clocks );
  CPPUNIT_TEST( test_slow_stream_repeats );
  CPPUNIT_TEST( test_fast_stream_skips );
  CPPUNIT_TEST( test_jitter_at_boundary );
  CPPUNIT_TEST( test_blocking_call_counts_shown_slot );
  CPPUNIT_TEST( test_stall_is_not_a_repeat );
  CPPUNIT_TEST( test_stall_restarts_drift );
  CPPUNIT_TEST( test_lasting_jump_reseeds_drift );
  CPPUNIT_TEST( test_reset_forgets_slot );
  CPPUNIT_TEST( test_scheduled_mode );
  CPPUNIT_TEST( test_card_vs_host );
  CPPUNIT_TEST( test_drift_needs_minimum_span );
  CPPUNIT_TEST_SUITE_END();

public:
  decklink_output_clock_stats_test();
  ~decklink_output_clock_stats_test();
  void setUp();
  void tearDown();

  void test_matched_clocks();
  void test_slow_stream_repeats();
  void test_fast_stream_skips();
  void test_jitter_at_boundary();
  void test_blocking_call_counts_shown_slot();
  void test_stall_is_not_a_repeat();
  void test_stall_restarts_drift();
  void test_lasting_jump_reseeds_drift();
  void test_reset_forgets_slot();
  void test_scheduled_mode();
  void test_card_vs_host();
  void test_drift_needs_minimum_span();
};

#endif // !defined DECKLINK_OUTPUT_CLOCK_STATS_TEST_HPP_2C8E5A17_9D4B_4F06_B1E3_7A0F6C9D2E84
