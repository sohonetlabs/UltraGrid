/**
 * @file   video_decompress/cmpto_j2k.cpp
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2013-2019 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @todo
 * Reconfiguration isn't entirely correct - on reconfigure, all frames
 * should be dropped and not copied to framebuffer. However this is usually
 * not an issue because dynamic video change is rare (except switching to
 * another stream, which, however, creates a new decoder).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "utils/misc.h"
#include "video.h"
#include "video_decompress.h"
#include "utils/stage_latency_stats.hpp"
#include "utils/synchronized_queue.h"
#include "utils/thread.h"

#include <cmpto_j2k_dec.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <queue>
#include <utility>

constexpr const int DEFAULT_TILE_LIMIT = 1;
/// maximal size of queue for decompressed frames
constexpr const int DEFAULT_MAX_QUEUE_SIZE = 2;
/// maximal number of concurrently decompressed frames
constexpr const int DEFAULT_MAX_IN_FRAMES = 4;
constexpr const int64_t DEFAULT_MEM_LIMIT = 1000000000LL;
constexpr const char *MOD_NAME = "[J2K dec.] ";

using namespace std;

struct state_decompress_j2k {
        state_decompress_j2k(unsigned int mif)
                : max_in_frames(mif) {}
        cmpto_j2k_dec_ctx *decoder{};
        cmpto_j2k_dec_cfg *settings{};

        struct video_desc desc{};
        codec_t out_codec{};

        mutex lock;
        queue<pair<char *, size_t>> decompressed_frames; ///< buffer, length
        int pitch;
        pthread_t thread_id{};
        unsigned int max_in_frames; ///< maximal frames that can be "in progress"
        unsigned int in_frames{}; ///< actual number of decompressed frames

        unsigned long long int dropped{}; ///< number of dropped frames because queue was full

        StageLatencyStats decode_stats{"J2K decode stats"};

        void (*convert)(unsigned char *dst_buffer,
                unsigned char *src_buffer,
                unsigned int width, unsigned int height){nullptr};
};

#define CHECK_OK(cmd, err_msg, action_fail) { \
        int j2k_error = cmd; \
        if (j2k_error != CMPTO_OK) {\
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << (err_msg) << ": " << cmpto_j2k_dec_get_last_error() << "\n"; \
                action_fail;\
        } \
}

#define NOOP ((void) 0)

static void rg48_to_r12l(unsigned char *dst_buffer,
                unsigned char *src_buffer,
                unsigned int width, unsigned int height)
{
        int src_pitch = vc_get_linesize(width, RG48);
        int dst_len = vc_get_linesize(width, R12L);
        decoder_t vc_copylineRG48toR12L = get_decoder_from_to(RG48, R12L);

        for(unsigned i = 0; i < height; i++){
                vc_copylineRG48toR12L(dst_buffer, src_buffer, dst_len, 0, 0, 0);
                src_buffer += src_pitch;
                dst_buffer += dst_len;
        }
}

/**
 * This function just runs in thread and gets decompressed images from decoder
 * putting them to queue (or dropping if full).
 */
static void *decompress_j2k_worker(void *args) {
    set_thread_name(__func__);
    auto s = (struct state_decompress_j2k *) args;

    while(true) {
        // Create an empty pointer on the stack to collect the image data
        struct cmpto_j2k_dec_img *img;
        // Collect the status from the J2K sdk on whether we collected the image or not.
        int decoded_img_status;
        // Attempt to collect the image from the decoder. If that fails, then loop until it collects it
        CHECK_OK(cmpto_j2k_dec_ctx_get_decoded_img(s->decoder, 1, &img, &decoded_img_status),
                 "Decode image", continue);

        // If we get this far then we have successfully received the image from the decoder, so we can mark
        // the frame as having been removed from the decoder.
        {
            std::lock_guard<std::mutex> lk(s->lock);
            if(s->in_frames) s->in_frames--;
        }

        // Decoder stopped (poison pill)
        if(img == nullptr) {
            break;
        }

        // Do some error handling if the decoder did not output the image correctly
        if(decoded_img_status != CMPTO_J2K_DEC_IMG_OK) {
            // Capture and display the error from J2K sdk
            const char *decoding_error;
            CHECK_OK(cmpto_j2k_dec_img_get_error(img, &decoding_error), "get error status",
                     decoding_error = "(failed)");
            log_msg(LOG_LEVEL_ERROR, "Image decoding failed: %s\n", decoding_error);
            continue;
        }

        void *custom_data;
        size_t custom_data_size;
        StageLatencyStats::clock::time_point pushed_at;
        if (cmpto_j2k_dec_img_get_custom_data(img, &custom_data, &custom_data_size) == CMPTO_OK
                && custom_data_size == sizeof pushed_at) {
            memcpy(&pushed_at, custom_data, sizeof pushed_at);
            auto now = StageLatencyStats::clock::now();
            s->decode_stats.record(now - pushed_at, s->desc.fps, now);
        }

        void *dec_data;
        size_t len;
        // Pull the decoded image data from the J2K sdk. If there is an error destroy the img object and loop again
        CHECK_OK(cmpto_j2k_dec_img_get_samples(img, &dec_data, &len), "Error getting samples",
                 cmpto_j2k_dec_img_destroy(img); continue);

        char *buffer = (char *) malloc(len);
        // If there is a conversion associated with the video format then apply the conversion into the buffer
        if(s->convert) {
            // Apply the conversion and then fetch the new length of the data
            s->convert(reinterpret_cast<unsigned char *>(buffer), (unsigned char *) dec_data,
                       s->desc.width, s->desc.height);
            len = vc_get_linesize(s->desc.width, s->out_codec) * s->desc.height;
        }
            // Copy the decoded data directly into the buffer
        else {
            memcpy(buffer, dec_data, len);
        }

        // Destroy the img object that was fetched from the J2K sdk
        CHECK_OK(cmpto_j2k_dec_img_destroy(img), "Unable to to return processed image", NOOP);

        // Push into the queue before fetching the next frame
        s->decompressed_frames.push({buffer, len});
    }

    return nullptr;
}

ADD_TO_PARAM("j2k-dec-mem-limit", "* j2k-dec-mem-limit=<limit>\n"
                                "  J2K max memory usage in bytes.\n");
ADD_TO_PARAM("j2k-dec-tile-limit", "* j2k-dec-tile-limit=<limit>\n"
                                "  number of tiles decoded at moment (less to reduce latency, more to increase performance, 0 unlimited)\n");
ADD_TO_PARAM("j2k-dec-encoder-queue", "* j2k-encoder-queue=<len>\n"
                                "  max number of frames held by encoder\n");
static void * j2k_decompress_init(void)
{
        struct state_decompress_j2k *s = NULL;
        long long int mem_limit = DEFAULT_MEM_LIMIT;
        unsigned int tile_limit = DEFAULT_TILE_LIMIT;
        unsigned int encoder_in_frames = DEFAULT_MAX_IN_FRAMES;
        int ret;

        if (get_commandline_param("j2k-dec-mem-limit")) {
                mem_limit = unit_evaluate(get_commandline_param("j2k-dec-mem-limit"));
        }

        if (get_commandline_param("j2k-dec-tile-limit")) {
                tile_limit = atoi(get_commandline_param("j2k-dec-tile-limit"));
        }

        if (get_commandline_param("j2k-dec-encoder-queue")) {
                encoder_in_frames = atoi(get_commandline_param("j2k-dec-encoder-queue"));
        }

        const auto *version = cmpto_j2k_dec_get_version();
        LOG(LOG_LEVEL_INFO) << MOD_NAME << "Using codec version: " << (version == nullptr ? "(unknown)" : version->name) << "\n";

        s = new state_decompress_j2k(encoder_in_frames);

        struct cmpto_j2k_dec_ctx_cfg *ctx_cfg;
        CHECK_OK(cmpto_j2k_dec_ctx_cfg_create(&ctx_cfg), "Error creating dec cfg", goto error);
        for (unsigned int i = 0; i < cuda_devices_count; ++i) {
                CHECK_OK(cmpto_j2k_dec_ctx_cfg_add_cuda_device(ctx_cfg, cuda_devices[i], mem_limit, tile_limit),
                                "Error setting CUDA device", goto error);
        }

        CHECK_OK(cmpto_j2k_dec_ctx_create(ctx_cfg, &s->decoder), "Error initializing context",
                        goto error);

        CHECK_OK(cmpto_j2k_dec_ctx_cfg_destroy(ctx_cfg), "Destroy cfg", NOOP);

        CHECK_OK(cmpto_j2k_dec_cfg_create(s->decoder, &s->settings), "Error creating configuration",
                        goto error);

        ret = pthread_create(&s->thread_id, NULL, decompress_j2k_worker, (void *) s);
        assert(ret == 0 && "Unable to create thread");

        return s;

error:
        if (!s) {
                return NULL;
        }
        if (s->settings) {
                cmpto_j2k_dec_cfg_destroy(s->settings);
        }
        if (s->decoder) {
                cmpto_j2k_dec_ctx_destroy(s->decoder);
        }
        delete s;
        return NULL;
}

static struct {
        codec_t ug_codec;
        enum cmpto_sample_format_type cmpto_sf;
        void (*convert)(unsigned char *dst_buffer, unsigned char *src_buffer, unsigned int width, unsigned int height);
} codecs[] = {
        {UYVY, CMPTO_422_U8_P1020, nullptr},
        {v210, CMPTO_422_U10_V210, nullptr},
        {RGB, CMPTO_444_U8_P012, nullptr},
        {BGR, CMPTO_444_U8_P210, nullptr},
        {RGBA, CMPTO_444_U8_P012Z, nullptr},
        {R10k, CMPTO_444_U10U10U10_MSB32BE_P210, nullptr},
        {R12L, CMPTO_444_U12_MSB16LE_P012, rg48_to_r12l},
};

static int j2k_decompress_reconfigure(void *state, struct video_desc desc,
                int rshift, int gshift, int bshift, int pitch, codec_t out_codec)
{
        struct state_decompress_j2k *s = (struct state_decompress_j2k *) state;

        if (out_codec == VIDEO_CODEC_NONE) { // probe format
                s->out_codec = VIDEO_CODEC_NONE;
                s->desc = desc;
                return true;
        }

        if (out_codec == R12L) {
                LOG(LOG_LEVEL_NOTICE) << MOD_NAME << "Decoding to 12-bit RGB.\n";
        }

        enum cmpto_sample_format_type cmpto_sf = (cmpto_sample_format_type) 0;

        for(const auto &codec : codecs){
                if(codec.ug_codec == out_codec){
                        cmpto_sf = codec.cmpto_sf;
                        s->convert = codec.convert;
                        break;
                }
        }

        if (!cmpto_sf) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Unsupported output codec: " <<
                                get_codec_name(out_codec) << "\n";
                abort();
        }

        if (out_codec != RGBA || (rshift == 0 && gshift == 8 && bshift == 16)) {
                CHECK_OK(cmpto_j2k_dec_cfg_set_samples_format_type(s->settings, cmpto_sf),
                                "Error setting sample format type", return false);
        } else { // RGBA with non-standard shift
                if (rshift % 8 != 0 || gshift % 8 != 0 || bshift % 8 != 0) {
                        LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Component shifts not aligned to a "
                                "byte boundary is not supported.\n";
                        return false;
                }
                cmpto_j2k_dec_comp_format fmt[3] = {};
                const int shifts[3] = { rshift, gshift, bshift };
                for (int i = 0; i < 3; ++i) {
                        fmt[i].comp_index = i;
                        fmt[i].data_type = CMPTO_INT8;
                        fmt[i].offset = shifts[i] / 8;
                        fmt[i].stride_x = get_bpp(out_codec);
                        fmt[i].stride_y = vc_get_linesize(desc.width, out_codec);
                        fmt[i].bit_depth = get_bits_per_component(out_codec);
                        fmt[i].bit_shift = 0;
                        fmt[i].is_or_combined = 0;
                        fmt[i].is_signed = 0;
                        fmt[i].sampling_factor_x = 1;
                        fmt[i].sampling_factor_y = 1;
                }

                CHECK_OK(cmpto_j2k_dec_cfg_set_samples_format(s->settings, fmt, 3),
                                "Error setting sample format", return false);
        }

        s->desc = desc;
        s->out_codec = out_codec;
        s->pitch = pitch;

        return true;
}

/**
 * Callback called by the codec when codestream is no longer required.
 */
static void release_cstream(void * custom_data, size_t custom_data_size, const void * codestream, size_t codestream_size)
{
        (void) custom_data; (void) custom_data_size; (void) codestream_size;
        free(const_cast<void *>(codestream));
}

static decompress_status j2k_probe_internal_codec(codec_t in_codec, unsigned char *buffer, size_t len, codec_t *internal_codec) {
        struct cmpto_j2k_dec_comp_info comp_info;
        if (cmpto_j2k_dec_cstream_get_comp_info(buffer, len, 0, &comp_info) != CMPTO_OK) {
                return DECODER_NO_FRAME;
        }

        switch (comp_info.bit_depth) {
        case 8:
                *internal_codec = in_codec == J2K ? UYVY : RGB;
                break;
        case 10:
                *internal_codec = in_codec == J2K ? v210 : R10k;
                break;
        case 12:
                *internal_codec = R12L;
                break;
        default:
                assert("J2K - unsupported RGB bit depth" && 0);
        }

        log_msg(LOG_LEVEL_VERBOSE, "J2K color space: %s\n", get_codec_name(*internal_codec));

        return DECODER_GOT_CODEC;
}

static decompress_status j2k_decompress(void *, unsigned char *, unsigned char *,
                unsigned int, int, struct video_frame_callbacks *, codec_t *)
{
        LOG(LOG_LEVEL_ERROR) << "The J2K decompression module is asynchronous only";
        return DECODER_ERROR;
}

static int j2k_decompress_get_property(void *state, int property, void *val, size_t *len)
{
        UNUSED(state);
        int ret = false;

        switch(property) {
                case DECOMPRESS_PROPERTY_ACCEPTS_CORRUPTED_FRAME:
                        if(*len >= sizeof(int)) {
                                *(int *) val = false;
                                *len = sizeof(int);
                                ret = true;
                        }
                        break;
                case DECOMPRESS_PROPERTY_IS_ASYNC: {
                    bool* is_async = static_cast<bool*>(val);
                    *is_async = true;
                    ret = true;
                    break;
                }
                default:
                        ret = false;
        }

        return ret;
}

static void j2k_decompress_done(void *state)
{
        struct state_decompress_j2k *s = (struct state_decompress_j2k *) state;

        cmpto_j2k_dec_ctx_stop(s->decoder);
        pthread_join(s->thread_id, NULL);
        log_msg(LOG_LEVEL_VERBOSE, "[J2K dec.] Decoder stopped.\n");

        cmpto_j2k_dec_cfg_destroy(s->settings);
        cmpto_j2k_dec_ctx_destroy(s->decoder);

        while (s->decompressed_frames.size() > 0) {
                auto decoded = s->decompressed_frames.front();
                s->decompressed_frames.pop();
                free(decoded.first);
        }

        delete s;
}

static const struct decode_from_to *j2k_decompress_get_decoders() {

        static const struct decode_from_to ret[] = {
                { J2K, VIDEO_CODEC_NONE, VIDEO_CODEC_NONE, 50 },
                { J2KR, VIDEO_CODEC_NONE, VIDEO_CODEC_NONE, 50 },
                { J2K, UYVY, UYVY, 300 },
                { J2K, v210, UYVY, 400 },
                { J2K, v210, v210, 200 }, // prefer decoding to 10-bit
                { J2KR, RGB, RGB, 300 },
                { J2KR, RGB, BGR, 300 },
                { J2KR, RGB, RGBA, 300 },
                { J2KR, R10k, R10k, 200 },
                { J2KR, R10k, RGB, 400 },
                { J2KR, R10k, RGBA, 400 },
                { J2KR, R12L, R12L, 100 }, // prefer RGB decoding to 12-bit
                { J2KR, R12L, R10k, 400 },
                { J2KR, R12L, RGB, 400 },
                { J2KR, R12L, RGBA, 400 },
                { J2K, VIDEO_CODEC_NONE, UYVY, 800 }, // fallback
                { J2KR, VIDEO_CODEC_NONE, RGB, 800 }, // ditto
                { J2KR, VIDEO_CODEC_NONE, RGBA, 800 }, // ditto
                { VIDEO_CODEC_NONE, VIDEO_CODEC_NONE, VIDEO_CODEC_NONE, 0 }
        };
        return ret;
}

static decompress_status j2k_decompress_empty_push(void *state, unsigned char *compressed, unsigned int compressed_len, codec_t *internal_codec) {
        auto s = static_cast<state_decompress_j2k *>(state);
        struct cmpto_j2k_dec_img *img;
        pair<char *, size_t> decoded;
        void *tmp;
        void *custom_data;
        StageLatencyStats::clock::time_point pushed_at;

        // Sent a notice saying there needs to be a reconfiguration completed if the video codecs don't match.
        if (s->out_codec == VIDEO_CODEC_NONE) {
                return j2k_probe_internal_codec(s->desc.color_spec, compressed, compressed_len, internal_codec);
        }

        // Don't let there be too many frames being processed by the decoder at once.
        if (s->in_frames >= s->max_in_frames + 1) {
                // Only bother printing an error for every 10 frames this happens with.
                if (s->dropped++ % 10 == 0) {
                        log_msg(LOG_LEVEL_WARNING, "[J2K dec] Some frames (%llu) dropped.\n", s->dropped);

                }
                goto error;
        }

        // Create the J2K img object
        CHECK_OK(cmpto_j2k_dec_img_create(s->decoder, &img), "Could not create frame", goto error);

        CHECK_OK(cmpto_j2k_dec_img_allocate_custom_data(img, sizeof pushed_at, &custom_data),
                        "Error allocating custom data", cmpto_j2k_dec_img_destroy(img); goto error);
        pushed_at = StageLatencyStats::clock::now();
        memcpy(custom_data, &pushed_at, sizeof pushed_at);

        // Allocate a temporary buffer, copy in the compressed buffer, and align it with the 
        // settings the J2K decoder expects.
        tmp = malloc(compressed_len);
        memcpy(tmp, compressed, compressed_len);
        CHECK_OK(cmpto_j2k_dec_img_set_cstream(img, tmp, compressed_len, &release_cstream),
                        "Error setting cstream", cmpto_j2k_dec_img_destroy(img); goto error);

        // Submit the frame to the decoder.
        CHECK_OK(cmpto_j2k_dec_img_decode(img, s->settings), "Decode image", cmpto_j2k_dec_img_destroy(img); goto error);
        {
                lock_guard<mutex> lk(s->lock);
                s->in_frames++;
        }

        // Response saying that the frame has been "pushed".
        return DECODER_FRAME_PUSHED;
error:
        return DECODER_ERROR;
}

static void j2k_decompress_empty_pop(void *state, decompress_status *status, struct video_frame *display_frame, int tile_index) {
        auto s = static_cast<state_decompress_j2k*>(state);
        
        // Fetch the log for the decompressed frames queue
        unique_lock<mutex> lk(s->lock);
        // Check if there is a frame waiting for us
        if (s->decompressed_frames.empty()) {
                *status = DECODER_NO_FRAME;
                return;
        }
        // If there us a frame waiting for us, pop it from the queue and release the lock.
        auto decoded = s->decompressed_frames.front();
        s->decompressed_frames.pop();
        lk.unlock();

        size_t linesize = vc_get_linesize(s->desc.width, s->out_codec);
        size_t frame_size = linesize * s->desc.height;
        // Perform some validation of the frame.
        if ((decoded.second + 3) / 4 * 4 != frame_size) { // for "RGBA with non-standard shift" (search) it would be (frame_size - 1)
                LOG(LOG_LEVEL_WARNING) << MOD_NAME << "Incorrect decoded size (" << frame_size << " vs. " << decoded.second << ")\n";
        }
        
        // Copy the decompressed frame into the display frame.
        for (size_t i = 0; i < s->desc.height; ++i) {
                memcpy(vf_get_tile(display_frame, tile_index)->data + i * s->pitch, decoded.first + i * linesize, min(linesize, decoded.second - min(decoded.second, i * linesize)));
        }

        free(decoded.first);
        // Respond by saying that a tile has been written to the frame.
        *status = DECODER_GOT_FRAME;
}

static const struct video_decompress_info j2k_decompress_info = {
        j2k_decompress_init,
        j2k_decompress_reconfigure,
        j2k_decompress,
        j2k_decompress_get_property,
        j2k_decompress_done,
        j2k_decompress_get_decoders,
        j2k_decompress_empty_push,
        j2k_decompress_empty_pop
};

REGISTER_MODULE(j2k, &j2k_decompress_info, LIBRARY_CLASS_VIDEO_DECOMPRESS, VIDEO_DECOMPRESS_ABI_VERSION);

