#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include "audio_write_queue_test.hpp"
#include "utils/audio_write_queue.hpp"

using std::chrono::milliseconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( audio_write_queue_test );

static const milliseconds WAIT_LIMIT{2000};

/// Records each written frame's first byte, and can hold the writer until released.
struct RecordingWriter {
        std::mutex lock;
        std::condition_variable changed;
        std::vector<char> written;
        bool held = false;
        bool writing = false;

        void write(const AudioWriteQueue::Frame &frame) {
                std::unique_lock<std::mutex> lk(this->lock);
                this->writing = true;
                this->changed.notify_all();
                this->changed.wait(lk, [this] { return !this->held; });
                this->written.push_back(frame.data.at(0));
                this->writing = false;
                this->changed.notify_all();
        }

        void hold() {
                std::lock_guard<std::mutex> lk(this->lock);
                this->held = true;
        }

        void release() {
                std::lock_guard<std::mutex> lk(this->lock);
                this->held = false;
                this->changed.notify_all();
        }

        bool wait_until_writing() {
                std::unique_lock<std::mutex> lk(this->lock);
                return this->changed.wait_for(lk, WAIT_LIMIT, [this] { return this->writing; });
        }

        bool wait_for_written(size_t count) {
                std::unique_lock<std::mutex> lk(this->lock);
                return this->changed.wait_for(lk, WAIT_LIMIT, [this, count] { return this->written.size() >= count; });
        }
};

/// One sample frame of 4-byte stereo is 8 bytes, so a push of `samples` carries that many.
static void push(AudioWriteQueue &queue, char marker, size_t samples = 1)
{
        std::vector<char> data(samples * 8);
        data.at(0) = marker;
        queue.push(data.data(), data.size(), 4, 2);
}

audio_write_queue_test::audio_write_queue_test()
{
}

audio_write_queue_test::~audio_write_queue_test()
{
}

void
audio_write_queue_test::setUp()
{
}

void
audio_write_queue_test::tearDown()
{
}

void
audio_write_queue_test::test_frames_written_in_order()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 8);
        push(queue, 1);
        push(queue, 2);
        push(queue, 3);

        CPPUNIT_ASSERT(writer.wait_for_written(3));
        CPPUNIT_ASSERT(writer.written == std::vector<char>({1, 2, 3}));
}

void
audio_write_queue_test::test_push_does_not_wait_for_writer()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 8);
        writer.hold();
        push(queue, 1);
        CPPUNIT_ASSERT(writer.wait_until_writing());

        auto start = std::chrono::steady_clock::now();
        push(queue, 2);
        push(queue, 3);
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - start < milliseconds(100));

        writer.release();
        CPPUNIT_ASSERT(writer.wait_for_written(3));
}

void
audio_write_queue_test::test_full_queue_drops_oldest()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 2);
        writer.hold();
        push(queue, 1);
        CPPUNIT_ASSERT(writer.wait_until_writing());

        push(queue, 2);
        push(queue, 3);
        push(queue, 4);
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, queue.get_dropped_frames());

        writer.release();
        CPPUNIT_ASSERT(writer.wait_for_written(3));
        CPPUNIT_ASSERT(writer.written == std::vector<char>({1, 3, 4}));
}

void
audio_write_queue_test::test_clear_drops_pending_frames()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 8);
        writer.hold();
        push(queue, 1);
        CPPUNIT_ASSERT(writer.wait_until_writing());

        push(queue, 2);
        push(queue, 3);
        queue.clear();
        push(queue, 4);

        writer.release();
        CPPUNIT_ASSERT(writer.wait_for_written(2));
        CPPUNIT_ASSERT(writer.written == std::vector<char>({1, 4}));
}

void
audio_write_queue_test::test_held_frames_wait_for_release()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 8);
        queue.hold();
        push(queue, 1);
        push(queue, 2);
        std::this_thread::sleep_for(milliseconds(50));
        {
                std::lock_guard<std::mutex> lk(writer.lock);
                CPPUNIT_ASSERT(writer.written.empty());
        }

        CPPUNIT_ASSERT_EQUAL(size_t{0}, queue.release(SIZE_MAX));
        CPPUNIT_ASSERT(writer.wait_for_written(2));
        CPPUNIT_ASSERT(writer.written == std::vector<char>({1, 2}));
}

void
audio_write_queue_test::test_release_keeps_newest_within_limit()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 8);
        queue.hold();
        push(queue, 1, 2000);
        push(queue, 2, 2000);
        push(queue, 3, 2000);

        CPPUNIT_ASSERT_EQUAL(size_t{4000}, queue.release(2700));
        CPPUNIT_ASSERT(writer.wait_for_written(1));
        std::this_thread::sleep_for(milliseconds(50));
        std::lock_guard<std::mutex> lk(writer.lock);
        CPPUNIT_ASSERT(writer.written == std::vector<char>({3}));
}

void
audio_write_queue_test::test_release_with_no_room_drops_all()
{
        RecordingWriter writer;
        AudioWriteQueue queue([&writer](const AudioWriteQueue::Frame &f) { writer.write(f); }, 8);
        queue.hold();
        push(queue, 1, 2000);

        CPPUNIT_ASSERT_EQUAL(size_t{2000}, queue.release(0));
        push(queue, 2, 2000);
        CPPUNIT_ASSERT(writer.wait_for_written(1));
        CPPUNIT_ASSERT(writer.written == std::vector<char>({2}));
}

#endif // defined HAVE_CPPUNIT
