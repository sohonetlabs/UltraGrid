#ifndef AUDIO_WRITE_QUEUE_TEST_HPP_9D2F4B71_3A6C_4E18_B5F0_1C8E7A3D6B92
#define AUDIO_WRITE_QUEUE_TEST_HPP_9D2F4B71_3A6C_4E18_B5F0_1C8E7A3D6B92

#include <cppunit/extensions/HelperMacros.h>

class audio_write_queue_test : public CPPUNIT_NS::TestFixture
{
  CPPUNIT_TEST_SUITE( audio_write_queue_test );
  CPPUNIT_TEST( test_frames_written_in_order );
  CPPUNIT_TEST( test_push_does_not_wait_for_writer );
  CPPUNIT_TEST( test_full_queue_drops_oldest );
  CPPUNIT_TEST( test_clear_drops_pending_frames );
  CPPUNIT_TEST( test_held_frames_wait_for_release );
  CPPUNIT_TEST( test_release_keeps_newest_within_limit );
  CPPUNIT_TEST( test_release_with_no_room_drops_all );
  CPPUNIT_TEST_SUITE_END();

public:
  audio_write_queue_test();
  ~audio_write_queue_test();
  void setUp();
  void tearDown();

  void test_frames_written_in_order();
  void test_push_does_not_wait_for_writer();
  void test_full_queue_drops_oldest();
  void test_clear_drops_pending_frames();
  void test_held_frames_wait_for_release();
  void test_release_keeps_newest_within_limit();
  void test_release_with_no_room_drops_all();
};

#endif // !defined AUDIO_WRITE_QUEUE_TEST_HPP_9D2F4B71_3A6C_4E18_B5F0_1C8E7A3D6B92
