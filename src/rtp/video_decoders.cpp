/*
 * Copyright (c) 2003-2004 University of Southern California
 * Copyright (c) 2005-2021 CESNET, z. s. p. o.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute.
 *
 * 4. Neither the name of the University nor of the Institute may be used
 *    to endorse or promote products derived from this software without
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
 * @file    video_decoders.cpp
 * @author  Colin Perkins
 * @author  Ladan Gharai
 * @author  Martin Pulec <pulec@cesnet.cz>
 * @ingroup video_rtp_decoder
 *
 * ## Workflow ##
 *
 * Normal workflow through threads is following:
 * 1. decode data from in context of receiving thread with function decode_frame(),
 *    it will decode it to framebuffer, passes it to FEC thread
 * 2. fec_thread() passes frame to decompress thread
 * 3. thread running decompress_thread(), displays the frame
 *
 * ### Uncompressed video (without FEC) ###
 * In step one, the decoder is a linedecoder and framebuffer is the display framebuffer
 *
 * ### Compressed video ###
 * Data is saved to decompress buffer. The decompression itself is done by decompress_thread().
 *
 * ### video with FEC ###
 * Data is saved to FEC buffer. Decoded with fec_thread().
 *
 * ### Encrypted video (without FEC) ###
 * Prior to decoding, packet is decompressed.
 *
 * ### Encrypted video (with FEC) ###
 * After FEC decoding, the whole block is decompressed.
 *
 * ## Probing compressed video codecs ##
 * When video is received, it is probed for an internal format:
 * * compression is reconfigured to (VIDEO_CODEC_NONE, VIDEO_CODEC_NONE) -
 *   internal and out codecs are set to 0
 * * if any decoder for the codec supports probing, s->out_codec is set to
 *   VIDEO_CODEC_END (otherwise a decompression is directly configured)
 * * decompression is done as usual (display is not reconfigured and output is
 *   put to a dummy framebuffer)
 * * when decoder is able to establish the codec, it returns
 *   DECODER_GOT_CODEC, thread send a reconf message to the main threads and
 *   the reconfiguration shall begin - display codecs are reordered ideally to
 *   match the incoming stream
 * * BUGS: usually multiple reconf messages are sent, decompress priority is
 *   evaluated only when there are multiple same (in_codec,internal_codec,out_codec)
 *   tuples.
 *
 * @todo
 * This code is very very messy, it needs to be rewritten.
 *
 * @note
 * @anchor vdec_note1
 * The properties of DXTn do not exactly match - bpp is 0.5, but line (actually 4
 * lines) is (2 * width) long, so it makes troubles when using line decoder
 * and tiles. So the fallback is external decoder. The DXT compression is exceptional
 * in that, that it can be both internally and externally decompressed.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "compat/platform_time.h"
#include "control_socket.h"
#include "crypto/openssl_decrypt.h"
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "messaging.h"
#include "module.h"
#include "pdb.h"
#include "participant_db.hpp"
#include "rtp/fec.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/playout_buffer.hpp"
#include "rtp/video_decoders.h"
#include "threadpool.hpp"
#include "utils/color_out.h"
#include "utils/macros.h"
#include "utils/misc.h"
#include "utils/synchronized_queue.h"
#include "utils/thread.h"
#include "utils/timed_message.h"
#include "utils/utility.hpp"
#include "utils/worker.h"
#include "video.h"
#include "video_decompress.h"
#include "video_display.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#ifdef RECONFIGURE_IN_FUTURE_THREAD
#include <future>
#endif
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

#ifdef HAVE_LIBAVCODEC_AVCODEC_H
#include <libavcodec/avcodec.h> // AV_INPUT_BUFFER_PADDING_SIZE
#endif

#define MOD_NAME "[video dec.] "

#define FRAMEBUFFER_NOT_READY(decoder) (decoder->frame == NULL && decoder->out_codec != VIDEO_CODEC_END)

#define VIDEO_DECODER_THREAD_COUNT 8

using namespace std;
using namespace std::string_literals;
using std::chrono::duration_cast;
using std::chrono::steady_clock;
using std::chrono::nanoseconds;

struct state_video_decoder;

/**
 * Interlacing changing function protoype. The function should be able to change buffer
 * in place, that is when dst and src are the same.
 */
typedef void (*change_il_t)(char *dst, char *src, int linesize, int height, void **state);

// prototypes
static bool reconfigure_decoder(struct state_video_decoder *decoder,
                struct video_desc desc, codec_t comp_int_fmt);
static int check_for_mode_change(struct state_video_decoder *decoder, uint32_t *hdr);
static void wait_for_framebuffer_swap(struct state_video_decoder *decoder);
static void *fec_thread(void *args);
static void *decompress_thread(void *args);
static void cleanup(struct state_video_decoder *decoder);
static void decoder_process_message(struct module *);

static int sum_map(map<int, int> const & m) {
        int ret = 0;
        for (map<int, int>::const_iterator it = m.begin();
                        it != m.end(); ++it) {
                ret += it->second;
        }
        return ret;
}

namespace {

#ifdef HAVE_LIBAVCODEC_AVCODEC_H
constexpr int PADDING = AV_INPUT_BUFFER_PADDING_SIZE;
#else
constexpr int PADDING = 0;
#endif
/**
 * Enumerates 2 possibilities how to decode arriving data.
 */
enum decoder_type_t {
        UNSET,
        LINE_DECODER,    ///< This is simple decoder, that decodes incoming data per line (pixelformats only).
        EXTERNAL_DECODER /**< real decompress, received buffer is opaque and has to be decompressed as
                          * a whole. */
};

/**
 * This structure holds data needed to use a linedecoder.
 */
struct line_decoder {
        int                  base_offset;  ///< from the beginning of buffer. Nonzero if decoding from mutiple tiles.
        long                 conv_num;     ///< in->out bpp conv numerator
        long                 conv_den;     ///< in->out bpp conv denominator
        int                  shifts[3];    ///< requested red,green and blue shift (in bits)
        decoder_t            decode_line;  ///< actual decoding function
        unsigned int         dst_linesize; ///< destination linesize
        unsigned int         dst_pitch;    ///< framebuffer pitch - it can be larger if SDL resolution is larger than data
        unsigned int         src_linesize; ///< source linesize
};

struct reported_statistics_cumul {
        ~reported_statistics_cumul() {
                print();
        }
        long long int last_buffer_number = -1; ///< last received buffer ID
        chrono::steady_clock::time_point t_last = chrono::steady_clock::now();
        unsigned long int displayed = 0, dropped = 0, corrupted = 0, missing = 0;
        atomic_ulong fec_ok = 0, fec_corrected = 0, fec_nok = 0;
        void print() {
                ostringstream fec;
                if (fec_ok + fec_nok + fec_corrected > 0) {
                        fec << " FEC noerr/OK/NOK: " << SBOLD(fec_ok) << "/" << SBOLD(fec_corrected) << "/" << SBOLD(fec_nok);
                }
                LOG(LOG_LEVEL_INFO) << SUNDERLINE("Video dec stats") << " (cumulative): "
                        << SBOLD(displayed + dropped + missing) << " total / "
                        << SBOLD(displayed) << " disp / "
                        << SBOLD(dropped) << " drop / "
                        << SBOLD(corrupted) << " corr / "
                        << SBOLD(missing) << " missing."
                        << fec.str() << "\n";
        }
        void update(int buffer_number) {
                if (last_buffer_number != -1) {
                        long long int diff = buffer_number -
                                ((last_buffer_number + 1) & ((1U<<BUFNUM_BITS) - 1));
                        diff = (diff + (1U<<BUFNUM_BITS)) % (1U<<BUFNUM_BITS);
                        if (diff < (1U<<BUFNUM_BITS) / 2) {
                                missing += diff;
                        } else { // frames may have been reordered, add arbitrary 1
                                missing += 1;
                        }
                }
                last_buffer_number = buffer_number;
                auto now = chrono::steady_clock::now();
                if (chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - t_last).count() > CUMULATIVE_REPORTS_INTERVAL) {
                        print();
                        t_last = now;
                }
        }
};

// message definitions
struct frame_msg {
        inline frame_msg(struct control_state *c, struct reported_statistics_cumul &sr) : control(c), recv_frame(nullptr),
                                nofec_frame(nullptr),
                             received_pkts_cum(0), expected_pkts_cum(0),
                             stats(sr)
        {}
        inline ~frame_msg() {
                if (recv_frame) {
                        int received_bytes = 0;
                        for (unsigned int i = 0; i < recv_frame->tile_count; ++i) {
                                received_bytes += sum_map(pckt_list[i]);
                        }
                        int expected_bytes = vf_get_data_len(recv_frame);
                        if (recv_frame->fec_params.type != FEC_NONE) {
                                if (is_corrupted) {
                                        stats.fec_nok += 1;
                                } else {
                                        if (received_bytes == expected_bytes) {
                                                stats.fec_ok += 1;
                                        } else {
                                                stats.fec_corrected += 1;
                                        }
                                }
                        }
                        stats.corrupted += is_corrupted;
                        stats.displayed += is_displayed;
                        stats.dropped += !is_displayed;
                }
                vf_free(recv_frame);
                vf_free(nofec_frame);
        }
        struct control_state *control;
        vector <uint32_t> buffer_num;
        struct video_frame *recv_frame; ///< received frame with FEC and/or compression
        struct video_frame *nofec_frame; ///< frame without FEC
        unique_ptr<map<int, int>[]> pckt_list;
        unsigned long long int received_pkts_cum, expected_pkts_cum;
        struct reported_statistics_cumul &stats;
        bool is_corrupted = false;
        bool is_displayed = false;
};

struct main_msg_reconfigure {
        inline main_msg_reconfigure(struct video_desc d,
                        unique_ptr<frame_msg> &&f,
                        bool force = false,
                        codec_t cim = VIDEO_CODEC_NONE) :
                desc(d),
                last_frame(std::move(f)),
                force(force),
                compress_internal_codec(cim) {}

        struct video_desc desc;
        unique_ptr<frame_msg> last_frame;
        bool force;
        codec_t compress_internal_codec;
};
}

/**
 * @brief Decoder state
 */
struct state_video_decoder
{
        state_video_decoder(struct module *parent) {
                module_init_default(&mod);
                mod.cls = MODULE_CLASS_DECODER;
                mod.priv_data = this;
                mod.new_message = decoder_process_message;
                module_register(&mod, parent);
                control = (struct control_state *) get_module(get_root_module(parent), "control");
        }
        ~state_video_decoder() {
                module_done(&mod);
        }
        struct module mod;
        struct control_state *control = {};

        thread decompress_thread_id, fec_thread_id, display_thread_id;
        struct video_desc received_vid_desc = {}; ///< description of the network video
        struct video_desc display_desc = {};      ///< description of the mode that display is currently configured to

        struct video_frame *frame = NULL; ///< @todo rewrite this more reasonably

        struct display   *display = NULL; ///< assigned display device
        /// @{
        vector<codec_t>   native_codecs; ///< list of display native codecs
        enum interlacing_t *disp_supported_il = NULL; ///< display supported interlacing mode
        size_t            disp_supported_il_cnt = 0; ///< count of @ref disp_supported_il
        /// @}

        unsigned int      max_substreams = 0; ///< maximal number of expected substreams
        change_il_t       change_il = NULL;      ///< function to change interlacing, if needed. Otherwise NULL.
        vector<void *>    change_il_state;

        mutex lock;

        enum decoder_type_t decoder_type = {};  ///< how will the video data be decoded
        struct line_decoder *line_decoder = NULL; ///< if the video is uncompressed and only pixelformat change
                                           ///< is neeeded, use this structure
        vector<struct state_decompress *> decompress_state; ///< state of the decompress (for every substream)
        bool accepts_corrupted_frame = false;     ///< whether we should pass corrupted frame to decompress
        bool buffer_swapped = true; /**< variable indicating that display buffer
                              * has been processed and we can write to a new one */
        condition_variable buffer_swapped_cv; ///< condition variable associated with @ref buffer_swapped

        synchronized_queue<unique_ptr<frame_msg>, 1> decompress_queue;
        synchronized_queue<unique_ptr<video_frame>, 1> display_queue;

        codec_t           out_codec = VIDEO_CODEC_NONE;
        int               pitch = 0;

        synchronized_queue<unique_ptr<frame_msg>, 1> fec_queue;

        enum video_mode   video_mode = {} ;  ///< video mode set for this decoder
        bool          merged_fb = false; ///< flag if the display device driver requires tiled video or not

        timed_message<LOG_LEVEL_WARNING> slow_msg; ///< shows warning ony in certain interval

        synchronized_queue<main_msg_reconfigure *, -1> msg_queue;

        const struct openssl_decrypt_info *dec_funcs = NULL; ///< decrypt state
        struct openssl_decrypt      *decrypt = NULL; ///< decrypt state
        std::vector<openssl_decrypt*> decryption_resources;
        unsigned int thread_count;

        std::atomic<bool> should_display;

#ifdef RECONFIGURE_IN_FUTURE_THREAD
        std::future<bool> reconfiguration_future;
        bool             reconfiguration_in_progress = false;
#endif
        struct reported_statistics_cumul stats = {}; ///< stats to be reported through control socket
};

/**
 * This function blocks until video frame is displayed and decoder::frame
 * can be filled with new data. Until this point, the video frame is not considered
 * valid.
 */
static void wait_for_framebuffer_swap(struct state_video_decoder *decoder) {
        unique_lock<mutex> lk(decoder->lock);
        decoder->buffer_swapped_cv.wait(lk, [decoder]{return decoder->buffer_swapped;});
}

#define ENCRYPTED_ERR "Receiving encrypted video data but " \
        "no decryption key entered!\n"
#define NOT_ENCRYPTED_ERR "Receiving unencrypted video data " \
        "while expecting encrypted.\n"

static void *fec_thread(void *args) {
        set_thread_name(__func__);
        struct state_video_decoder *decoder =
                (struct state_video_decoder *) args;

        fec *fec_state = NULL;
        struct fec_desc desc(FEC_NONE);

        while(1) {
                unique_ptr<frame_msg> data = decoder->fec_queue.pop();

                if (!data->recv_frame) { // poisoned
                        decoder->decompress_queue.push(std::move(data));
                        break; // exit from loop
                }

                struct video_frame *frame = decoder->frame;
                struct tile *tile = NULL;

                if (data->recv_frame->fec_params.type != FEC_NONE) {
                        if(!fec_state || desc.k != data->recv_frame->fec_params.k ||
                                        desc.m != data->recv_frame->fec_params.m ||
                                        desc.c != data->recv_frame->fec_params.c ||
                                        desc.seed != data->recv_frame->fec_params.seed
                          ) {
                                delete fec_state;
                                desc = data->recv_frame->fec_params;
                                fec_state = fec::create_from_desc(desc);
                                if(fec_state == NULL) {
                                        log_msg(LOG_LEVEL_FATAL, "[decoder] Unable to initialize FEC.\n");
                                        exit_uv(1);
                                        goto cleanup;
                                }
                        }
                }

                data->nofec_frame = vf_alloc(data->recv_frame->tile_count);
                data->nofec_frame->ssrc = data->recv_frame->ssrc;

                if (data->recv_frame->fec_params.type != FEC_NONE) {
                        bool buffer_swapped = false;
                        for (int pos = 0; pos < get_video_mode_tiles_x(decoder->video_mode)
                                        * get_video_mode_tiles_y(decoder->video_mode); ++pos) {
                                char *fec_out_buffer = NULL;
                                int fec_out_len = 0;

                                if (data->recv_frame->tiles[pos].data_len != (unsigned int) sum_map(data->pckt_list[pos])) {
                                        debug_msg("Frame incomplete - substream %d, buffer %d: expected %u bytes, got %u.\n", pos,
                                                        (unsigned int) data->buffer_num[pos],
                                                        data->recv_frame->tiles[pos].data_len,
                                                        (unsigned int) sum_map(data->pckt_list[pos]));
                                }

                                bool ret = fec_state->decode(data->recv_frame->tiles[pos].data,
                                                data->recv_frame->tiles[pos].data_len,
                                                &fec_out_buffer, &fec_out_len, data->pckt_list[pos]);

                                if (ret == false) {
                                        data->is_corrupted = true;
                                        verbose_msg("[decoder] FEC: unable to reconstruct data.\n");
                                        if (fec_out_len < (int) sizeof(video_payload_hdr_t)) {
                                                goto cleanup;
                                        }
                                        if (decoder->decoder_type == EXTERNAL_DECODER && !decoder->accepts_corrupted_frame) {
                                                goto cleanup;
                                        }
                                }

                                video_payload_hdr_t video_hdr;
                                memcpy(&video_hdr, fec_out_buffer,
                                                sizeof(video_payload_hdr_t));
                                fec_out_buffer += sizeof(video_payload_hdr_t);
                                fec_out_len -= sizeof(video_payload_hdr_t);

                                struct video_desc network_desc;
                                parse_video_hdr(video_hdr, &network_desc);
                                if (!video_desc_eq_excl_param(decoder->received_vid_desc,
                                                        network_desc, PARAM_TILE_COUNT)) {
                                        decoder->msg_queue.push(new main_msg_reconfigure(network_desc, std::move(data)));
                                        goto cleanup;
                                }

                                if (FRAMEBUFFER_NOT_READY(decoder) || (frame != nullptr && frame->color_spec == VIDEO_CODEC_NONE)) {
                                        goto cleanup;
                                }

                                if(decoder->decoder_type == EXTERNAL_DECODER) {
                                        data->nofec_frame->tiles[pos].data_len = fec_out_len;
                                        data->nofec_frame->tiles[pos].data = fec_out_buffer;
                                } else { // linedecoder
                                        if (!buffer_swapped) {
                                                buffer_swapped = true;
                                                wait_for_framebuffer_swap(decoder);
                                                unique_lock<mutex> lk(decoder->lock);
                                                decoder->buffer_swapped = false;
                                        }

                                        int divisor;

                                        if (!decoder->merged_fb) {
                                                divisor = decoder->max_substreams;
                                        } else {
                                                divisor = 1;
                                        }

                                        tile = vf_get_tile(frame, pos % divisor);

                                        struct line_decoder *line_decoder =
                                                &decoder->line_decoder[pos];

                                        int data_pos = 0;
                                        char *src = fec_out_buffer;
                                        char *dst = tile->data + line_decoder->base_offset;
                                        while(data_pos < (int) fec_out_len) {
                                                line_decoder->decode_line((unsigned char*)dst, (unsigned char *) src, line_decoder->dst_linesize,
                                                                line_decoder->shifts[0],
                                                                line_decoder->shifts[1],
                                                                line_decoder->shifts[2]);
                                                src += line_decoder->src_linesize;
                                                dst += vc_get_linesize(tile->width ,frame->color_spec);
                                                data_pos += line_decoder->src_linesize;
                                        }
                                }
                        }
                } else { /* PT_VIDEO */
                        for(int i = 0; i < (int) decoder->max_substreams; ++i) {
                                data->nofec_frame->tiles[i].data_len = data->recv_frame->tiles[i].data_len;
                                data->nofec_frame->tiles[i].data = data->recv_frame->tiles[i].data;

                                if (data->recv_frame->tiles[i].data_len != (unsigned int) sum_map(data->pckt_list[i])) {
                                        debug_msg("Frame incomplete - substream %d, buffer %d: expected %u bytes, got %u.%s\n", i,
                                                        (unsigned int) data->buffer_num[i],
                                                        data->recv_frame->tiles[i].data_len,
                                                        (unsigned int) sum_map(data->pckt_list[i]),
                                                        decoder->decoder_type == EXTERNAL_DECODER && !decoder->accepts_corrupted_frame ? " dropped.\n" : "");
                                        data->is_corrupted = true;
                                        if(decoder->decoder_type == EXTERNAL_DECODER && !decoder->accepts_corrupted_frame) {
                                                goto cleanup;
                                        }
                                }
                        }
                }

                decoder->decompress_queue.push(std::move(data));
cleanup:
                ;
        }

        delete fec_state;

        return NULL;
}

static bool blacklist_current_out_codec(struct state_video_decoder *decoder){
        if(decoder->out_codec == VIDEO_CODEC_NONE)
                return false;

        auto to_erase = find(decoder->native_codecs.begin(), decoder->native_codecs.end(), decoder->out_codec);
        if (to_erase == decoder->native_codecs.end()) {
                return true; // should it really return true?
        }
        log_msg(LOG_LEVEL_DEBUG, "Blacklisting codec %s\n", get_codec_name(decoder->out_codec));
        decoder->native_codecs.erase(to_erase);

        return true;
}

struct decompress_data {
        struct state_video_decoder *decoder;
        int pos;
        struct video_frame *compressed;
        int buffer_num;
        decompress_status ret = DECODER_NO_FRAME;
        unsigned char *out;
        codec_t internal_codec; // set only if probing (ret == DECODER_GOT_CODEC)
};

static void *decompress_worker(void *data)
{
        auto d = (struct decompress_data *) data;
        struct state_video_decoder *decoder = d->decoder;

        if (!d->compressed->tiles[d->pos].data)
                return NULL;

        d->ret = decompress_frame(decoder->decompress_state.at(d->pos),
                        (unsigned char *) d->out,
                        (unsigned char *) d->compressed->tiles[d->pos].data,
                        d->compressed->tiles[d->pos].data_len,
                        d->buffer_num,
                        &decoder->frame->callbacks,
                        &d->internal_codec);
        return d;
}

/**
 * Push the decompress data in the decompression module. The decompression module
 * will handle the the output via the "pop" API in the display thread.
*/
static void *decompress_worker_async_push(void *data)
{
        auto d = (struct decompress_data *) data;
        struct state_video_decoder *decoder = d->decoder;

        if (!d->compressed->tiles[d->pos].data)
                return NULL;

        d->ret = decompress_frame_async_push(decoder->decompress_state.at(d->pos),
                                             (unsigned char *) d->compressed->tiles[d->pos].data,
                                             d->compressed->tiles[d->pos].data_len,
                                             &d->internal_codec);
        return d;
}

/**
 * A helper function for determining if a decompression module is asynchronous or not
*/
static bool decompress_is_async(state_decompress* state) {
    bool is_async = false;

    if(state) {
        decompress_get_property(state, DECOMPRESS_PROPERTY_IS_ASYNC, &is_async, nullptr);
    }
    
    return is_async;
}

/**
 * A function for identifying if ALL decompression states held by the decoder are async or sync.
 * The return value will identify whether they're all async or sync. The program will terminate
 * if a mismatch is found since there is no active code for mixing and matching.
*/
static bool decoder_is_async(state_video_decoder* decoder) {
    bool is_async = false;
    for(auto state : decoder->decompress_state) {
        is_async = decompress_is_async(state);
    }

    // If a mix of async and sync code is in play, then we should terminate.
    if(!std::all_of(decoder->decompress_state.begin(),
                    decoder->decompress_state.end(),
                    [is_async](state_decompress* state){return decompress_is_async(state) == is_async;})) {
        LOG(LOG_LEVEL_ERROR) << "A mix of synchronous and asynchronous decompression is in use\n";
        terminate();
    }
    return is_async;
}

/**
 * Prepare the decompress data vector with the correct information per tile. This is done so that all
 * the fields required by the synchronous decompression API is correctly filled in.
*/
static void prepare_decompress_data_sync(state_video_decoder* decoder, frame_msg* msg,
                                         video_frame* display_frame, char* tmp,
                                         vector<decompress_data>* data, 
                                         int tile_count,
                                         int tile_height,
                                         int tile_width) {
    // Initialise the tiles (only if the tmp pointer is nullptr - Otherwise the display frame is not used)
    if(!tmp) {
        for(int pos = 0; pos < tile_count; ++pos) {
            display_frame->tiles[pos].data = static_cast<char*>(malloc(decoder->frame->tiles[pos].data_len));
            display_frame->tiles[pos].data_len = decoder->frame->tiles[pos].data_len;
        }
    }
    
    // For each tile set the correct input data, and output data pointers.
    for (int pos = 0; pos < tile_count; ++pos) {
            (*data)[pos].decoder = decoder;
            (*data)[pos].pos = pos;
            (*data)[pos].compressed = msg->nofec_frame;
            (*data)[pos].buffer_num = msg->buffer_num[pos];
            // Use the tmp pointer if it's been set. This is used when the output codec has not been
            // set yet. Thus the output data is temporary and will be discarded.
            if (tmp) {
                    (*data)[pos].out = (unsigned char *) tmp;
            }
            else if (decoder->merged_fb) {
                    // TODO: OK when rendering directly to display FB, otherwise, do not reflect pitch (we use PP)
                    int x = pos % get_video_mode_tiles_x(decoder->video_mode),
                        y = pos / get_video_mode_tiles_x(decoder->video_mode);
                    (*data)[pos].out = (unsigned char *) vf_get_tile(display_frame, 0)->data + y * decoder->pitch * tile_height +
                            vc_get_linesize(tile_width, decoder->out_codec) * x;
            } 
            else {
                    (*data)[pos].out = (unsigned char *) vf_get_tile(display_frame, pos)->data;
            }
    }
}

/**
 * Prepare the decompress data vector with the correct information per tile. This is done so that all
 * the fields required by the asynchronous decompression API is correctly filled in.
*/
static void prepare_decompress_data_async(state_video_decoder* decoder,
                                          frame_msg* msg,
                                          vector<decompress_data>* data,
                                          int tile_count) {
    
    for (int pos = 0; pos < tile_count; ++pos) {
            (*data)[pos].decoder = decoder;
            (*data)[pos].pos = pos;
            (*data)[pos].compressed = msg->nofec_frame;
            (*data)[pos].buffer_num = msg->buffer_num[pos];
    }
}

ADD_TO_PARAM("decoder-drop-policy",
                "* decoder-drop-policy=blocking|nonblock|<sec>\n"
                "  Force specified blocking policy (default nonblock).\n"
                "  <sec> - specifies frame timeout in seconds (can have suffixes, eg. \"20ms\")\n");
static void *decompress_thread(void *args) {
        set_thread_name(__func__);
        struct state_video_decoder *decoder =
                (struct state_video_decoder *) args;
        int tile_width = decoder->received_vid_desc.width; // get_video_mode_tiles_x(decoder->video_mode);
        int tile_height = decoder->received_vid_desc.height; // get_video_mode_tiles_y(decoder->video_mode);

        LOG(LOG_LEVEL_INFO) << "Tile Width: " << tile_width << " Tile Height: " << tile_height << "\n";

        long long force_putf_timeout = []() {
                auto drop_policy = commandline_params.find("decoder-drop-policy"s);
                if (drop_policy == commandline_params.end()) {
                        return -1LL;
                }
                if (drop_policy->second == "nonblock") {
                        return PUTF_NONBLOCK;
                }
                if (drop_policy->second == "blocking") {
                        return PUTF_BLOCKING;
                }
                return static_cast<long long>(unit_evaluate_dbl(drop_policy->second.c_str(), true) * NS_IN_SEC);
        }();

        while(1) {
                // Check whether the decoder contains async decompression modules, or synchronous decompression modules.
                bool is_async = decoder_is_async(decoder);

                unique_ptr<frame_msg> msg = decoder->decompress_queue.pop();
                unique_ptr<video_frame> display_frame = nullptr;

                if(!msg->recv_frame) { // poisoned
                    break;
                }

                auto t0 = std::chrono::steady_clock::now();
                unique_ptr<char[]> tmp;

                if (decoder->out_codec == VIDEO_CODEC_END) {
                        tmp = unique_ptr<char[]>(new char[tile_height * (tile_width * MAX_BPS + MAX_PADDING)]);
                }

                if(decoder->decoder_type == EXTERNAL_DECODER) {
                        int tile_count = get_video_mode_tiles_x(decoder->video_mode) *
                                        get_video_mode_tiles_y(decoder->video_mode);
                        vector<task_result_handle_t> handle(tile_count);
                        vector<decompress_data> data(tile_count);

                        if(!is_async) {
                            // If we're running synchronously then allocate a video frame for the decompression
                            // module to write into.
                            display_frame = unique_ptr<video_frame>(vf_alloc(tile_count));
                            display_frame->callbacks.data_deleter = vf_data_deleter;

                            prepare_decompress_data_sync(decoder, msg.get(), display_frame.get(),
                                                        tmp.get(), &data, tile_count, tile_height,
                                                        tile_width);

                            for (int pos = 0; pos < tile_count; ++pos) {
                                    if (tile_count > 1) {
                                            handle[pos] = task_run_async(decompress_worker, &data[pos]);
                                    } else {
                                            decompress_worker(&data[pos]);
                                    }
                            }
                        }
                        else {
                            // If we're running asynchronously then there is no need to allocate a buffer
                            // for the module to write into.
                            prepare_decompress_data_async(decoder, msg.get(), &data, tile_count);
                            for (int pos = 0; pos < tile_count; ++pos) {
                                    if (tile_count > 1) {
                                            handle[pos] = task_run_async(decompress_worker_async_push, &data[pos]);
                                    } else {
                                            decompress_worker_async_push(&data[pos]);
                                    }
                            }
                        }

                        // If there is more than one tile, then wait for the threaded tasks to be completed.
                        if (tile_count > 1) {
                                for (int pos = 0; pos < tile_count; ++pos) {
                                        wait_task(handle[pos]);
                                }
                        }

                        // Process the results of the decompression
                        for (int pos = 0; pos < tile_count; ++pos) {
                                // Check if the decompression module signalled that a reconfiguration is necessary.
                                if (data[pos].ret == DECODER_GOT_CODEC) {
                                        LOG(LOG_LEVEL_NOTICE) << MOD_NAME << "Detected internal codec: " << get_codec_name(data[pos].internal_codec) << "\n";
                                        decoder->msg_queue.push(new main_msg_reconfigure(decoder->received_vid_desc, nullptr, true, data[pos].internal_codec));
                                        goto skip_frame;
                                }

                                // If there was no frame sent back from decompression module check what to do
                                if (data[pos].ret != DECODER_GOT_FRAME){
                                        // If the decompression module reports it cannot decode, then blacklist the current codec
                                        // and attempt a reconfiguration.
                                        if (data[pos].ret == DECODER_CANT_DECODE){
                                                if(blacklist_current_out_codec(decoder))
                                                        decoder->msg_queue.push(new main_msg_reconfigure(decoder->received_vid_desc, nullptr, true));
                                        }

                                        // If the decompression module is asynchronous then mark the msg as being displayed
                                        // as we don't want to mark the frame buffer as being swapped.
                                        if(data[pos].ret == DECODER_FRAME_PUSHED) {
                                            msg->is_displayed = true;
                                        }

                                        // We want to skip the frame if no frame was given back.
                                        goto skip_frame;
                                }
                        }
                } else {
                        if (decoder->frame->decoder_overrides_data_len == TRUE) {
                                for (unsigned int i = 0; i < decoder->frame->tile_count; ++i) {
                                        decoder->frame->tiles[i].data_len = msg->nofec_frame->tiles[i].data_len;
                                }
                        }
                }

                LOG(LOG_LEVEL_DEBUG) << MOD_NAME << "Decompress duration: " <<
                        duration_cast<nanoseconds>(steady_clock::now() - t0).count() / 1000000.0 << " ms\n";

                if(decoder->change_il) {
                        for(unsigned int i = 0; i < decoder->frame->tile_count; ++i) {
                                struct tile *tile = vf_get_tile(decoder->frame, i);
                                decoder->change_il(tile->data, tile->data, vc_get_linesize(tile->width,
                                                        decoder->out_codec), tile->height, &decoder->change_il_state[i]);
                        }
                }

                // Push the frame into the display queue (this should only be run in a synchronous decompression module)
                decoder->frame->ssrc = msg->nofec_frame->ssrc;
                decoder->display_queue.push(std::move(display_frame));
                msg->is_displayed = true;

skip_frame:
                if(!msg->is_displayed) {
                        unique_lock<mutex> lk(decoder->lock);
                        // we have put the video frame and requested another one which is
                        // writable so on
                        decoder->buffer_swapped = true;
                        lk.unlock();
                        decoder->buffer_swapped_cv.notify_one();
                }

                // If this display frame hasn't been moved, then release it and free it via the destroy API.
                if(display_frame) {
                    video_frame* released_frame = display_frame.release();
                    vf_free(released_frame);
                }
        }

        return NULL;
}

/**
 * A helper function for getting the decoder drop policy from the
 * command line.
 */
static long long get_force_putf_timeout() {
    auto drop_policy = commandline_params.find("decoder-drop-policy"s);
    if (drop_policy == commandline_params.end()) {
        return -1LL;
    }
    if (drop_policy->second == "nonblock") {
        return PUTF_NONBLOCK;
    }
    if (drop_policy->second == "blocking") {
        return PUTF_BLOCKING;
    }
    return static_cast<long long>(unit_evaluate_dbl(drop_policy->second.c_str(), true) * NS_IN_SEC);
}

/**
 * Collect the display frame from the asynchronous decompression API.
 */
static bool async_collect_frame(state_video_decoder* decoder) {
    // Get the tile count from the existing frame
    int tile_count = 0;
    if(decoder->frame) {
        tile_count = static_cast<int>(decoder->frame->tile_count);
    }

    // Loop until we the decompression module has written into every tile (or the display thread is shutdown)
    std::vector<decompress_status> decompress_statuses = std::vector<decompress_status>(tile_count, DECODER_NO_FRAME);
    while(!std::all_of(decompress_statuses.begin(), decompress_statuses.end(), [](decompress_status status){return status == DECODER_GOT_FRAME;}) && decoder->should_display.load()) {
        for(int i = 0; i < tile_count; i++) {
            // Request a decompression is the decompress state is set, and the decoder frame is populated.
            if(decompress_statuses[i] != DECODER_GOT_FRAME && !decoder->decompress_state.empty() && decoder->decompress_state.at(i) && decoder->frame) {
                decompress_frame_async_pop(decoder->decompress_state.at(i), &(decompress_statuses[i]), decoder->frame, i);
            }
        }
    }

    // If the display loop is still true then the only way to get to
    // this code is if we collected a frame.
    return decoder->should_display.load();
}

/**
 * Collect the display frame from the synchronous decompression API.
 */
static bool sync_collect_frame(state_video_decoder* decoder) {
    // Block until we can grab the latest frame to display
    std::unique_ptr<video_frame> display_frame = decoder->display_queue.pop();

    // If we received a nullptr, then we should treat this as a signal
    // to break out of the loop.
    if(!display_frame) {
        return false;
    }

    // Copy the display frame into the decoder frame
    for(int i = 0; i < display_frame->tile_count; i++) {
        char* source = display_frame->tiles[i].data;
        char* dest = decoder->frame->tiles[i].data;
        size_t length = std::min<size_t>(display_frame->tiles[i].data_len, decoder->frame->tiles[i].data_len);
        memcpy(dest, source, length);
        // Set all the tile lengths to match the copy
        decoder->frame->tiles[i].data_len = length;
    }

    // In order to properly free the frame we need to release it from the unique_ptr
    video_frame* frame = display_frame.release();
    vf_free(frame);

    return true;
}

/**
 * A helper function to notify other threads that the video frame in the
 * decoder has been swapped.
 */
static void notify_buffer_swapped(state_video_decoder* decoder) {
    unique_lock<mutex> lk(decoder->lock);
    // we have put the video frame and requested another one which is
    // writable so on
    decoder->buffer_swapped = true;
    lk.unlock();
    decoder->buffer_swapped_cv.notify_one();
}

/**
 * The display thread. This thread is responsible for collecting the frames from the decompression
 * API and displaying them. It can either receive from the asynchronous API provided by a decompression
 * module, or the synchronous API provided from the decompression thread.
 */
static void display_thread(void* args) {
    set_thread_name(__func__);

    auto decoder = static_cast<state_video_decoder *>(args);

    // Get the decoder drop policy
    long long force_putf_timeout = get_force_putf_timeout();
    long long putf_timeout = force_putf_timeout != -1 ? force_putf_timeout : PUTF_NONBLOCK;


    while(decoder->should_display) {
        bool display_shutdown = false;

        // Find out if the collection needs to be completed asynchronously or not
        bool is_async = decoder_is_async(decoder);

        // Collect the frame from either the async collection or the sync collection
        if(is_async) {
            display_shutdown = !async_collect_frame(decoder);
        }
        else {
            display_shutdown = !sync_collect_frame(decoder);
        }

        // The collection of the frame has indicated that we should exit.
        if(display_shutdown) {
            break;
        }

        // Display the frame
        int ret = display_put_frame(decoder->display, decoder->frame, putf_timeout);

        // Refresh the decoder frame
        decoder->frame = display_get_frame(decoder->display);
        notify_buffer_swapped(decoder);
    }

    while(decoder->display_queue.size() > 0) {
        // Block until we can grab the latest frame to display
        std::unique_ptr<video_frame> display_frame = decoder->display_queue.pop();

        // In order to properly free the frame we need to release it from the unique_ptr
        video_frame* frame = display_frame.release();
        vf_free(frame);
    }
}

static void decoder_set_video_mode(struct state_video_decoder *decoder, enum video_mode video_mode)
{
        decoder->video_mode = video_mode;
        decoder->max_substreams = get_video_mode_tiles_x(decoder->video_mode)
                        * get_video_mode_tiles_y(decoder->video_mode);
}

ADD_TO_PARAM("decode-thread-count", "* decode-thread-count=<Thread Count>\n"
             "  The number of threads to use when decrypting packets.\n");

/**
 * @brief Initializes video decompress state.
 * @param video_mode  video_mode expected to be received from network
 * @param display     Video display that is supposed to be controlled from decoder.
 *                    display_get_frame(), display_put_frame(), display_get_propert()
 *                    and display_reconfigure() functions may be used. If set to NULL,
 *                    no decoding will take place.
 * @param encryption  Encryption config string. Currently, this is a passphrase to be
 *                    used. This may change eventually.
 * @return Newly created decoder state. If an error occured, returns NULL.
 */
struct state_video_decoder *video_decoder_init(struct module *parent,
                enum video_mode video_mode,
                struct display *display, const char *encryption)
{
        struct state_video_decoder *s;

        s = new state_video_decoder(parent);

        if (encryption) {
                s->dec_funcs = static_cast<const struct openssl_decrypt_info *>(load_library("openssl_decrypt",
                                        LIBRARY_CLASS_UNDEFINED, OPENSSL_DECRYPT_ABI_VERSION));
                if (!s->dec_funcs) {
                                log_msg(LOG_LEVEL_FATAL, "UltraGrid was build without OpenSSL support!\n");
                                delete s;
                                return NULL;
                }

                if (s->dec_funcs->init(&s->decrypt, encryption) != 0) {
                        log_msg(LOG_LEVEL_FATAL, "Unable to create decompress!\n");
                        delete s;
                        return NULL;
                }
        }

    // By default let the display thread run
    s->should_display.store(true);
    // Have the thread count be configurable via a parameter. Otherwise, default to the
    // full amount of available threads.
    s->thread_count = VIDEO_DECODER_THREAD_COUNT;
    const char* decodeThreadCount = get_commandline_param("decode-thread-count");
    if(decodeThreadCount) {
        int thread_count = std::stoi(decodeThreadCount);
        if(thread_count > 0) {
            s->thread_count = static_cast<unsigned int>(thread_count);
            LOG(LOG_LEVEL_INFO) << "The decoder thread count has been set to: " << s->thread_count << "\n";
        }
    }

    // Create a decryption structure per available thread if decryption is set.
    for(unsigned int i = 0; i < s->thread_count; i++) {
        // Hand in a pointer from the stack, this can then be copied into a vector
        openssl_decrypt* decryption_resource = nullptr;
        if (encryption && s->dec_funcs->init(&decryption_resource, encryption) != 0) {
            log_msg(LOG_LEVEL_FATAL, MOD_NAME "Unable to initialize decryption\n");
            delete s;
            return NULL;
        }
        // Copy the pointer into the vector
        s->decryption_resources.push_back(decryption_resource);
    }

    // Print out a log to let the user know stream decryption has been enabled
    if(encryption) {
        log_msg(LOG_LEVEL_INFO, MOD_NAME "Enabled stream decryption.\n");
    }

    decoder_set_video_mode(s, video_mode);

    if(!video_decoder_register_display(s, display)) {
            delete s;
            return NULL;
    }

    return s;
}

/**
 * @brief starts decompress and ldmg threads
 *
 * Called from video_decoder_register_display(). It is also called after
 * video_decoder_stop_threads() in reconfiguration.
 *
 * @invariant
 * decoder->display != NULL
 */
static void video_decoder_start_threads(struct state_video_decoder *decoder)
{
        assert(decoder->display); // we want to run threads only if decoder is active
        decoder->should_display.store(true);

        decoder->decompress_thread_id = thread(decompress_thread, decoder);
        decoder->fec_thread_id = thread(fec_thread, decoder);
        decoder->display_thread_id = thread(display_thread, decoder);
}

/**
 * @brief This function stops running threads.
 *
 * @invariant
 * decoder->display != NULL
 */
static void video_decoder_stop_threads(struct state_video_decoder *decoder)
{
        assert(decoder->display);

        unique_ptr<frame_msg> msg(new frame_msg(decoder->control, decoder->stats));
        decoder->fec_queue.push(std::move(msg));

        // Poison the display queue
        decoder->display_queue.push(nullptr);
        decoder->should_display.store(false);

        decoder->fec_thread_id.join();
        decoder->decompress_thread_id.join();
        decoder->display_thread_id.join();
}

static auto codec_list_to_str(vector<codec_t> const &codecs) {
        if (codecs.empty()) {
                return "(none)"s;
        }
        ostringstream oss;
        auto it = codecs.begin();
        oss << *it++;
        for ( ; it != codecs.end(); ++it) {
                oss << (it + 1 == codecs.end() ? " and " : ", ") << get_codec_name(*it);
        }
        return oss.str();
}

ADD_TO_PARAM("decoder-use-codec",
                "* decoder-use-codec=<codec>\n"
                "  Use specified pixel format for decoding (eg. v210). This overrides automatic\n"
                "  choice. The pixel format must be supported by the video display. Use 'help' to see\n"
                "  available options for a display (eg.: 'uv -d gl --param decoder-use-codec=help').\n");
/**
 * @brief Registers video display to be used for displaying decoded video frames.
 *
 * No display should be managed by this decoder when this function is called.
 *
 * @see decoder_remove_display
 *
 * @param decoder decoder state
 * @param display new display to be registered
 * @return        whether registration was successful
 */
bool video_decoder_register_display(struct state_video_decoder *decoder, struct display *display)
{
        assert(display != NULL);
        assert(decoder->display == NULL);

        int ret;

        decoder->display = display;

        codec_t native_codecs[VIDEO_CODEC_COUNT];
        size_t len = sizeof native_codecs;
        ret = display_ctl_property(decoder->display, DISPLAY_PROPERTY_CODECS, native_codecs, &len);
        decoder->native_codecs.clear();
        if (!ret) {
                log_msg(LOG_LEVEL_ERROR, "Failed to query codecs from video display.\n");
        } else {
                for (size_t i = 0; i < len / sizeof(codec_t); ++i) {
                        decoder->native_codecs.push_back(native_codecs[i]);
                }
        }
        if (get_commandline_param("decoder-use-codec")) {
                const char *codec_str = get_commandline_param("decoder-use-codec");
                codec_t req_codec = get_codec_from_name(codec_str);
                if ("help"s == codec_str) {
                        LOG(LOG_LEVEL_NOTICE) << MOD_NAME << "Supported codecs for current display are: " << codec_list_to_str(decoder->native_codecs) << "\n";
                        return false;
                }
                if (req_codec == VIDEO_CODEC_NONE) {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "Wrong decoder codec spec: %s.\n", codec_str);
                        LOG(LOG_LEVEL_INFO) << MOD_NAME << "Supported codecs for current display are: " << codec_list_to_str(decoder->native_codecs) << "\n";
                        return false;
                }
                if (find(decoder->native_codecs.begin(), decoder->native_codecs.end(), req_codec) != end(decoder->native_codecs)) {
                        decoder->native_codecs.clear();
                        decoder->native_codecs.push_back(req_codec);
                } else {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "Display doesn't support requested codec: %s.\n", codec_str);
                        LOG(LOG_LEVEL_INFO) << MOD_NAME << "Supported codecs for current display are: " << codec_list_to_str(decoder->native_codecs) << "\n";
                        return false;
                }
        }

        free(decoder->disp_supported_il);
        decoder->disp_supported_il_cnt = 20 * sizeof(enum interlacing_t);
        decoder->disp_supported_il = (enum interlacing_t*) calloc(decoder->disp_supported_il_cnt,
                        sizeof(enum interlacing_t));
        ret = display_ctl_property(decoder->display, DISPLAY_PROPERTY_SUPPORTED_IL_MODES, decoder->disp_supported_il, &decoder->disp_supported_il_cnt);
        if(ret) {
                decoder->disp_supported_il_cnt /= sizeof(enum interlacing_t);
        } else {
                enum interlacing_t tmp[] = { PROGRESSIVE, INTERLACED_MERGED, SEGMENTED_FRAME}; /* default if not said othervise */
                memcpy(decoder->disp_supported_il, tmp, sizeof(tmp));
                decoder->disp_supported_il_cnt = sizeof(tmp) / sizeof(enum interlacing_t);
        }

        video_decoder_start_threads(decoder);

        return true;
}

/**
 * @brief This removes display from current decoder.
 *
 * From now on, no video frames will be decoded with current decoder.
 * @see decoder_register_display - the counterpart of this functon
 *
 * @param decoder decoder from which will the decoder be removed
 */
void video_decoder_remove_display(struct state_video_decoder *decoder)
{
        if (decoder->display) {
                video_decoder_stop_threads(decoder);
                if (decoder->frame) {
                        display_put_frame(decoder->display, decoder->frame, PUTF_DISCARD);
                        decoder->frame = NULL;
                }
                decoder->display = NULL;
                memset(&decoder->display_desc, 0, sizeof(decoder->display_desc));
        }
}

static void cleanup(struct state_video_decoder *decoder)
{
        decoder->decoder_type = UNSET;
        for (auto &d : decoder->decompress_state) {
                decompress_done(d);
        }
        decoder->decompress_state.clear();
        if(decoder->line_decoder) {
                free(decoder->line_decoder);
                decoder->line_decoder = NULL;
        }

        for (auto && item : decoder->change_il_state) {
                free(item);
        }
        decoder->change_il_state.resize(0);
}

/**
 * @brief Destroys decoder created with decoder_init()
 * @param decoder decoder to be destroyed. If NULL, no action is performed.
 */
void video_decoder_destroy(struct state_video_decoder *decoder)
{
        if(!decoder)
                return;

        if (decoder->dec_funcs) {
                decoder->dec_funcs->destroy(decoder->decrypt);
        }

        if(!decoder->decryption_resources.empty()) {
            for(auto decryption_resource : decoder->decryption_resources) {
                if(decryption_resource) {
                    decoder->dec_funcs->destroy(decryption_resource);
                }
            }
        }

        video_decoder_remove_display(decoder);

        cleanup(decoder);

        free(decoder->disp_supported_il);

        delete decoder;
}

/**
 * Reorders display codecs to match compression internal format.
 *
 * First try to add HW-accelerated codecs, then exactly comp_int_fmt (if
 * available) and then sort the rest - matching color-space first, higher bit
 * depths first.
 *
 * Always first try "native" than generic decoder (native is the one matching
 * comp_int_fmt, generic should be catch-all allowing decompression of
 * arbitrary compressed stream of received codec).
 */
static vector<pair<codec_t, codec_t>> video_decoder_order_output_codecs(codec_t comp_int_fmt, vector<codec_t> const &display_codecs)
{
        vector<pair<codec_t, codec_t>> ret;
        set<codec_t> used;
        // first add hw-accelerated codecs
        for (auto codec : display_codecs) {
                if (codec_is_hw_accelerated(codec)) {
                        ret.push_back({comp_int_fmt, codec});
                        if (comp_int_fmt != VIDEO_CODEC_NONE) {
                                ret.push_back({VIDEO_CODEC_NONE, codec});
                        }
                        used.insert(codec);
                }
        }
        // then codecs matching exactly internal codec
        for (auto codec : display_codecs) {
                if (used.find(codec) != used.end()) {
                        continue;
                };
                if (codec == comp_int_fmt) {
                        ret.push_back({comp_int_fmt, codec});
                        if (comp_int_fmt != VIDEO_CODEC_NONE) {
                                ret.push_back({VIDEO_CODEC_NONE, codec});
                        }
                        used.insert(codec);
                }
        }
        // then add also all other codecs
        vector<codec_t> remaining;
        for (auto codec : display_codecs) {
                if (used.find(codec) != used.end()) {
                        continue;
                };
                remaining.push_back(codec);
        }
        if (comp_int_fmt != VIDEO_CODEC_NONE) {
                // sort - first codec of the same color space as internal (YUV
                // is implicitly !RGB), then the other
                // inside those 2 groups, sort nearest higher first, then downwardly the lower bit depts
                sort(remaining.begin(), remaining.end(), [comp_int_fmt](const codec_t &a, const codec_t &b) {
                                if (codec_is_a_rgb(a) != codec_is_a_rgb(b)) { // RGB and YUV (or vice versa)
                                        return codec_is_a_rgb(a) == codec_is_a_rgb(comp_int_fmt);
                                }
                                // either a or b is lower than comp_int_fmt bit depth - sort higher bit depth first
                                if (get_bits_per_component(a) < get_bits_per_component(comp_int_fmt) ||
                                                get_bits_per_component(b) < get_bits_per_component(comp_int_fmt)) {
                                        return get_bits_per_component(a) > get_bits_per_component(b);
                                }
                                // both are equal or higher - sort lower bit depth first
                                return get_bits_per_component(a) < get_bits_per_component(b);

                        });
        }
        for (auto & c : remaining) {
                ret.push_back({comp_int_fmt, c});
                if (comp_int_fmt != VIDEO_CODEC_NONE) {
                        ret.push_back({VIDEO_CODEC_NONE, c});
                }
        }

        if (log_level >= LOG_LEVEL_VERBOSE) {
                LOG(LOG_LEVEL_VERBOSE) << "Trying codecs in this order:\n";
                for (auto it = ret.begin(); it != ret.end(); ++it) {
                        LOG(LOG_LEVEL_VERBOSE) << "\t" << get_codec_name((*it).second) << ", internal: " << get_codec_name((*it).first) << "\n";
                }
        }

        return ret;
}

/**
 * This function selects, according to given video description, appropriate
 *
 * @param[in]  decoder     decoder to be taken parameters from (video mode, native codecs etc.)
 * @param[in]  desc        incoming video description
 * @param[out] decode_line If chosen decoder is a linedecoder, this variable contains the
 *                         decoding function.
 * @return                 Output codec, if no decoding function found, -1 is returned.
 */
static codec_t choose_codec_and_decoder(struct state_video_decoder *decoder, struct video_desc desc,
                                decoder_t *decode_line, codec_t comp_int_fmt)
{
        codec_t out_codec = VIDEO_CODEC_NONE;

        /* first check if the codec is natively supported */
        for (auto &codec : decoder->native_codecs) {
                if (desc.color_spec == codec) {
                        if ((desc.color_spec == DXT1 || desc.color_spec == DXT1_YUV ||
                                        desc.color_spec == DXT5)
                                        && decoder->video_mode != VIDEO_NORMAL)
                                continue; /// DXT1 it is a exception, see @ref vdec_note1

                        *decode_line = vc_memcpy;
                        decoder->decoder_type = LINE_DECODER;

                        if(desc.color_spec == RGBA || /* another exception - we may change shifts */
                                        desc.color_spec == RGB) { // should RGB be also handled
                                *decode_line = get_decoder_from_to(desc.color_spec, desc.color_spec);
                        }

                        out_codec = codec;
                        goto after_linedecoder_lookup;
                }
        }
        /* otherwise if we have line decoder (incl. slow codecs) */
        {
                vector<codec_t> native_codecs_copy = decoder->native_codecs;
                native_codecs_copy.push_back(VIDEO_CODEC_NONE); // this needs to be NULL-terminated
                *decode_line = get_best_decoder_from(desc.color_spec, native_codecs_copy.data(), &out_codec, true);
                if (*decode_line) {
                        decoder->decoder_type = LINE_DECODER;
                        goto after_linedecoder_lookup;
                }
        }

after_linedecoder_lookup:

        /* we didn't find line decoder. So try now regular (aka DXT) decoder */
        if(*decode_line == NULL) {
                decoder->decompress_state.resize(decoder->max_substreams);

                // try to probe video format
                if (comp_int_fmt == VIDEO_CODEC_NONE && decoder->out_codec != VIDEO_CODEC_END) {
                        bool supports_autodetection = decompress_init_multi(desc.color_spec,
                                        VIDEO_CODEC_NONE, VIDEO_CODEC_NONE, decoder->decompress_state.data(),
                                        decoder->decompress_state.size());
                        if (supports_autodetection) {
                                decoder->decoder_type = EXTERNAL_DECODER;
                                return VIDEO_CODEC_END;
                        }
                }

                vector<pair<codec_t, codec_t>> formats_to_try; // (comp_int_fmt || VIDEO_CODEC_NONE), display_fmt
                formats_to_try = video_decoder_order_output_codecs(comp_int_fmt, decoder->native_codecs);

                for (auto it = formats_to_try.begin(); it != formats_to_try.end(); ++it) {
                        out_codec = (*it).second;
                        if (decompress_init_multi(desc.color_spec, (*it).first,
                                                (*it).second,
                                                decoder->decompress_state.data(),
                                                decoder->decompress_state.size())) {
                                decoder->decoder_type = EXTERNAL_DECODER;
                                goto after_decoder_lookup;
                        }
                }
                decoder->decompress_state.clear();
        }
after_decoder_lookup:

        if(decoder->decoder_type == UNSET) {
                log_msg(LOG_LEVEL_ERROR, "Unable to find decoder for input codec \"%s\"!!!\n", get_codec_name(desc.color_spec));
                LOG(LOG_LEVEL_INFO) << "Compression internal codec is \"" << get_codec_name(comp_int_fmt) << "\". Native codecs are: " << codec_list_to_str(decoder->native_codecs) << "\n";
                return VIDEO_CODEC_NONE;
        }

        return out_codec;
}

/**
 * This function finds interlacing mode changing function.
 *
 * @param[in]  in_il       input_interlacing
 * @param[in]  supported   list of supported output interlacing modes
 * @param[in]  il_out_cnt  count of supported items
 * @param[out] out_il      selected output interlacing
 * @return                 selected interlacing changing function, NULL if not needed or not found
 */
static change_il_t select_il_func(enum interlacing_t in_il, enum interlacing_t *supported,
                int il_out_cnt, /*out*/ enum interlacing_t *out_il)
{
        struct transcode_t { enum interlacing_t in; enum interlacing_t out; change_il_t func; };

        struct transcode_t transcode[] = {
                {LOWER_FIELD_FIRST, INTERLACED_MERGED, il_lower_to_merged},
                {UPPER_FIELD_FIRST, INTERLACED_MERGED, il_upper_to_merged},
                {INTERLACED_MERGED, UPPER_FIELD_FIRST, il_merged_to_upper}
        };

        int i;
        /* first try to check if it can be nativelly displayed */
        for (i = 0; i < il_out_cnt; ++i) {
                if(in_il == supported[i]) {
                        *out_il = in_il;
                        return NULL;
                }
        }

        for (i = 0; i < il_out_cnt; ++i) {
                size_t j;
                for (j = 0; j < sizeof(transcode) / sizeof(struct transcode_t); ++j) {
                        if(in_il == transcode[j].in && supported[i] == transcode[j].out) {
                                *out_il = transcode[j].out;
                                return transcode[j].func;
                        }
                }
        }

        log_msg(LOG_LEVEL_WARNING, "[Warning] Cannot find transition between incoming and display "
                        "interlacing modes!\n");
        return NULL;
}

/**
 * Reconfigures decoder if network received video data format has changed.
 *
 * @param decoder       the video decoder state
 * @param desc          new video description to be reconfigured to
 * @return              boolean value if reconfiguration was successful
 *
 * @invariant
 * decoder->display != NULL
 */
static bool reconfigure_decoder(struct state_video_decoder *decoder,
                struct video_desc desc, codec_t comp_int_fmt)
{
        codec_t out_codec;
        decoder_t decode_line;
        enum interlacing_t display_il = PROGRESSIVE;
        //struct video_frame *frame;
        int display_requested_pitch = PITCH_DEFAULT;
        int display_requested_rgb_shift[] = DEFAULT_RGB_SHIFT_INIT;

        // this code forces flushing the pipelined data
        video_decoder_stop_threads(decoder);
        if (decoder->frame)
                display_put_frame(decoder->display, decoder->frame, PUTF_DISCARD);
        decoder->frame = NULL;
        video_decoder_start_threads(decoder);

        cleanup(decoder);

        desc.tile_count = get_video_mode_tiles_x(decoder->video_mode)
                        * get_video_mode_tiles_y(decoder->video_mode);

        out_codec = choose_codec_and_decoder(decoder, desc, &decode_line, comp_int_fmt);
        if (out_codec == VIDEO_CODEC_NONE) {
                LOG(LOG_LEVEL_ERROR) << "Could not find neither line conversion nor decompress from " <<
                        get_codec_name(desc.color_spec) << " to display supported formats (" << codec_list_to_str(decoder->native_codecs) << ").\n";
                return false;
        }
        decoder->out_codec = out_codec;
        struct video_desc display_desc = desc;

        int display_mode;
        size_t len = sizeof(int);
        int ret;

        ret = display_ctl_property(decoder->display, DISPLAY_PROPERTY_VIDEO_MODE,
                        &display_mode, &len);
        if(!ret) {
                debug_msg("Failed to get video display mode.\n");
                display_mode = DISPLAY_PROPERTY_VIDEO_MERGED;
        }

        if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED) {
                display_desc.width *= get_video_mode_tiles_x(decoder->video_mode);
                display_desc.height *= get_video_mode_tiles_y(decoder->video_mode);
                display_desc.tile_count = 1;
        }

        decoder->change_il = select_il_func(desc.interlacing, decoder->disp_supported_il,
                        decoder->disp_supported_il_cnt, &display_il);
        decoder->change_il_state.resize(decoder->max_substreams);
        if (out_codec != VIDEO_CODEC_END && !video_desc_eq(decoder->display_desc, display_desc)) {
                display_desc.interlacing = display_il;
                display_desc.color_spec = out_codec;
                int ret;
                /* reconfigure VO and give it opportunity to pass us pitch */
                ret = display_reconfigure(decoder->display, display_desc, decoder->video_mode);
                if(!ret) {
                        LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Unable to reconfigure display to "
                                << display_desc << "\n";
                        return false;
                }
                LOG(LOG_LEVEL_NOTICE) << MOD_NAME << "Successfully reconfigured display to "
                        << display_desc << "\n";
                decoder->display_desc = display_desc;

                len = sizeof(display_requested_rgb_shift);
                ret = display_ctl_property(decoder->display, DISPLAY_PROPERTY_RGB_SHIFT,
                                &display_requested_rgb_shift, &len);
                if(!ret) {
                        debug_msg("Failed to get r,g,b shift property from video driver.\n");
                        int rgb_shift[] = DEFAULT_RGB_SHIFT_INIT;
                        memcpy(&display_requested_rgb_shift, rgb_shift, sizeof(rgb_shift));
                }

                ret = display_ctl_property(decoder->display, DISPLAY_PROPERTY_BUF_PITCH,
                                &display_requested_pitch, &len);
                if(!ret) {
                        debug_msg("Failed to get pitch from video driver.\n");
                        display_requested_pitch = PITCH_DEFAULT;
                }
        }

        int linewidth;
        if (display_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                linewidth = desc.width;
        } else {
                linewidth = desc.width * get_video_mode_tiles_x(decoder->video_mode);
        }

        if(display_requested_pitch == PITCH_DEFAULT)
                decoder->pitch = vc_get_linesize(linewidth, out_codec);
        else
                decoder->pitch = display_requested_pitch;


        int src_x_tiles = get_video_mode_tiles_x(decoder->video_mode);
        int src_y_tiles = get_video_mode_tiles_y(decoder->video_mode);

        if(decoder->decoder_type == LINE_DECODER) {
                decoder->line_decoder = (struct line_decoder *) malloc(src_x_tiles * src_y_tiles *
                                        sizeof(struct line_decoder));
                if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED && decoder->video_mode == VIDEO_NORMAL) {
                        struct line_decoder *out = &decoder->line_decoder[0];
                        out->base_offset = 0;
                        out->conv_num = get_pf_block_pixels(desc.color_spec) * get_pf_block_bytes(out_codec);
                        out->conv_den = get_pf_block_bytes(desc.color_spec) * get_pf_block_pixels(out_codec);
                        memcpy(out->shifts, display_requested_rgb_shift, 3 * sizeof(int));

                        out->decode_line = decode_line;
                        out->dst_pitch = decoder->pitch;
                        out->src_linesize = vc_get_linesize(desc.width, desc.color_spec);
                        out->dst_linesize = vc_get_linesize(desc.width, out_codec);
                        decoder->merged_fb = true;
                } else if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED
                                && decoder->video_mode != VIDEO_NORMAL) {
                        int x, y;
                        for(x = 0; x < src_x_tiles; ++x) {
                                for(y = 0; y < src_y_tiles; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x +
                                                        src_x_tiles * y];
                                        out->base_offset = y * (desc.height)
                                                        * decoder->pitch +
                                                        vc_get_linesize(x * desc.width, out_codec);

                                        out->conv_num = get_pf_block_pixels(desc.color_spec) * get_pf_block_bytes(out_codec);
                                        out->conv_den = get_pf_block_bytes(desc.color_spec) * get_pf_block_pixels(out_codec);
                                        memcpy(out->shifts, display_requested_rgb_shift,
                                                        3 * sizeof(int));

                                        out->decode_line = decode_line;

                                        out->dst_pitch = decoder->pitch;
                                        out->src_linesize =
                                                vc_get_linesize(desc.width, desc.color_spec);
                                        out->dst_linesize =
                                                vc_get_linesize(desc.width, out_codec);
                                }
                        }
                        decoder->merged_fb = true;
                } else if (display_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                        int x, y;
                        for(x = 0; x < src_x_tiles; ++x) {
                                for(y = 0; y < src_y_tiles; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x +
                                                        src_x_tiles * y];
                                        out->base_offset = 0;
                                        out->conv_num = get_pf_block_pixels(desc.color_spec) * get_pf_block_bytes(out_codec);
                                        out->conv_den = get_pf_block_bytes(desc.color_spec) * get_pf_block_pixels(out_codec);
                                        memcpy(out->shifts, display_requested_rgb_shift,
                                                        3 * sizeof(int));

                                        out->decode_line = decode_line;
                                        out->src_linesize =
                                                vc_get_linesize(desc.width, desc.color_spec);
                                        out->dst_pitch =
                                                out->dst_linesize =
                                                vc_get_linesize(desc.width, out_codec);
                                }
                        }
                        decoder->merged_fb = false;
                }
        } else if (decoder->decoder_type == EXTERNAL_DECODER) {
                int buf_size;

                for(unsigned int i = 0; i < decoder->decompress_state.size(); ++i) {
                        buf_size = decompress_reconfigure(decoder->decompress_state.at(i), desc,
                                        display_requested_rgb_shift[0],
                                        display_requested_rgb_shift[1],
                                        display_requested_rgb_shift[2],
                                        decoder->pitch,
                                        out_codec == VIDEO_CODEC_END ? VIDEO_CODEC_NONE : out_codec);
                        if(!buf_size) {
                                return false;
                        }
                }
                decoder->merged_fb = display_mode != DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES;
                int res = 0, ret;
                size_t size = sizeof(res);
                ret = decompress_get_property(decoder->decompress_state.at(0),
                                DECOMPRESS_PROPERTY_ACCEPTS_CORRUPTED_FRAME,
                                &res, &size);
                decoder->accepts_corrupted_frame = ret && res;
        }

        // Pass metadata to receiver thread (it can tweak parameters)
        struct msg_receiver *msg = (struct msg_receiver *)
                new_message(sizeof(struct msg_receiver));
        msg->type = RECEIVER_MSG_VIDEO_PROP_CHANGED;
        msg->new_desc = decoder->received_vid_desc;
        struct response *resp =
                send_message_to_receiver(decoder->mod.parent, (struct message *) msg);
        free_response(resp);

        if (out_codec != VIDEO_CODEC_END) {
                decoder->frame = display_get_frame(decoder->display);
        }

        return true;
}

bool parse_video_hdr(uint32_t *hdr, struct video_desc *desc)
{
        uint32_t tmp;
        int fps_pt, fpsd, fd, fi;

        tmp = ntohl(hdr[0]);
        desc->tile_count = (tmp >> 22) + 1; // a bit hacky - assuming this packet is from last substream

        desc->width = ntohl(hdr[3]) >> 16;
        desc->height = ntohl(hdr[3]) & 0xffff;
        desc->color_spec = get_codec_from_fcc(hdr[4]);
        if(desc->color_spec == VIDEO_CODEC_NONE) {
                log_msg(LOG_LEVEL_ERROR, "Unknown FourCC \"%.4s\"!\n", (char *) &hdr[4]);
                return false;
        }

        tmp = ntohl(hdr[5]);
        desc->interlacing = (enum interlacing_t) (tmp >> 29);
        fps_pt = (tmp >> 19) & 0x3ff;
        fpsd = (tmp >> 15) & 0xf;
        fd = (tmp >> 14) & 0x1;
        fi = (tmp >> 13) & 0x1;

        desc->fps = compute_fps(fps_pt, fpsd, fd, fi);

        if (desc->fps == -1) {
                LOG_ONCE(LOG_LEVEL_WARNING, to_fourcc('U', 'F', 'P', 'S'), MOD_NAME "Unsupported FPS received (newer UG?), setting to 30.\n");
                desc->fps = 30.0;
        }

        return true;
}

/**
 * @retval TRUE  if reconfiguration occured
 * @retval FALSE reconfiguration was not needed
 */
static int reconfigure_if_needed(struct state_video_decoder *decoder,
                struct video_desc network_desc,
                bool force = false, codec_t comp_int_fmt = VIDEO_CODEC_NONE)
{
        bool desc_changed = !video_desc_eq_excl_param(decoder->received_vid_desc, network_desc, PARAM_TILE_COUNT);
        if(!desc_changed && !force)
                return FALSE;

        if (desc_changed) {
                LOG(LOG_LEVEL_NOTICE) << "[video dec.] New incoming video format detected: " << network_desc << endl;
                decoder->received_vid_desc = network_desc;
        }

        if(force){
                log_msg(LOG_LEVEL_VERBOSE, "forced reconf\n");
        }

#ifdef RECONFIGURE_IN_FUTURE_THREAD
        decoder->reconfiguration_in_progress = true;
        decoder->reconfiguration_future = std::async(std::launch::async,
                        [decoder](){ return reconfigure_decoder(decoder, decoder->received_vid_desc); });
#else
        int ret = reconfigure_decoder(decoder, decoder->received_vid_desc, comp_int_fmt);
        if (!ret) {
                log_msg(LOG_LEVEL_ERROR, "[video dec.] Reconfiguration failed!!!\n");
                decoder->frame = NULL;
                decoder->out_codec = VIDEO_CODEC_NONE;
        }
#endif
        return TRUE;
}
/**
 * Checks if network format has changed.
 *
 * @param decoder decoder state
 * @param hdr     raw RTP payload header
 *
 * @return TRUE if format changed (and reconfiguration was successful), FALSE if not
 */
static int check_for_mode_change(struct state_video_decoder *decoder,
                uint32_t *hdr)
{
        struct video_desc network_desc;

        parse_video_hdr(hdr, &network_desc);
        return reconfigure_if_needed(decoder, network_desc);
}

#define ERROR_GOTO_CLEANUP ret = FALSE; goto cleanup;
#define max(a, b)       (((a) > (b))? (a): (b))

/**
 * A structure to define the FEC Data.
 */
struct FecData {
    // K is the amount of blocks for the original data
    unsigned int k;
    // M is the amount of additional blocks
    unsigned int m;
    // C and Seed and LDGM specific variables
    unsigned int c;
    unsigned int seed;
};

struct PacketData {
    char* data;
    unsigned int* header;
    size_t length;
    size_t position;
    unsigned int substream;
};

// Forward declare the functions
FecData getFecData(rtp_packet& pckt);
bool checkDecryption(state_video_decoder* decoder, rtp_packet& pckt);
void buildFrame(const std::vector<PacketData>& packetDataVec, UIntPair block, bool decrypt, openssl_mode cryptoMode,
                state_video_decoder* decoder, unsigned int packetType, size_t bufferLength, video_frame* frame,
                map<int, int>* fecPacketData, openssl_decrypt* decryption);

/**
 *  @brief A function for getting the FEC data from the packet. This is done
 *         by checking the packet type, and then extracting the data (or returning)
 *         a default otherwise.
 */
FecData getFecData(rtp_packet& pckt) {
    FecData fecData;
    // Check if the packet is a FEC packet type
    if(PT_VIDEO_HAS_FEC(pckt.pt)) {
        // Grab the FEC parts of the header from the packet header
        unsigned int* header = (unsigned int *) pckt.data;
        unsigned int fecHeader = ntohl(header[3]);
        unsigned int fecSeed = ntohl(header[4]);
        
        // Convert the bit-shifted header information back into the FEC
        // data, and write back into the object.
        fecData.k = fecHeader >> 19;
        fecData.m = 0x1fff & (fecHeader >> 6);
        fecData.c = 0x3f & fecHeader;
        fecData.seed = fecSeed;
    }
    else {
        // Select a default for all of the FEC values if the packet is not
        // an FEC packet.
        fecData.k = 0;
        fecData.m = 0;
        fecData.c = 0;
        fecData.k = 0;
    }
    return fecData;
}

/**
 *  @brief Check the decoder to ensure it holds the correct state for the decryption object.
 */
bool checkDecryption(state_video_decoder* decoder, rtp_packet& pckt) {
    // Figure out if the packet is encrypted and whether or not the decoder
    // decryption structure has been initialised.
    bool isPacketEncrypted = PT_VIDEO_IS_ENCRYPTED(pckt.pt);
    bool decoderDecryptionIsSet = decoder->decrypt != nullptr;

    // If the packet has been encrypted and there is no decryption state or
    // if the packet is not encrypted and there is a decryption state then log an error
    if ((isPacketEncrypted && !decoderDecryptionIsSet) || (!isPacketEncrypted && decoderDecryptionIsSet)) {
        log_msg(LOG_LEVEL_ERROR, decoderDecryptionIsSet ? NOT_ENCRYPTED_ERR : ENCRYPTED_ERR);
        return false;
    }
    else {
        return true;
    }
}

/**
 * @brief A function for building up a frame from a range of packets. This function should be thread safe,
 *        but it does perform concurrent writes into the same block of memory. This should not be a problem
 *        as each packet should represent a unique area of the memory OR be a duplicate block of memory (and therefore)
 *        not matter in the race condition this generates.
 */
void buildFrame(const std::vector<PacketData>& packetDataVec, UIntPair block, bool decrypt, openssl_mode cryptoMode,
                state_video_decoder* decoder, unsigned int packetType, size_t bufferLength, video_frame* frame,
                map<int, int>* fecPacketData, openssl_decrypt* decryption)
{
    int prints = 0;

    // Get an iterator to the beginning of the block being processed by this function
    auto packetDataIt = packetDataVec.begin() + block.first;
    for(unsigned int i = 0; i < block.second; i++) {
        // Get the packet data from the iterator
        PacketData packetData = *packetDataIt;
        // The given packet length is always larger or equal to the 
        // size of the decrypted packet
        char decryptedData[packetData.length];
        if(decrypt) {
            size_t headerSize = packetType == PT_ENCRYPT_VIDEO ? sizeof(video_payload_hdr_t) : sizeof(fec_payload_hdr_t);
            int decryptedLength = decoder->dec_funcs->decrypt(decryption, packetData.data, packetData.length,
                                                              (char *) packetData.header, headerSize, decryptedData, cryptoMode);
            packetData.data = decryptedData;
            packetData.length = decryptedLength;
        }

        // Create a context to capture the lock whilst writing to the fecPacketData
        {
            std::unique_lock<std::mutex> stateLock = std::unique_lock<std::mutex>(decoder->lock);
            fecPacketData[packetData.substream][(int) packetData.position] = packetData.length;
        }

        // Ensure that the data we are given is valid
        if(packetData.length > 0) {
            size_t len = packetData.length;

            if ((packetType == PT_VIDEO || packetType == PT_ENCRYPT_VIDEO) && decoder->decoder_type == LINE_DECODER) {
                struct tile* tile = nullptr;

                if (!decoder->merged_fb) {
                        tile = vf_get_tile(decoder->frame, (int) packetData.substream);
                } else {
                        tile = vf_get_tile(decoder->frame, 0);
                }

                line_decoder *line_decoder = &decoder->line_decoder[packetData.substream];

                // MAGIC, don't touch it, you definitely break it
                // *source* is data from network, *destination* is frame buffer

                // Compute Y pos in source frame and convert it to
                // byte offset in the destination frame
                int y = (packetData.position / line_decoder->src_linesize) * line_decoder->dst_pitch;

                // Compute X pos in source frame
                int sX = packetData.position % line_decoder->src_linesize;

                // Convert X pos from source frame into the destination frame.
                // it is byte offset from the beginning of a line
                int dX = sX * line_decoder->conv_num / line_decoder->conv_den;

                // Pointer to data payload in packet
                auto source = reinterpret_cast<unsigned char*>(packetData.data);

                /* copy whole packet that can span several lines.
                    * we need to clip data (v210 case) or center data (RGBA, R10k cases)
                    */
                while (len > 0) {
                    /* len id payload length in source BPP
                        * decoder needs len in destination BPP, so convert it
                        */
                    int l = len * line_decoder->conv_num / line_decoder->conv_den;

                    /* do not copy multiple lines, we need to
                        * copy (& clip, center) line by line
                        */
                    if (l + dX > (int) line_decoder->dst_linesize) {
                        l = line_decoder->dst_linesize - dX;
                    }

                    /* compute byte offset in destination frame */
                    int offset = y + dX;

                    /* watch the SEGV */
                    if (static_cast<unsigned int>(l + line_decoder->base_offset + offset) <= tile->data_len) {
                        /*decode frame:
                            * we have offset for destination
                            * we update source contiguously
                            * we pass {r,g,b}shifts */
                        line_decoder->decode_line((unsigned char*)tile->data + line_decoder->base_offset + offset, source, l,
                                        line_decoder->shifts[0], line_decoder->shifts[1],
                                        line_decoder->shifts[2]);
                        /* we decoded one line (or a part of one line) to the end of the line
                            * so decrease *source* len by 1 line (or that part of the line */
                        len -= line_decoder->src_linesize - sX;
                        /* jump in source by the same amount */
                        source += line_decoder->src_linesize - sX;
                    }
                    else {
                        /* this should not ever happen as we call reconfigure before each packet
                            * iff reconfigure is needed. But if it still happens, something is terribly wrong
                            * say it loudly
                            */
                        if((prints % 100) == 0) {
                                log_msg(LOG_LEVEL_ERROR, "WARNING!! Discarding input data as frame buffer is too small.\n"
                                                "Well this should not happened. Expect troubles pretty soon.\n");
                        }
                        prints++;
                        len = 0;
                    }
                    /* each new line continues from the beginning */
                    dX = 0;        /* next line from beginning */
                    sX = 0;
                    y += line_decoder->dst_pitch;  /* next line */
                }
            } 
            // PT_VIDEO_LDGM or external decoder
            else { 
                if (packetData.position + len > bufferLength) {
                    if((prints % 100) == 0) {
                        log_msg(LOG_LEVEL_ERROR, "WARNING!! Discarding input data as frame buffer is too small.\n"
                                        "Well this should not happened. Expect troubles pretty soon.\n");
                    }
                    prints++;
                    len = max<int>(0, bufferLength - packetData.position);
                }

                if(packetData.data) {
                    // Simply copy the available memory into the frame. Meaningful processing will
                    // be completed on the data further on
                    memcpy(frame->tiles[packetData.substream].data + packetData.position, (unsigned char*) packetData.data, len);
                }
            }
        }
        
        // Iterate the packet iterator
        packetDataIt++;
    }
}

/**
 * @brief Decodes a participant buffer representing one video frame.
 * @param cdata        PBUF buffer
 * @param decoder_data @ref vcodec_state containing decoder state and some additional data
 * @retval TRUE        if decoding was successful.
 *                     It stil doesn't mean that the frame will be correctly displayed,
 *                     decoding may fail in some subsequent (asynchronous) steps.
 * @retval FALSE       if decoding failed
 */
bool decode_video_frame(std::unique_ptr<BufferFrame> bufferFrame, vcodec_state* playoutBufState, PlayoutBufferStats stats)
{
    // Fetch the decoder from the state
    state_video_decoder* decoder = playoutBufState->decoder;

    bool ret = true;
    rtp_packet* pckt = NULL;
    int maxSubstreams = decoder->max_substreams;
    uint32_t ssrc = 0U;
    unsigned int frame_size = 0;

    std::vector<uint32_t> bufferNum(maxSubstreams);

    // Allocate memory for the video frame being built
    struct video_frame* frame = vf_alloc(maxSubstreams);
    frame->callbacks.data_deleter = vf_data_deleter;
    // The following is just FEC related optimisation - normally we fill up
    // allocated buffers when we have compressed data. But in case of FEC, there
    // is just the FEC buffer present, so we point to it instead of copying
    unique_ptr<map<int, int>[]> fecPacketData(new map<int, int>[maxSubstreams]);

    FecData fecData = {};

    int bufferNumber = 0;
    int bufferLength = 0;
    int packetType = 0;
    bool decrypt = false;

    // Check if there is a frame buffer assigned to the decoder. If not, then
    // error out.
    if(!decoder->display) {
        LOG(LOG_LEVEL_ERROR) << "No frame buffer has been assigned to the decoder\n";
        // Free the video frame we assigned and return a failure
        vf_free(frame);
        return false;
    }

#ifdef RECONFIGURE_IN_FUTURE_THREAD
    // check if we are not in the middle of reconfiguration
    if (decoder->reconfiguration_in_progress) {
        std::future_status status =
            decoder->reconfiguration_future.wait_until(std::chrono::system_clock::now());
        if (status == std::future_status::ready) {
            bool ret = decoder->reconfiguration_future.get();
            if (ret) {
                decoder->frame = display_get_frame(decoder->display);
            } else {
                log_msg(LOG_LEVEL_ERROR, "Decoder reconfiguration failed!!!\n");
                decoder->frame = NULL;
            }
            decoder->reconfiguration_in_progress = false;
        } else {
            // skip the frame if we are not yet reconfigured
            vf_free(frame);
            return FALSE;
        }
    }
#endif

    // A messaging system used by other threads to indicate that a reconfiguration is needed. Pop the messages from the queue
    // and check the contents of it. (Not sure this is thread safe as there doesn't appear to be a lock, when other threads could
    // be writing into it).
    main_msg_reconfigure *msg_reconf;
    while ((msg_reconf = decoder->msg_queue.pop(true /* nonblock */))) {
        if (reconfigure_if_needed(decoder, msg_reconf->desc, msg_reconf->force, msg_reconf->compress_internal_codec)) {
#ifdef RECONFIGURE_IN_FUTURE_THREAD
            vf_free(frame);
            return FALSE;
#endif
        }
        if (msg_reconf->last_frame) {
                decoder->fec_queue.push(std::move(msg_reconf->last_frame));
        }
        delete msg_reconf;
    }

    // Validate the decryption settings
    if(bufferFrame->size() > 0) {
        if(auto packetWrapper = bufferFrame->getPacket(0)) {
            // Extract the reference from the optional
            rtp_packet& packet = packetWrapper->get();

            // Validate decryption settings are correct
            if(!checkDecryption(decoder, packet)) {
                // Free the video frame we assigned and return a failure
                vf_free(frame);
                return false;
            }

            // Get any FEC settings
            fecData = getFecData(packet);
            decrypt = PT_VIDEO_IS_ENCRYPTED(packet.pt);

            if (!PT_VIDEO_HAS_FEC(packet.pt))
            {
                /* Critical section
                 * each thread *MUST* wait here if this condition is true
                 */
                // Set the pointer to the beginning of the header from the packet
                auto header = (unsigned int*) packet.data;
                if (check_for_mode_change(decoder, header)) {
#ifdef RECONFIGURE_IN_FUTURE_THREAD
                    vf_free(frame);
                                return FALSE;
#endif
                }

                // hereafter, display framebuffer can be used, so we
                // check if we got it
                if (FRAMEBUFFER_NOT_READY(decoder)) {
                    vf_free(frame);
                    return FALSE;
                }
            }
        }
    }
    else {
        LOG(LOG_LEVEL_ERROR) << "Frame buffer has no packets for decoding\n";
        // Free the video frame we assigned and return a failure
        vf_free(frame);
        return false;
    }

    unsigned int largestSubstream = 0;
    openssl_mode cryptoMode = MODE_AES128_NONE;

    ThreadPool<openssl_decrypt*> threadPool = ThreadPool<openssl_decrypt*>(decoder->decryption_resources, decoder->thread_count);

    std::vector<PacketData> videoPackets = std::vector<PacketData>();
    PacketData packetData;

    // Calculate all blocks for every packet except the last one (because that needs to be sent last)
    Blocks packetBlocks = createBlocks(bufferFrame->size(), 8);

    for(std::unique_ptr<rtp_packet>& packet : *bufferFrame) {
        // Set up a pointer to the header of the packet
        unsigned int* header;
        
        // Create variables for saving the length of the data in the packet,
        // a pointer to the data itself, the position of the data in the frame,
        // and the datas substream. We are also interested if the data is 
        // encrypted and with what mode
        char* data;
        size_t dataLen;
        size_t dataPos;
        unsigned int substream;
        
        // Extract the ssrc, and packet type from packet
        packetType = packet->pt;
        ssrc = packet->ssrc;

        // Set the pointer to the beginning of the header from the packet
        header = (unsigned int*) packet->data;
        // The substream and buffer number are in the 1st element of the header
        unsigned int headerSubBuf = ntohl(header[0]);
        substream = headerSubBuf >> 22;
        bufferNumber = headerSubBuf & 0x3fffff;
        // The position of the data is in the 2nd element of the header
        dataPos = (size_t) ntohl(header[1]);
        // The buffer length is in the 3rd element of the header
        bufferLength = ntohl(header[2]);

        // Store the largest seen substream so we can check to see if the decoder
        // is correctly configured
        largestSubstream = std::max<unsigned int>(largestSubstream, substream);

        switch(packetType) {
            // If the video is not encrypted, then the data and the length are just simple functions
            // of manipulating the base of the packet and it's length by the size of the header.
            case PT_VIDEO:
                {
                    size_t videoPayloadHeaderSize = sizeof(video_payload_hdr_t);
                    dataLen = packet->data_len - videoPayloadHeaderSize;
                    data = (char*)(packet->data) + videoPayloadHeaderSize;
                }
                break;
            case PT_VIDEO_RS:
            case PT_VIDEO_LDGM:
                {
                    size_t fecPayloadHeaderSize = sizeof(fec_payload_hdr_t);
                    dataLen = packet->data_len - fecPayloadHeaderSize;
                    data = (char*)(packet->data) + fecPayloadHeaderSize;
                }
                break;
            // If the video is encrypted then we need to extract the crypto mode so that we can properly 
            // decrypt the data
            case PT_ENCRYPT_VIDEO:
            case PT_ENCRYPT_VIDEO_LDGM:
            case PT_ENCRYPT_VIDEO_RS:
                {
                    // Grab the size of the headers
                    size_t cryptoHeaderSize = sizeof(crypto_payload_hdr_t);
                    size_t mediaHeaderSize;
                    if(packetType == PT_ENCRYPT_VIDEO) {
                        mediaHeaderSize = sizeof(video_payload_hdr_t);
                    }
                    else {
                        mediaHeaderSize = sizeof(fec_payload_hdr_t);
                    }

                    // Calculate the length of the data and the data
                    dataLen = packet->data_len - cryptoHeaderSize - mediaHeaderSize;
                    data = (char*)(packet->data) + cryptoHeaderSize + mediaHeaderSize;
                    // Calculate where the crypto header is and grab the 
                    // first 8 bits for the crypto mode
                    unsigned int cryptoHeader = ntohl(*(header + (mediaHeaderSize / sizeof(unsigned int))));
                    cryptoMode = (openssl_mode)(cryptoHeader >> 24);

                    // Check we have been given a valid crypto mode
                    if(cryptoMode == MODE_AES128_NONE || cryptoMode > MODE_AES128_MAX) {
                        LOG(LOG_LEVEL_WARNING) << "Unknown cipher mode: " << (int) cryptoMode << " - Dropping frame\n";
                        ret = false;
                        goto cleanup;
                    }
                }
                break;
            default:
                {
                    if (packetType == PT_Unassign_Type95) {
                        LOG_ONCE(LOG_LEVEL_WARNING, to_fourcc('U', 'V', 'P', 'T'), MOD_NAME "Unassigned PT 95 received, ignoring.\n");
                    }
                    else {
                        LOG(LOG_LEVEL_WARNING) << MOD_NAME "Unknown packet type: " << pckt->pt << ".\n";
                    }
                    ret = false;
                    goto cleanup;
                }
        }

        // Store the packets in a list that we can process later
        packetData.position = dataPos;
        packetData.substream = substream;
        packetData.length = dataLen;
        packetData.data = data;
        packetData.header = header;
        videoPackets.emplace_back(packetData);

        bufferNum[substream] = bufferNumber;
    }

    // Check to see if the largest substream found when processing the packets exceeds 
    // the maximum set for the frame.
    if ((int) largestSubstream >= maxSubstreams) {
        log_msg(LOG_LEVEL_WARNING, "[decoder] received substream ID %d. Expecting at most %d substreams. Did you set -M option?\n", largestSubstream, maxSubstreams);
        // The guess is valid - we start with highest substream number (anytime - since it holds a m-bit)
        // in next iterations, index is valid
        video_mode video_mode = guess_video_mode(largestSubstream + 1);
        if (video_mode != VIDEO_UNKNOWN) {
            // Try reconfiguring the decoder
            log_msg(LOG_LEVEL_NOTICE, "[decoder] Guessing mode: ");
            decoder_set_video_mode(decoder, video_mode);
            // Just for sure, that we reconfigure in next iteration
            decoder->received_vid_desc.width = 0;
            log_msg(LOG_LEVEL_NOTICE, "%s. Check if it is correct.\n", get_video_mode_description(decoder->video_mode));
        } 
        else {
            log_msg(LOG_LEVEL_FATAL, "[decoder] Unknown video mode!\n");
            handle_error(1);
        }
        // We need skip this frame (variables are illegal in this iteration
        // and in case that we got unrecognized number of substreams - exit
        ret = false;
        goto cleanup;
    }

    // Allocate memory if we're looking at an LDGM frame, or wait for the frame buffer swap to occur
    if ((packetType == PT_VIDEO || packetType == PT_ENCRYPT_VIDEO) && decoder->decoder_type == LINE_DECODER) {
        // Wait for the buffer swap to occur
        wait_for_framebuffer_swap(decoder);
        unique_lock<mutex> lk(decoder->lock);
        decoder->buffer_swapped = false;
    }
    else {
        for(int i = 0; i < static_cast<int>(largestSubstream) + 1; i++) {
            frame->tiles[i].data = (char *) malloc(bufferLength + PADDING);
            frame->tiles[i].data_len = bufferLength;
        }
    }

    threadPool.Start();
    for(UIntPair block : packetBlocks) {
        // Use lambda to construct function to pass into thread
        std::function<void(struct openssl_decrypt*)> job = [block, videoPackets, cryptoMode, decoder, packetType, bufferLength, frame, &fecPacketData, decrypt]
        // Our thread pools allow per thread resources to be sent, so send encryption block
        (struct openssl_decrypt* decryption) 
        {
            // Build frame
            buildFrame(videoPackets, block, decrypt, cryptoMode, decoder, packetType, bufferLength, frame, fecPacketData.get(), decryption);
        };
        threadPool.QueueJob(job);
    }
    // Wait for the thread pool to finish executing, and then shut down the threads
    while(threadPool.Busy());
    threadPool.Stop();

    if (FRAMEBUFFER_NOT_READY(decoder) && (packetType == PT_VIDEO || packetType == PT_ENCRYPT_VIDEO)) {
        LOG(LOG_LEVEL_INFO) << "Frame buffer not ready\n";
        ret = false;
        goto cleanup;
    }

    assert(ret);

    for(int i = 0; i < maxSubstreams; ++i) {
        frame_size += frame->tiles[i].data_len;
    }

    /// Zero missing parts of framebuffer - this may be useful for compressed video
    /// (which may be also with FEC - but we use systematic codes therefore it may
    /// benefit from that as well).
    if (decoder->decoder_type != LINE_DECODER) {
        for(int i = 0; i < maxSubstreams; ++i) {
            unsigned int last_end = 0;

            for (auto const & packets : fecPacketData[i]) {
                unsigned int start = packets.first;
                unsigned int len = packets.second;
                if (last_end < start) {
                    memset(frame->tiles[i].data + last_end, 0, start - last_end);
                }
                last_end = start + len;
            }
            if (last_end < frame->tiles[i].data_len) {
                memset(frame->tiles[i].data + last_end, 0, frame->tiles[i].data_len - last_end);
            }
        }
    }

    // format message
    {
        unique_ptr <frame_msg> fec_msg (new frame_msg(decoder->control, decoder->stats));
        fec_msg->buffer_num = std::move(bufferNum);
        fec_msg->recv_frame = frame;
        frame = NULL;
        fec_msg->recv_frame->fec_params = fec_desc(fec::fec_type_from_pt(packetType), fecData.k, fecData.m, fecData.c, fecData.seed);
        fec_msg->recv_frame->ssrc = ssrc;
        fec_msg->pckt_list = std::move(fecPacketData);
        fec_msg->received_pkts_cum = stats.getReceivedPacketsTotal();
        fec_msg->expected_pkts_cum = stats.getExpectedPacketsTotal();

        auto t0 = std::chrono::steady_clock::now();
        decoder->fec_queue.push(std::move(fec_msg));
        auto t1 = std::chrono::steady_clock::now();
        double tpf = 1.0 / decoder->display_desc.fps;
        if (std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count() > tpf && decoder->stats.displayed > 20) {
                decoder->slow_msg.print("Your computer may be too SLOW to play this !!!\n");
        }

    }
cleanup:
    ;
    if(!ret) {
        vf_free(frame);
    }

    playoutBufState->max_frame_size = max(playoutBufState->max_frame_size, frame_size);
    playoutBufState->decoded++;

    decoder->stats.update(bufferNumber);

    return ret;
}

static void decoder_process_message(struct module *m)
{
        struct state_video_decoder *s = (struct state_video_decoder *) m->priv_data;

        struct message *msg;
        while ((msg = check_message(m))) {
                struct response *r;
                struct msg_universal *m_univ = (struct msg_universal *) msg;
                if (strcmp(m_univ->text, "get_format") == 0) {
                        s->lock.lock();
                        string video_desc = s->received_vid_desc;
                        s->lock.unlock();
                        r = new_response(RESPONSE_OK, video_desc.c_str());
                } else {
                        r = new_response(RESPONSE_NOT_FOUND, NULL);
                }
                free_message(msg, r);

        }
}

