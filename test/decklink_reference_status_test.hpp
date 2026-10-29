#ifndef DECKLINK_REFERENCE_STATUS_TEST_HPP_7B2E4D19_5C8A_4F63_9E1D_2A6C8F0B3D57
#define DECKLINK_REFERENCE_STATUS_TEST_HPP_7B2E4D19_5C8A_4F63_9E1D_2A6C8F0B3D57

#include <cppunit/extensions/HelperMacros.h>

class decklink_reference_status_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( decklink_reference_status_test );
  CPPUNIT_TEST( test_first_update_is_not_a_change );
  CPPUNIT_TEST( test_changes_are_counted );
  CPPUNIT_TEST( test_reset_compares_with_previous_reference );
  CPPUNIT_TEST( test_devices_are_tracked_separately );
  CPPUNIT_TEST( test_poll_interval );
  CPPUNIT_TEST( test_describe );
  CPPUNIT_TEST_SUITE_END();

public:
  decklink_reference_status_test();
  ~decklink_reference_status_test();
  void setUp();
  void tearDown();

  void test_first_update_is_not_a_change();
  void test_changes_are_counted();
  void test_reset_compares_with_previous_reference();
  void test_devices_are_tracked_separately();
  void test_poll_interval();
  void test_describe();
};

#endif // !defined DECKLINK_REFERENCE_STATUS_TEST_HPP_7B2E4D19_5C8A_4F63_9E1D_2A6C8F0B3D57
