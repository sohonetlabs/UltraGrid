#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include "decklink_reference_status_test.hpp"
#include "video_display/decklink_reference_status.hpp"

using std::chrono::milliseconds;
using std::chrono::seconds;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( decklink_reference_status_test );

static const DecklinkReferenceStatus::clock::time_point T0{seconds(100)};
static const DecklinkReference LOCKED{true, "4K DCI 24"};
static const DecklinkReference UNLOCKED{false, "4K DCI 24"};
static const DecklinkReference NO_REFERENCE{false, ""};

decklink_reference_status_test::decklink_reference_status_test()
{
}

decklink_reference_status_test::~decklink_reference_status_test()
{
}

void
decklink_reference_status_test::setUp()
{
}

void
decklink_reference_status_test::tearDown()
{
}

void
decklink_reference_status_test::test_first_update_is_not_a_change()
{
        DecklinkReferenceStatus status;
        status.reset(1);
        status.update(0, LOCKED, 45, T0);

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, status.get_changes());
}

void
decklink_reference_status_test::test_changes_are_counted()
{
        DecklinkReferenceStatus status;
        status.reset(1);
        status.update(0, LOCKED, 45, T0);
        status.update(0, LOCKED, 46, T0 + seconds(1));
        status.update(0, UNLOCKED, 46, T0 + seconds(2));
        status.update(0, NO_REFERENCE, std::nullopt, T0 + seconds(3));
        status.update(0, NO_REFERENCE, std::nullopt, T0 + seconds(4));

        CPPUNIT_ASSERT_EQUAL(uint64_t{2}, status.get_changes());
}

void
decklink_reference_status_test::test_reset_compares_with_previous_reference()
{
        DecklinkReferenceStatus status;
        status.reset(1);
        status.update(0, LOCKED, 45, T0);

        status.reset(1);
        status.update(0, LOCKED, 45, T0 + seconds(1));
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, status.get_changes());

        status.reset(1);
        status.update(0, UNLOCKED, 45, T0 + seconds(2));
        CPPUNIT_ASSERT_EQUAL(uint64_t{1}, status.get_changes());
}

void
decklink_reference_status_test::test_devices_are_tracked_separately()
{
        DecklinkReferenceStatus status;
        status.reset(2);
        status.update(0, LOCKED, 45, T0);
        status.update(1, UNLOCKED, 45, T0);
        status.update(0, LOCKED, 45, T0 + seconds(1));
        status.update(1, UNLOCKED, 45, T0 + seconds(1));

        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, status.get_changes());
}

void
decklink_reference_status_test::test_poll_interval()
{
        DecklinkReferenceStatus status;
        status.reset(1);
        CPPUNIT_ASSERT(status.poll_due(T0));

        status.update(0, LOCKED, 45, T0);
        CPPUNIT_ASSERT(!status.poll_due(T0 + milliseconds(999)));
        CPPUNIT_ASSERT(status.poll_due(T0 + seconds(1)));

        status.reset(1);
        CPPUNIT_ASSERT(status.poll_due(T0 + milliseconds(1)));
}

void
decklink_reference_status_test::test_describe()
{
        CPPUNIT_ASSERT_EQUAL(std::string("locked (4K DCI 24)"), DecklinkReferenceStatus::describe(LOCKED));
        CPPUNIT_ASSERT_EQUAL(std::string("not locked (4K DCI 24)"), DecklinkReferenceStatus::describe(UNLOCKED));
        CPPUNIT_ASSERT_EQUAL(std::string("none"), DecklinkReferenceStatus::describe(NO_REFERENCE));
}

#endif // defined HAVE_CPPUNIT
