#ifndef CODEC_CONVERSIONS_TEST_H
#define CODEC_CONVERSIONS_TEST_H

#include <cppunit/extensions/HelperMacros.h>
#include <list>

#include "utils/misc.h"

class codec_conversions_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( codec_conversions_test );
  CPPUNIT_TEST( test_testcard_uyvy_to_i420 );
  CPPUNIT_TEST( test_rgba_r12l_roundtrip );
  CPPUNIT_TEST_SUITE_END();

public:
  codec_conversions_test();
  ~codec_conversions_test();
  void setUp();
  void tearDown();

  void test_testcard_uyvy_to_i420();
  void test_rgba_r12l_roundtrip();
};

#endif // defined CODEC_CONVERSIONS_TEST_H
