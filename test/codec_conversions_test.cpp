#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#ifdef HAVE_CPPUNIT

#include <cppunit/config/SourcePrefix.h>
#include <list>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "codec_conversions_test.h"
#include "video_codec.h"
#include "video_capture/testcard_common.h"

using std::default_random_engine;
using std::list;
using std::pair;
using std::string;
using std::to_string;
using std::ostringstream;
using std::vector;

// Registers the fixture into the 'registry'
CPPUNIT_TEST_SUITE_REGISTRATION( codec_conversions_test );

codec_conversions_test::codec_conversions_test()
{
}

codec_conversions_test::~codec_conversions_test()
{
}

void
codec_conversions_test::setUp()
{
}

void
codec_conversions_test::tearDown()
{
}

void
codec_conversions_test::test_testcard_uyvy_to_i420()
{
        list<pair<size_t,size_t>> sizes = { {1, 2}, {2, 1}, { 16, 1}, {16, 16}, {127, 255} };
        for (auto &i : sizes) {
                size_t size_x = i.first;
                size_t size_y = i.second;
                /// @todo Check also if chroma is horizontally interpolated in UV planes
                unsigned char uyvy_pattern[4] = { 'u', 'y', 'v', 'Y' };
                size_t uyvy_buf_size = ((size_x + 1) & ~1) * 2 * size_y;

                unsigned char uyvy_buf[uyvy_buf_size];
                for (size_t i = 0; i < size_y * 2 * ((size_x + 1) & ~1); ++i) {
                        uyvy_buf[i] = uyvy_pattern[i % 4];
                }

                auto *i420_buf = (unsigned char *) malloc(vc_get_datalen(size_x, size_y, I420));
                testcard_convert_buffer(UYVY, I420, i420_buf, uyvy_buf, size_x, size_y);
                unsigned char *y_ptr = i420_buf;
                for (size_t i = 0; i < size_y; ++i) {
                        for (size_t j = 0; j < size_x; ++j) {
                                ostringstream oss;
                                unsigned char expected = uyvy_pattern[((2 * j + 1) % 4)];
                                unsigned char actual = *y_ptr++;
                                oss << size_x << "X" << size_y << ": [" << i << ", " << j << "] expected " << expected << ", actual: " << actual << "\n";
                                CPPUNIT_ASSERT_EQUAL_MESSAGE(oss.str(), expected, actual);
                        }
                }
                // U
                unsigned char *u_ptr = i420_buf + size_x * size_y;
                for (size_t i = 0; i < (size_y + 1) / 2; ++i) {
                        for (size_t j = 0; j < (size_x + 1) / 2; ++j) {
                                ostringstream oss;
                                unsigned char expected = 'u';
                                unsigned char actual = *u_ptr++;
                                oss << "[" << i << ", " << j << "] expected " << expected << ", actual: " << actual << "\n";
                                CPPUNIT_ASSERT_EQUAL_MESSAGE(oss.str(), expected, actual);
                        }
                }
                // V
                unsigned char *v_ptr = i420_buf + size_x * size_y + ((size_x + 1) / 2) * ((size_y + 1) / 2);
                for (size_t i = 0; i < (size_y + 1) / 2; ++i) {
                        for (size_t j = 0; j < (size_x + 1) / 2; ++j) {
                                ostringstream oss;
                                unsigned char expected = 'v';
                                unsigned char actual = *v_ptr++;
                                oss << "[" << i << ", " << j << "] expected " << expected << ", actual: " << actual << "\n";
                                CPPUNIT_ASSERT_EQUAL_MESSAGE(oss.str(), expected, actual);
                        }
                }
                free(i420_buf);
        }
}

/**
 * Round-trips 8-bit RGBA -> R12L (12-bit packed RGB) -> 8-bit RGBA. Since RGBA
 * values are left-shifted by 4 into the 12-bit slots and the reverse decoder
 * takes the top 8 bits, RGB channels must match exactly (alpha is discarded).
 */
void
codec_conversions_test::test_rgba_r12l_roundtrip()
{
        constexpr int width  = 64; // must be multiple of R12L block (8 px)
        constexpr int height = 4;
        constexpr size_t rgba_len = static_cast<size_t>(width) * height * 4;
        const size_t r12l_len = vc_get_datalen(width, height, R12L);

        decoder_t rgba_to_r12l = get_decoder_from_to(RGBA, R12L);
        decoder_t r12l_to_rgba = get_decoder_from_to(R12L, RGBA);
        CPPUNIT_ASSERT(rgba_to_r12l != nullptr);
        CPPUNIT_ASSERT(r12l_to_rgba != nullptr);

        vector<unsigned char> rgba_in(rgba_len);
        vector<unsigned char> r12l_buf(r12l_len);
        vector<unsigned char> rgba_out(rgba_len);

        auto check_roundtrip = [&](const string &label) {
                rgba_to_r12l(r12l_buf.data(), rgba_in.data(),
                                static_cast<int>(r12l_len), 0, 8, 16);
                r12l_to_rgba(rgba_out.data(), r12l_buf.data(),
                                static_cast<int>(rgba_len), 0, 8, 16);

                for (int i = 0; i < width * height; ++i) {
                        for (int j = 0; j < 3; ++j) {
                                ostringstream oss;
                                oss << label << " pixel " << i << " channel "
                                        << j;
                                CPPUNIT_ASSERT_EQUAL_MESSAGE(oss.str(),
                                                rgba_in[4 * i + j],
                                                rgba_out[4 * i + j]);
                        }
                }
        };

        for (size_t i = 0; i < rgba_len; ++i) {
                rgba_in[i] = static_cast<unsigned char>(i & 0xFF);
        }
        check_roundtrip("ramp");

        const unsigned char pattern[4] = { 0xFF, 0x00, 0x00, 0xFF };
        for (size_t i = 0; i < rgba_len; ++i) {
                rgba_in[i] = pattern[i % 4];
        }
        check_roundtrip("pattern");

        default_random_engine rand_gen;
        for (size_t i = 0; i < rgba_len; ++i) {
                rgba_in[i] = static_cast<unsigned char>(rand_gen() & 0xFF);
        }
        check_roundtrip("random");
}

#endif // defined HAVE_CPPUNIT
