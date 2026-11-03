#ifndef OUTPUT_PACER_TEST_HPP_7B2E4D91_5C3A_4F68_8E1D_9A6C0B3F5E27
#define OUTPUT_PACER_TEST_HPP_7B2E4D91_5C3A_4F68_8E1D_9A6C0B3F5E27

#include <cppunit/extensions/HelperMacros.h>

class output_pacer_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( output_pacer_test );
  CPPUNIT_TEST( test_steady_stream );
  CPPUNIT_TEST( test_slow_decode_keeps_cadence );
  CPPUNIT_TEST( test_stall_primes_again );
  CPPUNIT_TEST( test_burst_drops_excess );
  CPPUNIT_TEST( test_slow_stream_clock );
  CPPUNIT_TEST( test_fast_stream_clock );
  CPPUNIT_TEST( test_reset_primes_again );
  CPPUNIT_TEST( test_delay );
  CPPUNIT_TEST_SUITE_END();

public:
  output_pacer_test();
  ~output_pacer_test();
  void setUp();
  void tearDown();

  void test_steady_stream();
  void test_slow_decode_keeps_cadence();
  void test_stall_primes_again();
  void test_burst_drops_excess();
  void test_slow_stream_clock();
  void test_fast_stream_clock();
  void test_reset_primes_again();
  void test_delay();
};

#endif // !defined OUTPUT_PACER_TEST_HPP_7B2E4D91_5C3A_4F68_8E1D_9A6C0B3F5E27
