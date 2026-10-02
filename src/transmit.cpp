/*
 * FILE:    transmit.cpp
 * AUTHOR:  Colin Perkins <csp@csperkins.org>
 *          Ladan Gharai
 *          Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *          David Cassany    <david.cassany@i2cat.net>
 *          Ignacio Contreras <ignacio.contreras@i2cat.net>
 *          Gerard Castillo  <gerard.castillo@i2cat.net>
 *          Jordi "Txor" Casas Ríos <txorlings@gmail.com>
 *          Martin Pulec     <pulec@cesnet.cz>
 *
 * Copyright (c) 2005-2010 Fundació i2CAT, Internet I Innovació Digital a Catalunya
 * Copyright (c) 2001-2004 University of Southern California
 * Copyright (c) 2005-2021 CESNET z.s.p.o.
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
 *      California Information Sciences Institute. This product also includes
 *      software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of the University, Institute, CESNET nor the names of
 *    its contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "audio/codec.h"
#include "audio/types.h"
#include "audio/utils.h"
#include "crypto/random.h"
#include "control_socket.h"
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "crypto/openssl_encrypt.h"
#include "module.h"
#include "rtp/fec.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/rtpenc_h264.h"
#include "tv.h"
#include "transmit.h"
#include "utils/jpeg_reader.h"
#include "utils/misc.h" // unit_evaluate
#include "video.h"
#include "video_codec.h"
#include "compat/platform_time.h"
#include "threadpool.hpp"
#include "utils/utility.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <utility>
#include <string>
#include <vector>

#define MOD_NAME "[transmit] "
#define TRANSMIT_MAGIC	0xe80ab15f

#define FEC_MAX_MULT 10

#define CONTROL_PORT_BANDWIDTH_REPORT_INTERVAL_MS 1000

#define NANOSECOND 1000'000'000

#ifdef HAVE_MACOSX
#define GET_STARTTIME gettimeofday(&start, NULL)
#define GET_STOPTIME gettimeofday(&stop, NULL)
#define GET_DELTA delta = (stop.tv_sec - start.tv_sec) * 1000000000l + (stop.tv_usec - start.tv_usec) * 1000L
#elif defined HAVE_LINUX
#define GET_STARTTIME clock_gettime(CLOCK_REALTIME, &start)
#define GET_STOPTIME clock_gettime(CLOCK_REALTIME, &stop)
#define GET_DELTA delta = (stop.tv_sec - start.tv_sec) * 1000000000l + stop.tv_nsec - start.tv_nsec
#else // Windows
#define GET_STARTTIME {QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
#define GET_STOPTIME { QueryPerformanceCounter(&stop); }
#define GET_DELTA delta = (long)((double)(stop.QuadPart - start.QuadPart) * 1000 * 1000 * 1000 / freq.QuadPart);
#endif

#define DEFAULT_CIPHER_MODE MODE_AES128_GCM

using std::array;
using std::vector;

static void tx_update(struct tx *tx, struct video_frame *frame, int substream);
static void tx_done(struct module *tx);
static uint32_t format_interl_fps_hdr_row(enum interlacing_t interlacing, double input_fps);

static void
tx_send_base(struct tx *tx, struct video_frame *frame, struct rtp *rtpSession,
                uint32_t ts, int send_m,
                unsigned int substream,
                int fragment_offset);

void audio_tx_send_channel_segments(struct tx* tx, struct rtp *rtp_session, const audio_frame2 * buffer,
                                    int channel, size_t segmentOffset, uint32_t segmentCount, int pt, uint32_t timestamp);


static bool set_fec(struct tx *tx, const char *fec);
static void fec_check_messages(struct tx *tx);
uint64_t calculateFrameTarget(struct tx*, struct video_frame*);

struct rate_limit_dyn {
        unsigned long avg_frame_size;   ///< moving average
        long long last_excess; ///< nr of frames last excessive frame was emitted
        static constexpr int EXCESS_GAP = 4; ///< minimal gap between excessive frames
};

/**
 * A helper class for regularly reporting how long it is taking to send the packets
 */
class PacketTiming {
public:
    PacketTiming(size_t limit, MediaType mediaType) : limit(limit), mediaType(mediaType) {
        this->values = std::queue<long long>();
        this->total = 0;
        this->reportTiming = std::chrono::steady_clock::now();
    };

    /**
     * @brief A wrapper function for the start recording of the packet timing.
     */
    void start() {
        this->begin = std::chrono::steady_clock::now();
    }

    /**
     * @brief Measure the time that has elapsed since the start function has been called and record it as part
     *        of a moving average.
     */
    void measure() {
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        long long packetTiming = std::chrono::duration_cast<std::chrono::milliseconds>(end - this->begin).count();

        // If our queue has surpassed its limit, then remove an element
        if(this->values.size() > this->limit) {
            this->total -= this->values.front();
            this->values.pop();
        }

        // Expand the total and the values queue by this
        this->values.push(packetTiming);
        this->total += packetTiming;
    }

    /**
     * @brief Calculate the average timing for the packets.
     */
    double average() {
        size_t queueSize = this->values.size();
        if(queueSize > 0) {
            return static_cast<double>(this->total) / static_cast<double>(queueSize);
        }
        else {
            return 0.0;
        }
    }

    /**
     * @brief A regular report should be produced of the timings it takes to send packets
     *        (since this has been an issue it is something we want to track).
     *
     * @param packetCount
     */
    void report(size_t packetCount, uint32_t frameTarget = 0) {
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        long long timeSinceReport = std::chrono::duration_cast<std::chrono::seconds>(now - this->reportTiming).count();

        if(timeSinceReport > 10) {
            std::stringstream message;
            message << MOD_NAME << "Packet processing time average for "
                    << (this->mediaType == MediaType::MEDIA_VIDEO ? "video" : "audio")
                    << " is: " << this->average() << "ms with " << packetCount << " packets. ";

            // Optionally output the frame target so that this can be compared with the actual value.
            if(frameTarget != 0) {
                message << "Frame Target: " << frameTarget << "ns.";
            }
            message << "\n";

            LOG(LOG_LEVEL_INFO) << message.str();
            this->reportTiming = now;
        }
    }
private:
    std::chrono::steady_clock::time_point reportTiming;
    std::chrono::steady_clock::time_point begin;
    std::queue<long long> values;
    size_t limit;
    long long total;
    MediaType mediaType;
};

struct tx {
        struct module mod;

        uint32_t magic;
        enum tx_media_type media_type;
        unsigned mtu;
        double max_loss;

        uint32_t last_ts;
        int      last_frame_fragment_id;

        unsigned int buffer:22;
        unsigned long int sent_frames;
        int32_t avg_len;
        int32_t avg_len_last;

        enum fec_type fec_scheme;
        int mult_count;

        int last_fragment;

        struct control_state *control = nullptr;
        size_t sent_since_report = 0;
        uint64_t last_stat_report = 0;

        const struct openssl_encrypt_info *enc_funcs;
        struct openssl_encrypt *encryption;
        openssl_mode encryption_mode;
        std::vector<struct openssl_encrypt*> encryption_resources;
        std::unique_ptr<ThreadPool<struct openssl_encrypt*>> threadPool;
        unsigned int thread_count;
        PacketTiming videoTiming;
        PacketTiming audioTiming;

        uint64_t videoFrameTarget;
        
        long long int bitrate;
        struct rate_limit_dyn dyn_rate_limit_state;
		
        char tmp_packet[RTP_MAX_MTU];
};

struct PacketInfo {
    // The offset of the data to send from the original block
    size_t fragmentOffset;
    // The size of the payload header (used for encryption)
    size_t payloadHeaderSize;
    // Whether to attach and use the terminating M bit.
    bool sendM;
    // The timestamp to attach to the packet.
    uint32_t timestamp;
    // A pointer to the head of the total block of data being sent
    char* data;
    // The length of the total block of data being sent
    size_t dataLen;
    // The packet type
    unsigned short packetType;
};

void tx_send_packets(struct tx *tx, struct rtp *rtpSession, const std::vector<int>& packetSizes, const struct PacketInfo& packetInfo,
                     const uint32_t* rtpHeader, int rtpHeaderLen, int startingPacket, int packetAmount, bool packetPace, struct openssl_encrypt* encryption);


static void tx_update(struct tx *tx, struct video_frame *frame, int substream)
{
        if(!frame) {
                return;
        }
        
        uint64_t tmp_avg = tx->avg_len * tx->sent_frames + frame->tiles[substream].data_len *
                (frame->fec_params.type != FEC_NONE ?
                 (double) frame->fec_params.k / (frame->fec_params.k + frame->fec_params.m) :
                 1);
        tx->sent_frames++;
        tx->avg_len = tmp_avg / tx->sent_frames;
        if(tx->sent_frames >= 100) {
                if(tx->fec_scheme == FEC_LDGM && tx->max_loss > 0.0) {
                        if(abs(tx->avg_len_last - tx->avg_len) > tx->avg_len / 3) {
                                int data_len = tx->mtu -  (40 + (sizeof(fec_payload_hdr_t)));
                                data_len = (data_len / 48) * 48;
                                //void *fec_state_old = tx->fec_state;

                                struct msg_sender *msg = (struct msg_sender *)
                                        new_message(sizeof(struct msg_sender));
                                snprintf(msg->fec_cfg, sizeof(msg->fec_cfg), "LDGM percents %d %d %f",
                                                data_len, tx->avg_len, tx->max_loss);
                                msg->type = SENDER_MSG_CHANGE_FEC;
                                struct response *resp = send_message_to_receiver(get_parent_module(&tx->mod),
                                                (struct message *) msg);
                                free_response(resp);
                                tx->avg_len_last = tx->avg_len;
                        }
                }
                tx->avg_len = 0;
                tx->sent_frames = 0;
        }
}

ADD_TO_PARAM("encryption-mode", "* encryption-mode=<encryption mode number>\n"
             "  The encryption mode to use matching the internal enums (useful for backwards compatibility).\n"
             "    1. AES128-CTR\n"
             "    2. AES128-CFB\n"
             "    3. AES128-ECB\n"
             "    4. AES128-CBC\n"
             "    5. AES128-GCM (Default)\n");


ADD_TO_PARAM("encode-thread-count", "* encode-thread-count=<Thread Count>\n"
             "  The number of threads to use when encrypting packets.\n");

ADD_TO_PARAM("video-frame-target", "* video-frame-target=<Target in ms>\n"
             "  The target for sending out all packets (for shaping purposes) in ms (milliseconds)."
             " Set to zero to use 66% of the duration of the frame.");

struct tx *tx_init(struct module *parent, unsigned mtu, enum tx_media_type media_type,
                const char *fec, const char *encryption, long long int bitrate)
{
        if (mtu > RTP_MAX_MTU) {
                log_msg(LOG_LEVEL_ERROR, "Requested MTU exceeds maximal value allowed by RTP library (%d B).\n", RTP_MAX_MTU);
                return NULL;
        }

        if (bitrate < RATE_MIN) {
                log_msg(LOG_LEVEL_ERROR, "Invalid bitrate value %lld passed (either positive bitrate or magic values from %d supported)!\n", bitrate, RATE_MIN);
                return NULL;
        }

        struct tx *tx = (struct tx *) calloc(1, sizeof(struct tx));
        if (tx == nullptr) {
                return tx;
        }
        module_init_default(&tx->mod);
        tx->mod.cls = MODULE_CLASS_TX;
        tx->mod.priv_data = tx;
        tx->mod.deleter = tx_done;
        module_register(&tx->mod, parent);

        tx->magic = TRANSMIT_MAGIC;
        tx->media_type = media_type;
        tx->mult_count = 1;
        tx->max_loss = 0.0;
        tx->mtu = mtu;
        tx->buffer = lrand48() & 0x3fffff;
        tx->avg_len = tx->avg_len_last = tx->sent_frames = 0u;
        tx->fec_scheme = FEC_NONE;
        tx->last_frame_fragment_id = -1;
        if (fec) {
                if(!set_fec(tx, fec)) {
                        module_done(&tx->mod);
                        return NULL;
                }
        }

        // Have the thread count be configurable via a parameter. Otherwise, default to the
        // full amount of available threads.
        tx->thread_count = ThreadPool<int>::ThreadCount();
        const char* encodeThreadCount = get_commandline_param("encode-thread-count");
        if(encodeThreadCount) {
            int thread_count = std::stoi(encodeThreadCount);
            if(thread_count > 0) {
                tx->thread_count = static_cast<unsigned int>(thread_count);
                LOG(LOG_LEVEL_INFO) << "The encoder thread count has been set to: " << tx->thread_count << "\n";
            }
        }

        const char* videoFrameTarget = get_commandline_param("video-frame-target");
        if(videoFrameTarget) {
            uint64_t frameTarget = std::stoi(videoFrameTarget);
            // Convert the target to nanoseconds.
            tx->videoFrameTarget = frameTarget * 1000 * 1000;
        }
        else {
            // Otherwise set to a default (zero will use 66% of the duration of the frame)
            tx->videoFrameTarget = 0;
        }

        if (encryption) {
                tx->enc_funcs = static_cast<const struct openssl_encrypt_info *>(load_library("openssl_encrypt",
                                        LIBRARY_CLASS_UNDEFINED, OPENSSL_ENCRYPT_ABI_VERSION));
                if (!tx->enc_funcs) {
                        fprintf(stderr, "UltraGrid was build without OpenSSL support!\n");
                        module_done(&tx->mod);
                        return NULL;
                }

                tx->encryption_mode = DEFAULT_CIPHER_MODE;
                const char* modeRawParam = get_commandline_param("encryption-mode");
                if(modeRawParam) {
                    auto modeParam = std::string(modeRawParam);
                    try {
                        tx->encryption_mode = static_cast<openssl_mode>(std::stoi(modeParam));
                    }
                    catch(std::exception& ex) {
                        LOG(LOG_LEVEL_ERROR) << "Unable to initialise correct encryption mode: " << modeRawParam << " is not a recognised mode\n";
                        module_done(&tx->mod);
                        return NULL;
                    }
                }

                if (tx->enc_funcs->init(&tx->encryption, encryption, tx->encryption_mode) != 0) {
                    log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unable to initialize encryption\n");
                    module_done(&tx->mod);
                    return NULL;
                }
                log_msg(LOG_LEVEL_INFO, MOD_NAME "Encryption set to mode %d\n", (int) tx->encryption_mode);
                
                // Create an encryption structure per available thread.
                for(unsigned int i = 0; i < tx->thread_count; i++) {
                    // Hand in a pointer from the stack, this can then be copied into a vector
                    struct openssl_encrypt* encryption_resource = nullptr;
                    if (tx->enc_funcs->init(&encryption_resource, encryption, tx->encryption_mode) != 0) {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unable to initialize encryption\n");
                        module_done(&tx->mod);
                        return NULL;
                    }
                    // Copy the pointer into the vector
                    tx->encryption_resources.push_back(encryption_resource);
                }
        }
        else {
            // If there is no encryption then just handover nullptr's for the encryption resource
            unsigned int threadCount = ThreadPool<int>::ThreadCount();
            for(unsigned int i = 0; i < threadCount; i++) {
                tx->encryption_resources.push_back(nullptr);
            }
        }

        tx->bitrate = bitrate;

        tx->control = (struct control_state *) get_module(get_root_module(parent), "control");

        tx->audioTiming = PacketTiming(200, MediaType::MEDIA_AUDIO);
        tx->videoTiming = PacketTiming(200, MediaType::MEDIA_VIDEO);
        tx->threadPool = std::make_unique<ThreadPool<struct openssl_encrypt*>>(tx->encryption_resources, tx->thread_count);

        return tx;
}

static bool set_fec(struct tx *tx, const char *fec_const)
{
        char *fec = strdup(fec_const);
        bool ret = true;

        char *fec_cfg = NULL;
        if(strchr(fec, ':')) {
                char *delim = strchr(fec, ':');
                *delim = '\0';
                fec_cfg = delim + 1;
        }

        struct msg_sender *msg = (struct msg_sender *)
                new_message(sizeof(struct msg_sender));
        msg->type = SENDER_MSG_CHANGE_FEC;

        snprintf(msg->fec_cfg, sizeof(msg->fec_cfg), "flush");

        if (strcasecmp(fec, "none") == 0) {
                tx->fec_scheme = FEC_NONE;
        } else if(strcasecmp(fec, "mult") == 0) {
                tx->fec_scheme = FEC_MULT;
                assert(fec_cfg);
                tx->mult_count = (unsigned int) atoi(fec_cfg);
                assert(tx->mult_count <= FEC_MAX_MULT);
        } else if(strcasecmp(fec, "LDGM") == 0) {
                if(tx->media_type == TX_MEDIA_AUDIO) {
                        fprintf(stderr, "LDGM is not currently supported for audio!\n");
                        ret = false;
                } else {
                        if(!fec_cfg || (strlen(fec_cfg) > 0 && strchr(fec_cfg, '%') == NULL)) {
                                snprintf(msg->fec_cfg, sizeof(msg->fec_cfg), "LDGM cfg %s",
                                                fec_cfg ? fec_cfg : "");
                        } else { // delay creation until we have avarage frame size
                                tx->max_loss = atof(fec_cfg);
                        }
                        tx->fec_scheme = FEC_LDGM;
                }
        } else if(strcasecmp(fec, "RS") == 0) {
                snprintf(msg->fec_cfg, sizeof(msg->fec_cfg), "RS cfg %s",
                                fec_cfg ? fec_cfg : "");
                tx->fec_scheme = FEC_RS;
        } else if(strcasecmp(fec, "help") == 0) {
                std::cout << "Usage:\n"
                        "\t-f [A:|V:]{ mult:count | ldgm[:params] | rs[:params] }\n";
                ret = false;
        } else {
                fprintf(stderr, "Unknown FEC: %s\n", fec);
                ret = false;
        }

        if (ret) {
                struct response *resp = send_message_to_receiver(get_parent_module(&tx->mod),
                                (struct message *) msg);
                free_response(resp);
        } else {
                free_message((struct message *) msg, NULL);
        }

        free(fec);
        return ret;
}

static void fec_check_messages(struct tx *tx)
{
        struct message *msg;
        while ((msg = check_message(&tx->mod))) {
                auto *data = reinterpret_cast<struct msg_universal *>(msg);
                const char *text = data->text;
                if (strstr(text, MSG_UNIVERSAL_TAG_TX) != text) {
                        LOG(LOG_LEVEL_ERROR) << "[Transmit] Unexpected TX message: " << text << "\n";
                        free_message(msg, new_response(RESPONSE_BAD_REQUEST, "Unexpected message"));
                        continue;
                }
                text += strlen(MSG_UNIVERSAL_TAG_TX);
                struct response *r = nullptr;
                if (strstr(text, "fec ") == text) {
                        text += strlen("fec ");
                        if (set_fec(tx, text)) {
                                r = new_response(RESPONSE_OK, nullptr);
                                LOG(LOG_LEVEL_NOTICE) << "[Transmit] FEC set to new setting: " << text << "\n";
                        } else {
                                r = new_response(RESPONSE_INT_SERV_ERR, "cannot set FEC");
                                LOG(LOG_LEVEL_ERROR) << "[Transmit] Unable to reconfiure FEC to: " << text << "\n";
                        }
                } else if (strstr(text, "rate ") == text) {
                        text += strlen("rate ");
                        auto new_rate = unit_evaluate(text);
                        if (new_rate >= RATE_MIN) {
                                tx->bitrate = new_rate;
                                r = new_response(RESPONSE_OK, nullptr);
                                LOG(LOG_LEVEL_NOTICE) << "[Transmit] Bitrate set to: " << text << (new_rate > 0 ? "B" : "") << "\n";
                        } else {
                                r = new_response(RESPONSE_BAD_REQUEST, "Wrong value for bitrate");
                                LOG(LOG_LEVEL_ERROR) << "[Transmit] Wrong bitrate: " << text << "\n";
                        }
                } else {
                        r = new_response(RESPONSE_BAD_REQUEST, "Unknown TX message");
                        LOG(LOG_LEVEL_ERROR) << "[Transmit] Unknown TX message: " << text << "\n";
                }

                free_message(msg, r);
        }
}

static void tx_done(struct module *mod)
{
        struct tx *tx = (struct tx *) mod->priv_data;
        assert(tx->magic == TRANSMIT_MAGIC);

        // If the encryption is set then destroy it
        if(tx->encryption) {
            tx->enc_funcs->destroy(tx->encryption);
        }
        // If the items in the encryption resources are set then free them
        if(!tx->encryption_resources.empty()) {
            for(auto encryption_resource : tx->encryption_resources) {
                if(encryption_resource) {
                    tx->enc_funcs->destroy(encryption_resource);
                }
            }
        }

        free(tx);
}

/*
 * sends one or more frames (tiles) with same TS in one RTP stream. Only one m-bit is set.
 */
void
tx_send(struct tx *tx, struct video_frame *frame, struct rtp *rtp_session)
{
        unsigned int i;
        uint32_t ts = 0;

        assert(!frame->fragment || tx->fec_scheme == FEC_NONE); // currently no support for FEC with fragments
        assert(!frame->fragment || frame->tile_count); // multiple tile are not currently supported for fragmented send
        fec_check_messages(tx);

        ts = get_local_mediatime();
        if(frame->fragment &&
                        tx->last_frame_fragment_id == frame->frame_fragment_id) {
                ts = tx->last_ts;
        } else {
                tx->last_frame_fragment_id = frame->frame_fragment_id;
                tx->last_ts = ts;
        }

        for(i = 0; i < frame->tile_count; ++i)
        {
                int last = FALSE;
                int fragment_offset = 0;
                
                if (i == frame->tile_count - 1) {
                        if(!frame->fragment || frame->last_fragment)
                                last = TRUE;
                }
                if(frame->fragment)
                        fragment_offset = vf_get_tile(frame, i)->offset;

                tx_send_base(tx, frame, rtp_session, ts, last,
                                i, fragment_offset);
        }
        tx->buffer++;
}

/**
 * Format the video header into 6 32-bit words
 *
 * [ 10 bits for tile index ][      22 bits for buffer index       ]
 * [                    32 bits for data position                  ]
 * [               32 bits for overall tile data length            ]
 * [    16 bits for tile width    ][    16 bits for tile height    ]
 * [              32 bits for frame colour specification           ]
 * [                  32 bits for interlaced FPS row               ]
 *
 *
 * @param frame       The video frame being sent
 * @param tile_idx    The index of the tile being sent from within the video frame
 * @param buffer_idx  The buffer index
 * @param video_hdr   A pointer to the video header being written into
 */
void format_video_header(struct video_frame *frame, int tile_idx, int buffer_idx, uint32_t *video_hdr)
{
        uint32_t tmp;

        tmp = tile_idx << 22;
        tmp |= 0x3fffff & buffer_idx;
        video_hdr[0] = htonl(tmp);
        video_hdr[2] = htonl(frame->tiles[tile_idx].data_len);
        video_hdr[3] = htonl(frame->tiles[tile_idx].width << 16 | frame->tiles[tile_idx].height);
        video_hdr[4] = get_fourcc(frame->color_spec);

        /* word 6 */
        video_hdr[5] = format_interl_fps_hdr_row(frame->interlacing, frame->fps);
}

void format_audio_header(const audio_frame2 *frame, int channel, int buffer_idx, uint32_t *audio_hdr)
{
        uint32_t tmp = 0;
        tmp = channel << 22U; /* bits 0-9 */
        tmp |= buffer_idx; /* bits 10-31 */
        audio_hdr[0] = htonl(tmp);

        audio_hdr[2] = htonl(frame->get_data_len(channel));

        /* fourth word */
        tmp = (frame->get_bps() * 8) << 26U;
        tmp |= frame->get_sample_rate();
        audio_hdr[3] = htonl(tmp);

        /* fifth word */
        audio_hdr[4] = htonl(get_audio_tag(frame->get_codec()));
}

void
tx_send_tile(struct tx *tx, struct video_frame *frame, int pos, struct rtp *rtp_session)
{
        int last = FALSE;
        uint32_t ts = 0;
        int fragment_offset = 0;

        assert(!frame->fragment || tx->fec_scheme == FEC_NONE); // currently no support for FEC with fragments
        assert(!frame->fragment || frame->tile_count); // multiple tile are not currently supported for fragmented send
        fec_check_messages(tx);

        ts = get_local_mediatime();
        if(frame->fragment &&
                        tx->last_frame_fragment_id == frame->frame_fragment_id) {
                ts = tx->last_ts;
        } else {
                tx->last_frame_fragment_id = frame->frame_fragment_id;
                tx->last_ts = ts;
        }
        if(!frame->fragment || frame->last_fragment)
                last = TRUE;
        if(frame->fragment)
                fragment_offset = vf_get_tile(frame, pos)->offset;
        tx_send_base(tx, frame, rtp_session, ts, last, pos,
                        fragment_offset);
        tx->buffer ++;
}

static uint32_t format_interl_fps_hdr_row(enum interlacing_t interlacing, double input_fps)
{
        unsigned int fpsd, fd, fps, fi;
        uint32_t tmp;

        tmp = interlacing << 29;
        fps = round(input_fps);
        fpsd = 1; /// @todo make use of this value (for now it is always one)
        fd = 0;
        fi = 0;
        if (input_fps > 1.0 && fabs(input_fps - round(input_fps) / 1.001) < 0.005) { // 29.97 etc.
                fd = 1;
        } else if (fps < 1.0) {
                fps = round(1.0 / input_fps);
                fi = 1;
        }

        tmp |= fps << 19;
        tmp |= fpsd << 15;
        tmp |= fd << 14;
        tmp |= fi << 13;
        return htonl(tmp);
}

static inline void check_symbol_size(unsigned int fec_symbol_size, int payload_len)
{
        thread_local static bool status_printed = false;

        if (status_printed) {
                return;
        }

        if (fec_symbol_size > static_cast<unsigned int>(payload_len)) {
                LOG(LOG_LEVEL_WARNING) << "Warning: FEC symbol size exceeds payload size! "
                                "FEC symbol size: " << fec_symbol_size << "\n";
        } else {
                LOG(LOG_LEVEL_INFO) << "FEC symbol size: " << fec_symbol_size << ", symbols per packet: " <<
                                payload_len / fec_symbol_size << ", payload size: " <<
                                payload_len / fec_symbol_size * fec_symbol_size << "\n";
        }
        status_printed = true;
}

/**
 * Splits symbol (FEC symbol or uncompressed line) to 1 or more MTUs. Symbol starts
 * always on beginning of packet.
 *
 * If symbol_size is longer than MTU (more symbols fit one packet), the aligned
 * packet size is always the same.
 *
 * @param symbol_size  FEC symbol size or linesize for uncompressed
 */
static inline int get_video_pkt_len(int mtu,
                int symbol_size, int *symbol_offset)
{
        if (symbol_size > mtu) {
                if (symbol_size - *symbol_offset <= mtu) {
                        mtu = symbol_size - *symbol_offset;
                        *symbol_offset = 0;
                } else {
                        *symbol_offset += mtu;
                }
                return mtu;
        }
        return mtu / symbol_size * symbol_size;
}

/// @param mtu is tx->mtu - hdrs_len
static vector<int> get_packet_sizes(struct video_frame *frame, int substream, int mtu) {
        if (frame->fec_params.type != FEC_NONE) {
                check_symbol_size(frame->fec_params.symbol_size, mtu);
        }

        unsigned int symbol_size = 1;
        int symbol_offset = 0;
        if (frame->fec_params.type == FEC_NONE && !is_codec_opaque(frame->color_spec)) {
                symbol_size = vc_get_linesize(frame->tiles[substream].width, frame->color_spec);
                int pf_block_size = PIX_BLOCK_LCM / get_pf_block_pixels(frame->color_spec) * get_pf_block_bytes(frame->color_spec);
                assert(pf_block_size <= mtu);
                mtu = mtu / pf_block_size * pf_block_size;
        } else if (frame->fec_params.type != FEC_NONE) {
                symbol_size = frame->fec_params.symbol_size;
        }
        vector<int> ret;
        unsigned pos = 0;
        do {
                int len = symbol_size == 1
                        ? mtu
                        : get_video_pkt_len(mtu, symbol_size, &symbol_offset);
                pos += len;
                ret.push_back(len);
        } while (pos < frame->tiles[substream].data_len);
        return ret;
}

/**
 * Returns inter-packet interval in nanoseconds.
 */
__attribute__((unused)) static long get_packet_rate(struct tx *tx, struct video_frame *frame, int substream, long packet_count)
{
        if (tx->bitrate == RATE_UNLIMITED) {
                return 0;
        }
        double time_for_frame = 1.0 / frame->fps / frame->tile_count;
        double interval_between_pkts = time_for_frame / tx->mult_count / packet_count;
        // use only 75% of the time - we less likely overshot the frame time and
        // can minimize risk of swapping packets between 2 frames (out-of-order ones)
        interval_between_pkts = interval_between_pkts * 0.75;
        // prevent bitrate to be "too low", here 1 Mbps at minimum
        interval_between_pkts = std::min<double>(interval_between_pkts, tx->mtu / 1000'000.0);
        long packet_rate_auto = interval_between_pkts * 1000'000'000L;

        if (tx->bitrate == RATE_AUTO) { // adaptive (spread packets to 75% frame time)
               return packet_rate_auto;
        }
        if (tx->bitrate == RATE_DYNAMIC) {
                if (frame->tiles[substream].data_len > 2 * tx->dyn_rate_limit_state.avg_frame_size
                                && tx->dyn_rate_limit_state.last_excess > rate_limit_dyn::EXCESS_GAP) {
                        packet_rate_auto /= 2; // double packet rate for this frame
                        tx->dyn_rate_limit_state.last_excess = 0;
                } else {
                        tx->dyn_rate_limit_state.last_excess += 1;
                }
                tx->dyn_rate_limit_state.avg_frame_size = (9 * tx->dyn_rate_limit_state.avg_frame_size + frame->tiles[substream].data_len) / 10;
                return packet_rate_auto;
        }
        long long int bitrate = tx->bitrate & ~RATE_FLAG_FIXED_RATE;
        int avg_packet_size = frame->tiles[substream].data_len / packet_count;
        long packet_rate = 1000'000'000L * avg_packet_size * 8 / bitrate; // fixed rate
        if ((tx->bitrate & RATE_FLAG_FIXED_RATE) == 0) { // adaptive capped rate
                packet_rate = std::max(packet_rate, packet_rate_auto);
        }
        return packet_rate;
}

uint64_t calculateFrameTarget(struct tx* tx, struct video_frame* frame) {
    // If we're not limiting the rate, then return 0.
    switch(tx->bitrate) {
        case RATE_UNLIMITED: {
            return 0;
        }
        default: {
            // Calculate the number of nanoseconds required for this frame to play
            auto frameDuration = static_cast<uint64_t>(NANOSECOND / frame->fps);
            // The sending of packets occurs once for each tile, so the target timing for sending the "frame"
            // needs to be split per tile.
            uint64_t frameTarget = (frameDuration / frame->tile_count);
            // We cannot use the entire duration of the frame, as we need timing for other operations, but we
            // could use up to 66% of the duration of the frame (28ms for a 24fps frame).
            return (frameTarget / 3) * 2;
        }
    }
}

// static void tx_send_base(struct tx *tx, struct video_frame *frame, struct rtp *rtpSession,
//                          uint32_t ts, int send_m, unsigned int substream, int fragment_offset)
// {
//         if (!rtp_has_receiver(rtpSession)) {
//                 return;
//         }
//
//         // See definition in rtp_callback.h
//         uint32_t rtpHeader[100];
//         int rtpHeaderLen;
//
//         // A value specified in our packet format
//         unsigned short packetType = fec_pt_from_fec_type(TX_MEDIA_VIDEO,
//                                                          frame->fec_params.type,
//                                                          tx->encryption);
//
//         // Setup variables for calculating the timing for sending packets
//         // for the traffic shaper
// #ifdef HAVE_LINUX
//         struct timespec start, stop;
// #elif defined HAVE_MACOSX
//         struct timeval start, stop;
// #else // Windows
// 	LARGE_INTEGER start, stop, freq;
// #endif
//         long delta, overslept = 0;
//
//         // Fetch the tile of the frame we are operating on
//         struct tile* tile = &frame->tiles[substream];
//         int dataLen;
//
//         // Set up the variables for the multiple packet FEC
//         array <int, FEC_MAX_MULT> multPos{};
//         int multIndex = 0;
//
//         // Calculate the length of the header in bytes
//         int headersLen = (rtp_is_ipv6(rtpSession) ? 40 : 20) + 8 + 12; // IP hdr size + UDP hdr size + RTP hdr size
//
//         assert(tx->magic == TRANSMIT_MAGIC);
//
//         // Update the transmission statistics
//         tx_update(tx, frame, substream);
//
//         // Set up the headers for the out-going packets
//         if (frame->fec_params.type == FEC_NONE) {
//             // Expand the length of the headers by the size of a video payload header
//             headersLen += (sizeof(video_payload_hdr_t));
//             rtpHeaderLen = sizeof(video_payload_hdr_t);
//             // Create the video header - see func docs for header format
//             format_video_header(frame, substream, tx->buffer, rtpHeader);
//         } else {
//             // Expand the length of the headers by the size of a FEC payload header
//             headersLen += (sizeof(fec_payload_hdr_t));
//             rtpHeaderLen = sizeof(fec_payload_hdr_t);
//             // Setup the substream as a bit shifted 10 bit number
//             uint32_t tmp = substream << 22;
//             // Overlay the buffer into the other 22 available bits.
//             tmp |= 0x3fffff & tx->buffer;
//             rtpHeader[0] = htonl(tmp);
//             // Set the overall length of the tile data in the second section
//             rtpHeader[2] = htonl(tile->data_len);
//             // Set up the K parameter as a 13-bit number (bit-shifted)
//             // Set up the M parameter as a 13-bit number (bit-shifted)
//             // Overlay the C parameter (Max as a 6-bit value)
//             rtpHeader[3] = htonl(
//                              frame->fec_params.k << 19 |
//                              frame->fec_params.m << 6 |
//                              frame->fec_params.c);
//             // Insert the FEC seed
//             rtpHeader[4] = htonl(frame->fec_params.seed);
//         }
//
//         // If encryption is enabled expand the headers
//         if (tx->encryption) {
//             // Add the size of the crypto header and encryption overhead
//             headersLen += sizeof(crypto_payload_hdr_t) + tx->enc_funcs->get_overhead(tx->encryption);
//             // Add the default cipher mode to the first section of the crypto header (8-bit number)
//             rtpHeader[rtpHeaderLen / sizeof(uint32_t)] = htonl(DEFAULT_CIPHER_MODE << 24);
//             // Expand the size of the rtpHeader by the crypto header as well
//             rtpHeaderLen += sizeof(crypto_payload_hdr_t);
//         }
//
//         // Calculate the size of all of the out-going packets. This varies based off of MTU, and FEC settings.
//         // (Try and ensure FEC settings don't exceed the size of the MTU otherwise we'll get fragmented packets.
//         vector<int> packetSizes = get_packet_sizes(frame, substream, tx->mtu - headersLen);
//
//         // Calculate the amount of packets being sent. (Amount of packets * Multiplication FEC)
//         long packetCount = packetSizes.size() * (tx->fec_scheme == FEC_MULT ? tx->mult_count : 1);
//
//         // Calculate the rate at which we should be sending out packets
//         long packetRate = get_packet_rate(tx, frame, substream, packetCount);
//
//         // Create a header per packet
//         void *rtpHeaders = malloc(packetCount * rtpHeaderLen);
//         // Create a pointer for keeping track of the copy location for the header
//         auto rtpHeaderPacket = (uint32_t *) rtpHeaders;
//         // Look through the packets and duplicate the header into the RTP headers array
//         for (int i = 0; i < packetCount; ++i) {
//             memcpy(rtpHeaderPacket, rtpHeader, rtpHeaderLen);
//             rtpHeaderPacket += rtpHeaderLen / sizeof(uint32_t);
//         }
//         // Reset the pointer for keeping track of the header location to be the head
//         // of the array
//         rtpHeaderPacket = (uint32_t *) rtpHeaders;
//
//         if (!tx->encryption) {
//                 rtp_async_start(rtpSession, packetCount);
//         }
//
//         int packet_idx = 0;
//         unsigned pos = 0;
//         do {
//                 GET_STARTTIME;
//                 int m = 0;
//                 if(tx->fec_scheme == FEC_MULT) {
//                         pos = multPos[multIndex];
//                 }
//
//                 int offset = pos + fragment_offset;
//
//             rtpHeaderPacket[1] = htonl(offset);
//
//                 char *data = tile->data + pos;
//             dataLen = packetSizes.at(packet_idx);
//                 if (pos + dataLen >= (unsigned int) tile->data_len) {
//                         if (send_m) {
//                                 m = 1;
//                         }
//                     dataLen = tile->data_len - pos;
//                 }
//                 pos += dataLen;
//                 if(dataLen) { /* check needed for FEC_MULT */
//                         char encrypted_data[dataLen + MAX_CRYPTO_EXCEED];
//
//                         if (tx->encryption) {
//                             dataLen = tx->enc_funcs->encrypt(tx->encryption,
//                                                              data, dataLen,
//                                                              (char *) rtpHeaderPacket,
//                                                 frame->fec_params.type != FEC_NONE ? sizeof(fec_payload_hdr_t) :
//                                                 sizeof(video_payload_hdr_t),
//                                                              encrypted_data);
//                                 data = encrypted_data;
//                         }
//
//                         if (control_stats_enabled(tx->control)) {
//                                 auto current_time_ms = time_since_epoch_in_ms();
//                                 if(current_time_ms - tx->last_stat_report >= CONTROL_PORT_BANDWIDTH_REPORT_INTERVAL_MS){
//                                         std::ostringstream oss;
//                                         oss << "tx_send " << std::hex << rtp_my_ssrc(rtpSession) << std::dec << " video " << tx->sent_since_report;
//                                         control_report_stats(tx->control, oss.str());
//                                         tx->last_stat_report = current_time_ms;
//                                         tx->sent_since_report = 0;
//                                 }
//                                 tx->sent_since_report += dataLen + rtpHeaderLen;
//                         }
//
//                         rtp_send_data_hdr(rtpSession, ts, packetType, m, 0, 0,
//                                           (char *) rtpHeaderPacket, rtpHeaderLen,
//                                           data, dataLen, 0, 0, 0);
//                 }
//
//                 if (multIndex + 1 == tx->mult_count) {
//                         ++packet_idx;
//                 }
//
//                 if(tx->fec_scheme == FEC_MULT) {
//                     multPos[multIndex] = pos;
//                     multIndex = (multIndex + 1) % tx->mult_count;
//                 }
//
//             rtpHeaderPacket += rtpHeaderLen / sizeof(uint32_t);
//
//                 // TRAFFIC SHAPER
//                 if (pos < (unsigned int) tile->data_len) { // wait for all but last packet
//                         do {
//                                 GET_STOPTIME;
//                                 GET_DELTA;
//                         } while (packetRate - delta - overslept > 0);
//                         overslept = -(packetRate - delta - overslept);
//                         //fprintf(stdout, "%ld ", overslept);
//                 }
//         } while (pos < tile->data_len || multIndex != 0); // when multiplying, we need all streams go to the end
//
//         if (!tx->encryption) {
//                 rtp_async_wait(rtpSession);
//         }
//         free(rtpHeaders);
// }


static void tx_send_base(struct tx *tx, struct video_frame *frame, struct rtp *rtpSession,
                                  uint32_t ts, int send_m, unsigned int substream, int fragment_offset)
{
        if (!rtp_has_receiver(rtpSession)) {
                return;
        }

        // See definition in rtp_callback.h
        uint32_t rtpHeader[100];
        int rtpHeaderSize;

        // A value specified in our packet format
        unsigned short packetType = fec_pt_from_fec_type(TX_MEDIA_VIDEO,
                                                         frame->fec_params.type,
                                                         tx->encryption);

        // Fetch the tile of the frame we are operating on
        struct tile* tile = &frame->tiles[substream];
        int dataLen = 0;

        // Calculate the length of the header in bytes
        int headersLen = (rtp_is_ipv6(rtpSession) ? 40 : 20) + 8 + 12; // IP hdr size + UDP hdr size + RTP hdr size

        assert(tx->magic == TRANSMIT_MAGIC);

        // Update the transmission statistics
        tx_update(tx, frame, substream);

        // Set up the headers for the out-going packets
        if (frame->fec_params.type == FEC_NONE) {
            // Expand the length of the headers by the size of a video payload header
            headersLen += (sizeof(video_payload_hdr_t));
            rtpHeaderSize = sizeof(video_payload_hdr_t);
            // Create the video header - see func docs for header format
            format_video_header(frame, substream, tx->buffer, rtpHeader);
        } else {
            // Expand the length of the headers by the size of a FEC payload header
            headersLen += (sizeof(fec_payload_hdr_t));
            rtpHeaderSize = sizeof(fec_payload_hdr_t);
            // Setup the substream as a bit shifted 10 bit number
            uint32_t tmp = substream << 22;
            // Overlay the buffer into the other 22 available bits.
            tmp |= 0x3fffff & tx->buffer;
            rtpHeader[0] = htonl(tmp);
            // Set the overall length of the tile data in the second section
            rtpHeader[2] = htonl(tile->data_len);
            // Set up the K parameter as a 13-bit number (bit-shifted)
            // Set up the M parameter as a 13-bit number (bit-shifted)
            // Overlay the C parameter (Max as a 6-bit value)
            rtpHeader[3] = htonl(
                             frame->fec_params.k << 19 |
                             frame->fec_params.m << 6 |
                             frame->fec_params.c);
            // Insert the FEC seed
            rtpHeader[4] = htonl(frame->fec_params.seed);
        }

        // If encryption is enabled expand the headers
        if (tx->encryption) {
            // Add the size of the crypto header and encryption overhead
            headersLen += sizeof(crypto_payload_hdr_t) + tx->enc_funcs->get_overhead(tx->encryption);
            // Add the default cipher mode to the first section of the crypto header (8-bit number)
            rtpHeader[rtpHeaderSize / sizeof(uint32_t)] = htonl(tx->encryption_mode << 24);
            // Expand the size of the rtpHeader by the crypto header as well
            rtpHeaderSize += sizeof(crypto_payload_hdr_t);
        }

        // Calculate our packet timing. If zero, then it has not been set to a static value from
        // the command line.
        if(tx->videoFrameTarget == 0) {
            tx->videoFrameTarget = calculateFrameTarget(tx, frame);
        }

        // Calculate the size of all of the out-going packets. This varies based off of MTU, and FEC settings.
        // (Try and ensure FEC settings don't exceed the size of the MTU otherwise we'll get fragmented packets)
        vector<int> packetSizes = get_packet_sizes(frame, static_cast<int>(substream), tx->mtu - headersLen);
        // Calculate the amount of packets being sent. (Amount of packets * Multiplication FEC)
        long packetCount = packetSizes.size();

        if (!tx->encryption) {
                rtp_async_start(rtpSession, packetCount);
        }

        // Create the threadpool and initialise the workers.
        unsigned int threadCount = tx->thread_count;
        tx->threadPool->Start();

        // Create a list of bound functions to ensure they are executed last
        std::vector<std::function<void(struct openssl_encrypt*)>> finalPackets = std::vector<std::function<void(struct openssl_encrypt*)>>();

        // There is a lot of information required to send a packet. Wrap this into a structure to be passed for our blocks to share.
        struct PacketInfo packetInfo;
        packetInfo.data = (char*) tile->data;
        packetInfo.dataLen = tile->data_len;
        packetInfo.timestamp = ts;
        packetInfo.fragmentOffset = fragment_offset;
        packetInfo.packetType = packetType;
        packetInfo.payloadHeaderSize = frame->fec_params.type != FEC_NONE ? sizeof(fec_payload_hdr_t) : sizeof(video_payload_hdr_t);
        packetInfo.sendM = send_m;

        // Start the timer for recording the time it takes to send all of the packets.
        tx->videoTiming.start();

        // Calculate all blocks for every packet except the last one (because that needs to be sent last)
        const int blockPackets = static_cast<int>(packetCount) - 1;
        // Calculate the maximum block of packets we should send per thread.
        const int maxBlockSize = ceil((double)blockPackets / (double)threadCount);
        // Start from the beginning of the packet listing
        int blockBegin = 0;
        for(int i = 0; i < static_cast<int>(threadCount); i++) {
            // Set the block size to be the max, or the remaining amount of packets to send.
            int blockSize = maxBlockSize;
            if(blockBegin + blockSize > blockPackets) {
                blockSize = blockPackets - blockBegin;
            }

            // Use lambda to construct function to pass into thread
            std::function<void(struct openssl_encrypt*)> job = [tx, rtpSession, packetSizes, packetInfo,
                                                                rtpHeader, rtpHeaderSize, blockBegin, blockSize,
                                                                finalPackets]
            // Our thread pools allow per thread resources to be sent, so send encryption block
            (struct openssl_encrypt* encryption)
            {
                tx_send_packets(tx, rtpSession, packetSizes, packetInfo, rtpHeader, static_cast<int>(rtpHeaderSize / sizeof(uint32_t)), blockBegin, blockSize, true, encryption);
            };
            tx->threadPool->QueueJob(job);

            // Increment the beginning of the next block by the size we are sending
            blockBegin += blockSize;
        }

        // Update statistics for the control stats
        if (control_stats_enabled(tx->control)) {
            // Collect the time, and calculate if the correct period of time since the last
            // report has happened
            auto current_time_ms = time_since_epoch_in_ms();
            if(current_time_ms - tx->last_stat_report >= CONTROL_PORT_BANDWIDTH_REPORT_INTERVAL_MS){
                // Generate the report and send it to the control stats
                std::ostringstream oss;
                oss << "tx_send " << std::hex << rtp_my_ssrc(rtpSession) << std::dec << " video " << tx->sent_since_report;
                control_report_stats(tx->control, oss.str());

                // Reset the timing for the report to be now
                tx->last_stat_report = current_time_ms;
                tx->sent_since_report = 0;
            }
            // Update the amount of data thats been sent since the last report
            tx->sent_since_report += dataLen + rtpHeaderSize;
        }

        // Wait for the threads to finish taking jobs off of the queue
        while(tx->threadPool->Busy());
        // Join all remaining working threads as this ensures all packets are sent
        tx->threadPool->Stop();

        // Send the last packet
        tx_send_packets(tx, rtpSession, packetSizes, packetInfo, rtpHeader, rtpHeaderSize / sizeof(uint32_t), packetCount - 1, 1, false, tx->threadPool->threadResources.at(0));

        // Measure the time it has taken to send the video packets, and then, if enough time has passed,
        // report on it.
        tx->videoTiming.measure();
        tx->videoTiming.report(packetCount, tx->videoFrameTarget);

        if (!tx->encryption) {
                rtp_async_wait(rtpSession);
        }
}

void tx_send_packets(struct tx *tx, struct rtp *rtpSession, const std::vector<int>& packetSizes, const struct PacketInfo& packetInfo,
                     const uint32_t* rtpHeader, int rtpHeaderLen, int startingPacket, int packetAmount, bool packetPace, struct openssl_encrypt* encryption) {
    // Calculate the starting position this function will send packets from.
    size_t initialPos = 0;
    for(int i = 0; i < startingPacket; i++) {
        initialPos += packetSizes.at(i);
    }

    // If the video frame target is set to zero, then don't bother with the timing
    if(tx->videoFrameTarget == 0) {
        packetPace = false;
    }

    uint64_t packetDurationTarget = 0;
    // Calculate the timing for each packet - Nanoseconds
    if(packetAmount > 0) {
        packetDurationTarget = tx->videoFrameTarget / packetAmount;
    }

    // Set up the variables we will use to send the packets
    size_t pos = initialPos;
    int m = 0;
    // Calculate how many duplicates should be sent
    int multCount = tx->fec_scheme == FEC_MULT ? tx->mult_count : 1;

    // Get the timing from the beginning, so we can calculate if we're on target or not.
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    // Keep track of the total time spent pacing
    std::chrono::duration idleDuration = std::chrono::nanoseconds(0);

    // Loop for each packet we are sending
    for(int packetIndex = startingPacket; packetIndex < startingPacket + packetAmount; packetIndex++) {
        // Create a loop index to keep track of how many times this loop has occurred.
        int loopIndex = packetIndex - startingPacket;

        // Create a unique header block for the packet and copy the rtp header block
        std::unique_ptr<uint32_t[]> rtpHeaderPacket = std::make_unique<uint32_t[]>(rtpHeaderLen);
        std::copy(rtpHeader, rtpHeader + rtpHeaderLen, rtpHeaderPacket.get());

        // Calculate and add to the rtpHeaderPacket the offset of this data
        // from the original pointer.
        size_t dataOffset = pos + packetInfo.fragmentOffset;
        rtpHeaderPacket[1] = htonl(dataOffset);

        // Create a pointer to the data that marks the head of the data we
        // are sending in the packet. Get the size of the packet from the
        // packet sizes array.
        char *data = packetInfo.data + pos;
        size_t dataLen = packetSizes.at(packetIndex);
        // Take a copy of the data length because encryption can change the length of the data
        size_t originalDataLen = dataLen;

        // If the data we are sending is the last block, then adjust the length
        // of the data we're sending to match what is available, and set the M
        // bit to be 1 marking it as the final packet of the frame.
        if (pos + dataLen >= packetInfo.dataLen) {
                dataLen = packetInfo.dataLen - pos;
                if (packetInfo.sendM) {
                        m = 1;
                }
        }

        // Check there is some data to send
        if(dataLen) {
            // Allocate on the stack for encrypted data
            char encrypted_data[dataLen + MAX_CRYPTO_EXCEED];
            if (tx->encryption) {            
                // Encrypt the data and update our variable for the data length, and the data being sent
                dataLen = tx->enc_funcs->encrypt(encryption,
                                                 data, dataLen,
                                                 (char *) rtpHeaderPacket.get(),
                                                 packetInfo.payloadHeaderSize,
                                                 encrypted_data);
                data = encrypted_data;
            }

            // Send the packets the multiplicative amount, if FEC Mult is being used
            for(int i = 0; i < multCount; i++) {
                // Send the packet
                rtp_send_data_hdr(rtpSession, packetInfo.timestamp, packetInfo.packetType, m, 0, 0,
                                  (char *) rtpHeaderPacket.get(), rtpHeaderLen * sizeof(uint32_t),
                                  data, dataLen, 0, 0, 0);
            }
        }

        // If packet pacing is enabled, then we should compare the actual
        // performance against our expected performance. If we are ahead
        // we should wait until we're at our expected timing.
        if(packetPace) {
            // Get the time
            std::chrono::steady_clock::time_point sentTimePoint = std::chrono::steady_clock::now();
            // Calculate where we should be according to our target
            std::chrono::duration targetDuration = std::chrono::nanoseconds(packetDurationTarget) * (loopIndex + 1);
            std::chrono::steady_clock::time_point target = start + targetDuration;
            // Keep track of if we had to wait at all
            bool waiting = false;
            // Loop until we're on target
            while(std::chrono::steady_clock::now() < target) {
                waiting = true;
            }
            // Add the duration spent waiting to the idle timer
            if(waiting) {
                idleDuration += (std::chrono::steady_clock::now() - sentTimePoint);
            }
        }

        // Push forward the position by the amount of data we just sent
        pos += originalDataLen;
    }

    // If we spent any time waiting then log the total time spent idling to pace the packets
    if(idleDuration != std::chrono::nanoseconds(0)) {
        LOG(LOG_LEVEL_DEBUG) << MOD_NAME
                             << "Total idle time: "
                             << std::chrono::duration_cast<std::chrono::nanoseconds>(idleDuration).count()
                             << "\n";
    }
}

/* 
 * This multiplication scheme relies upon the fact, that our RTP/pbuf implementation is
 * not sensitive to packet duplication. Otherwise, we can get into serious problems.
 */
void audio_tx_send(struct tx* tx, struct rtp *rtp_session, const audio_frame2 * buffer)
{
        if (!rtp_has_receiver(rtp_session)) {
                return;
        }

        int pt = fec_pt_from_fec_type(TX_MEDIA_AUDIO, buffer->get_fec_params(0).type, tx->encryption); /* PT set for audio in our packet format */
        unsigned m = 0u;
        const char *chan_data;
        // see definition in rtp_callback.h
        uint32_t rtp_hdr[100];
        uint32_t timestamp;
#ifdef HAVE_LINUX
        struct timespec start, stop;
#elif defined HAVE_MACOSX
        struct timeval start, stop;
#else // Windows
	LARGE_INTEGER start, stop, freq;
#endif
        long delta;
        int mult_first_sent = 0;

        fec_check_messages(tx);

        timestamp = get_local_mediatime();

        // Start the recording of the amount of time it takes to send the audio channels
        tx->audioTiming.start();
        size_t packetCount = 0;

        for (int channel = 0; channel < buffer->get_channel_count(); ++channel)
        {
                int rtp_hdr_len = 0;
                int hdrs_len = (rtp_is_ipv6(rtp_session) ? 40 : 20) + 8 + 12; // MTU - IP hdr - UDP hdr - RTP hdr - payload_hdr
                unsigned int fec_symbol_size = buffer->get_fec_params(channel).symbol_size;
                unsigned int fecMultFactor = buffer->get_fec_params(channel).mult;

                chan_data = buffer->get_data(channel);
                unsigned pos = 0u;

                array <int, FEC_MAX_MULT> mult_pos{};
                int mult_index = 0;

                if (buffer->get_fec_params(0).type == FEC_NONE) {
                        hdrs_len += (sizeof(audio_payload_hdr_t));
                        rtp_hdr_len = sizeof(audio_payload_hdr_t);
                        format_audio_header(buffer, channel, tx->buffer, rtp_hdr);
                } else if(buffer->get_fec_params(0).type != FEC_RS) {
                        hdrs_len += (sizeof(fec_payload_hdr_t));
                        rtp_hdr_len = sizeof(fec_payload_hdr_t);
                        uint32_t tmp = channel << 22;
                        tmp |= 0x3fffff & tx->buffer;
                        // see definition in rtp_callback.h
                        rtp_hdr[0] = htonl(tmp);
                        rtp_hdr[2] = htonl(buffer->get_data_len(channel));
                        rtp_hdr[3] = htonl(
                                        buffer->get_fec_params(channel).k << 19 |
                                        buffer->get_fec_params(channel).m << 6 |
                                        buffer->get_fec_params(channel).c);
                        rtp_hdr[4] = htonl(buffer->get_fec_params(channel).seed);
                }
                // Setup the header for Reed Solomon
                else {
                    hdrs_len += (sizeof(fec_payload_hdr_t));
                    rtp_hdr_len = sizeof(fec_payload_hdr_t);
                    uint32_t tmp = channel << 22;
                    tmp |= 0x3fffff & tx->buffer;
                    // see definition in rtp_callback.h
                    rtp_hdr[0] = htonl(tmp);
                    rtp_hdr[2] = htonl(buffer->get_data_len(channel));
                    rtp_hdr[3] = htonl(
                            // Both k & m are limited to 256 in the existing implementation
                            buffer->get_fec_params(channel).k << 24 |
                            buffer->get_fec_params(channel).m << 16 |
                            // Knowing the symbol size when it arrives is very important
                            // as it will help with splitting up the data appropriately. 12 bits
                            // allows for a symbol size up to the same size as a UDP packet (4096).
                            buffer->get_fec_params(channel).symbol_size << 4 |
                            // If every FEC packet knows the channel count, then receiving the M-bit
                            // packet is not crucial to the entire frame being processed.
                            (buffer->get_channel_count() - 1));
                    rtp_hdr[4] = htonl(buffer->get_fec_params(channel).seed);
                }

                if (tx->encryption) {
                        hdrs_len += sizeof(crypto_payload_hdr_t) + tx->enc_funcs->get_overhead(tx->encryption);
                        rtp_hdr[rtp_hdr_len / sizeof(uint32_t)] = htonl(DEFAULT_CIPHER_MODE << 24);
                        rtp_hdr_len += sizeof(crypto_payload_hdr_t);
                }

                if (buffer->get_fec_params(0).type != FEC_NONE) {
                        check_symbol_size(fec_symbol_size, tx->mtu - hdrs_len);
                }

                int packet_rate = 0;
#if 0
                if (tx->bitrate > 0) {
                        //packet_rate = 1000ll * 1000 * 1000 * tx->mtu * 8 / tx->bitrate;
			packet_rate = 0;
                } else if (tx->bitrate == RATE_UNLIMITED) {
                        packet_rate = 0;
                } else if (tx->bitrate == RATE_AUTO || tx->bitrate == RATE_DYNAMIC) {
                        /**
                         * @todo
                         * Following code would actually work but seems to be useless in most of cases (eg.
                         * PCM 2 channels 2 Bps takes 5 std. Eth frames). On the other hand it could cause
                         * unexpectable problems (I'd write them here but if I'd expect them they wouldn't
                         * be unexpectable.)
                         */
                        double time_for_frame = buffer->get_duration() / buffer->get_channel_count();
                        if (time_for_frame > 0.0) {
                                long long req_bitrate = buffer->get_data_len(channel) * 8 / time_for_frame * tx->mult_count;
                                // adjust computed value to 3
                                req_bitrate = req_bitrate * 3;
                                packet_rate = compute_packet_rate(req_bitrate, tx->mtu);
                        } else {
                                packet_rate = 0;
                        }
                        packet_rate = 0;
                } else {
                        abort();
                }
#endif

                do {
                        int data_len = 0;
                        if(tx->fec_scheme == FEC_MULT) {
                                pos = mult_pos[mult_index];
                        }

                        // If we are using Reed Sollomon then we want to ensure that the packets we
                        // are sending are a multiple of the size of the FEC symbols. This introduces
                        // the risk that each packet being sent IS NOT within the MTU.
                        if(tx->fec_scheme == FEC_RS) {
                            data_len = fec_symbol_size * fecMultFactor;
                        }
                        else {
                            data_len = tx->mtu - hdrs_len;
                        }

                        const char *data = chan_data + pos;
                        if(pos + data_len >= (unsigned int) buffer->get_data_len(channel)) {
                                data_len = buffer->get_data_len(channel) - pos;
                                if(channel == buffer->get_channel_count() - 1)
                                        m = 1;
                        }
                        rtp_hdr[1] = htonl(pos);
                        pos += data_len;
                        
                        GET_STARTTIME;
                        
                        if(data_len) { /* check needed for FEC_MULT */
                                char encrypted_data[data_len + MAX_CRYPTO_EXCEED];
                                if(tx->encryption) {
                                        data_len = tx->enc_funcs->encrypt(tx->encryption,
                                                        const_cast<char *>(data), data_len,
                                                        (char *) rtp_hdr, rtp_hdr_len - sizeof(crypto_payload_hdr_t),
                                                        encrypted_data);
                                        data = encrypted_data;
                                }

                                if (control_stats_enabled(tx->control)) {
                                        auto current_time_ms = time_since_epoch_in_ms();
                                        if(current_time_ms - tx->last_stat_report >= CONTROL_PORT_BANDWIDTH_REPORT_INTERVAL_MS){
                                                std::ostringstream oss;
                                                oss << "tx_send " << std::hex << rtp_my_ssrc(rtp_session) << std::dec << " audio " << tx->sent_since_report;
                                                control_report_stats(tx->control, oss.str());
                                                tx->last_stat_report = current_time_ms;
                                                tx->sent_since_report = 0;
                                        }
                                        tx->sent_since_report += data_len + rtp_hdr_len;
                                }


                                rtp_send_data_hdr(rtp_session, timestamp, pt, m, 0,        /* contributing sources */
                                      0,        /* contributing sources length */
                                      (char *) rtp_hdr, rtp_hdr_len,
                                      const_cast<char *>(data), data_len,
                                      0, 0, 0);
                                // Increment the sent packets
                                packetCount++;

                                // If the expectation is that this is being used in a lossy network it is important
                                // that this packet arrive, so send it twice.
                                if(tx->fec_scheme == FEC_RS && m == 1) {
                                    rtp_send_data_hdr(rtp_session, timestamp, pt, m, 0,        /* contributing sources */
                                                      0,        /* contributing sources length */
                                                      (char *) rtp_hdr, rtp_hdr_len,
                                                      const_cast<char *>(data), data_len,
                                                      0, 0, 0);
                                }
                        }

                        if(tx->fec_scheme == FEC_MULT) {
                                mult_pos[mult_index] = pos;
                                mult_first_sent ++;
                                if(mult_index != 0 || mult_first_sent >= (tx->mult_count - 1))
                                                mult_index = (mult_index + 1) % tx->mult_count;
                        }

                        if (pos < buffer->get_data_len(channel)) {
                                do {
                                        GET_STOPTIME;
                                        GET_DELTA;
                                        if (delta < 0)
                                                delta += 1000000000L;
                                } while (packet_rate - delta > 0);
                        }

                        /* when trippling, we need all streams goes to end */
                        if(tx->fec_scheme == FEC_MULT) {
                                pos = mult_pos[tx->mult_count - 1];
                        }

                      
                } while (pos < buffer->get_data_len(channel));
        }

        // Measure the time it has taken to send the audio packets, and then, if enough time has passed,
        // report on it.
        tx->audioTiming.measure();
        tx->audioTiming.report(packetCount);

        tx->buffer++;
}

/**
 * @brief A helper function for sending a block of segments from an audio buffer. This is useful when the transmission
 *        should prioritise sending some of the packets first over others.
 *
 * @param tx             The transmission structure
 * @param rtp_session    The RTP session
 * @param buffer         The audio frame that is being sent
 * @param channel        The channel number that is being sent from the buffer
 * @param segmentOffset  The number of segments offset from the beginning of the channel data
 * @param segmentCount   The number of segments to send
 * @param pt             The pt
 * @param timestamp      The timestamp for sending out the packets
 */
void audio_tx_send_channel_segments(struct tx* tx, struct rtp *rtp_session, const audio_frame2 * buffer, int channel, size_t segmentOffset, uint32_t segmentCount, int pt, uint32_t timestamp) {
    unsigned m = 0u;
#ifdef HAVE_LINUX
    struct timespec start, stop;
#elif defined HAVE_MACOSX
    struct timeval start, stop;
#else // Windows
    LARGE_INTEGER start, stop, freq;
#endif
    long delta;
    int rtp_hdr_len = 0;
    int hdrs_len = (rtp_is_ipv6(rtp_session) ? 40 : 20) + 8 + 12; // MTU - IP hdr - UDP hdr - RTP hdr - payload_hdr
    unsigned int fec_symbol_size = buffer->get_fec_params(channel).symbol_size;
    unsigned int fecMultFactor = buffer->get_fec_params(channel).mult;

    const char* chan_data = buffer->get_data(channel);
    uint32_t rtp_hdr[100];
    unsigned int pos = segmentOffset * fec_symbol_size;

    hdrs_len += (sizeof(fec_payload_hdr_t));
    rtp_hdr_len = sizeof(fec_payload_hdr_t);
    uint32_t tmp = channel << 22;
    tmp |= 0x3fffff & tx->buffer;
    // see definition in rtp_callback.h
    rtp_hdr[0] = htonl(tmp);
    rtp_hdr[2] = htonl(buffer->get_data_len(channel));
    rtp_hdr[3] = htonl(
    // Both k & m are limited to 256 in the existing implementation
            buffer->get_fec_params(channel).k << 24 |
            buffer->get_fec_params(channel).m << 16 |
            // Knowing the symbol size when it arrives is very important
            // as it will help with splitting up the data appropriately. 12 bits
            // allows for a symbol size up to the same size as a UDP packet (4096).
            buffer->get_fec_params(channel).symbol_size << 4 |
            // If every FEC packet knows the channel count, then receiving the M-bit
            // packet is not crucial to the entire frame being processed.
            (buffer->get_channel_count() - 1));
    rtp_hdr[4] = htonl(buffer->get_fec_params(channel).seed);

    if (tx->encryption) {
        hdrs_len += sizeof(crypto_payload_hdr_t) + tx->enc_funcs->get_overhead(tx->encryption);
        rtp_hdr[rtp_hdr_len / sizeof(uint32_t)] = htonl(DEFAULT_CIPHER_MODE << 24);
        rtp_hdr_len += sizeof(crypto_payload_hdr_t);
    }

    check_symbol_size(fec_symbol_size, tx->mtu - hdrs_len);

    int packet_rate = 0;
    // This has been temporarily removed elsewhere, so it needs to happen here as well
#if 0
    if (tx->bitrate > 0 || tx->bitrate == RATE_UNLIMITED || tx->bitrate == RATE_AUTO) {
        packet_rate = 0;
    }
    else {
        abort();
    }
#endif

    do {
        int data_len = 0;
        data_len = fec_symbol_size * fecMultFactor;
        segmentCount -= fecMultFactor;

        const char *data = chan_data + pos;
        if(pos + data_len >= (unsigned int) buffer->get_data_len(channel)) {
            data_len = buffer->get_data_len(channel) - pos;
            if(channel == buffer->get_channel_count() - 1)
                m = 1;
        }
        rtp_hdr[1] = htonl(pos);
        pos += data_len;

        GET_STARTTIME;

        if(data_len) { /* check needed for FEC_MULT */
            char encrypted_data[data_len + MAX_CRYPTO_EXCEED];
            if(tx->encryption) {
                data_len = tx->enc_funcs->encrypt(tx->encryption,
                                                  const_cast<char *>(data), data_len,
                                                  (char *) rtp_hdr, rtp_hdr_len - sizeof(crypto_payload_hdr_t),
                                                  encrypted_data);
                data = encrypted_data;
            }

            rtp_send_data_hdr(rtp_session, timestamp, pt, m, 0,        /* contributing sources */
                              0,        /* contributing sources length */
                              (char *) rtp_hdr, rtp_hdr_len,
                              const_cast<char *>(data), data_len,
                              0, 0, 0);
            // If the expectation is that this is being used in a lossy network it is important
            // that this packet arrive, so send it twice.
            if(m == 1) {
                rtp_send_data_hdr(rtp_session, timestamp, pt, m, 0,        /* contributing sources */
                                  0,        /* contributing sources length */
                                  (char *) rtp_hdr, rtp_hdr_len,
                                  const_cast<char *>(data), data_len,
                                  0, 0, 0);
            }

            if (pos < buffer->get_data_len(channel)) {
                do {
                    GET_STOPTIME;
                    GET_DELTA;
                    if (delta < 0)
                        delta += 1000000000L;
                } while (packet_rate - delta > 0);
            }
        }
    } while (pos < buffer->get_data_len(channel) && segmentCount > 0);
}

/**
 * @brief When using Reed-Solomon FEC it is best to send all of the audio data segments, followed by the parity segments.
 *        This means that all of the audio data will be received first, which if there are late packets gives the audio
 *        frame the best chance of being fully played back.
 *
 * @param tx           The transmission structure
 * @param rtp_session  The rtp session
 * @param buffer       The audio frame to be sent
 */
void audio_fec_tx_send(struct tx* tx, struct rtp *rtp_session, const audio_frame2 * buffer)
{
    if (!rtp_has_receiver(rtp_session)) {
        return;
    }

    int pt = fec_pt_from_fec_type(TX_MEDIA_AUDIO, buffer->get_fec_params(0).type, tx->encryption); /* PT set for audio in our packet format */
    uint32_t timestamp;
    fec_check_messages(tx);

    timestamp = get_local_mediatime();

    unsigned int fecMultFactor = buffer->get_fec_params(0).mult;
    unsigned int fecKSize = buffer->get_fec_params(0).k;
    unsigned int fecMSize = buffer->get_fec_params(0).m + fecKSize;

    // Calculate the total amount of packets that will be sent
    unsigned int totalPackets = ceil(fecMSize / fecMultFactor);
    // Calculate the total amount of packets that contain the audio data
    unsigned int audioSegmentPackets = ceil(fecKSize / fecMultFactor);
    // Calculate the amount of audio segments that need to be sent as a factor of the multiplication
    // that is applied when RS FEC is in use
    unsigned int audioSegmentCount = audioSegmentPackets * fecMultFactor;
    // Calculate the remaining amount of parity segments that need to be sent (as a factor of the multiplication)
    unsigned int paritySegmentCount = (totalPackets - audioSegmentPackets) * fecMultFactor;

    // Start the recording of the amount of time it takes to send the audio channels
    tx->audioTiming.start();

    // Start by sending all of the segments containing the audio data
    for (int channel = 0; channel < buffer->get_channel_count(); channel++)
    {
        audio_tx_send_channel_segments(tx, rtp_session, buffer, channel, 0, audioSegmentCount, pt, timestamp);
    }
    // Loop over the parity bits and send the parity segments channel by channel
    for(unsigned int i = 0; i < paritySegmentCount; i += fecMultFactor) {
        // Follow up with the parity segments on the end of each channel
        for (int channel = 0; channel < buffer->get_channel_count(); channel++)
        {
            audio_tx_send_channel_segments(tx, rtp_session, buffer, channel, audioSegmentCount + i, fecMultFactor, pt, timestamp);
        }
    }

    // Measure the time it has taken to send the video packets, and then, if enough time has passed,
    // report on it.
    tx->audioTiming.measure();
    tx->audioTiming.report(static_cast<size_t>(totalPackets));

    tx->buffer++;
}

/**
 * audio_tx_send_standard - Send interleaved channels from the audio_frame2,
 *                       	as the mulaw and A-law standards (dynamic or std PT).
 */
void audio_tx_send_standard(struct tx* tx, struct rtp *rtp_session,
		const audio_frame2 * buffer) {
	//TODO to be more abstract in order to accept A-law too and other supported standards with such implementation
	assert(buffer->get_codec() == AC_MULAW || buffer->get_codec() == AC_ALAW || buffer->get_codec() == AC_OPUS);

	int pt;
	uint32_t ts;
	static uint32_t ts_prev = 0;

	// Configure the right Payload type,
	// 8000 Hz, 1 channel and 2 bps is the ITU-T G.711 standard (should be 1 bps...)
	// Other channels or Hz goes to DynRTP-Type97
	if (buffer->get_channel_count() == 1 && buffer->get_sample_rate() == 8000) {
		if (buffer->get_codec() == AC_MULAW)
			pt = PT_ITU_T_G711_PCMU;
		else if (buffer->get_codec() == AC_ALAW)
			pt = PT_ITU_T_G711_PCMA;
		else pt = PT_DynRTP_Type97;
	} else {
		pt = PT_DynRTP_Type97;
	}

	// The sizes for the different audio_frame2 channels must be the same.
	for (int i = 1; i < buffer->get_channel_count(); i++)
		assert(buffer->get_data_len(0) == buffer->get_data_len(i));

	int data_len = buffer->get_data_len(0) * buffer->get_channel_count(); 	/* Number of samples to send 			*/
	int payload_size = tx->mtu - 40 - 8 - 12; /* Max size of an RTP payload field (minus IPv6, UDP and RTP header lengths) */

        if (buffer->get_codec() == AC_OPUS) { // OPUS needs to fit one package
                if (payload_size < data_len) {
                        log_msg(LOG_LEVEL_ERROR, "Transmit: OPUS frame larger than packet! Discarding...\n");
                        return;
                }
        } else { // we may split the data into more packets, compute chunk size
                int frame_size = buffer->get_channel_count() * buffer->get_bps();
                payload_size = payload_size / frame_size * frame_size; // align to frame size
        }

	int pos = 0;
	do {
                int pkt_len = std::min(payload_size, data_len - pos);

                // interleave
                if (buffer->get_codec() == AC_OPUS) {
                        if (buffer->get_channel_count() > 1) { // we cannot interleave OPUS here
                                LOG(LOG_LEVEL_ERROR) << "Transmit: Only OPUS with 1 channel is supported in RFC-compliant mode! Discarding...\n";
                                return;
                        }
                        memcpy(tx->tmp_packet, buffer->get_data(0), pkt_len);
                } else {
                        for (int ch = 0; ch < buffer->get_channel_count(); ch++) {
                                remux_channel(tx->tmp_packet, buffer->get_data(ch) + pos / buffer->get_channel_count(), buffer->get_bps(), pkt_len / buffer->get_channel_count(), 1, buffer->get_channel_count(), 0, ch);
                        }
                }

                // Update first sample timestamp
                if (buffer->get_codec() == AC_OPUS) {
                        /* OPUS packet will be the whole contained in one packet
                         * according to RFC 7587. For PCMA/PCMU there may be more
                         * packets so we cannot use the whole frame duration. */
                        ts = get_std_audio_local_mediatime(buffer->get_duration(), 48000);
                } else {
                        ts = get_std_audio_local_mediatime((double) pkt_len / (double) buffer->get_channel_count() / (double) buffer->get_sample_rate(), buffer->get_sample_rate());
                }
                rtp_send_ctrl(rtp_session, ts_prev, 0, get_time_in_ns()); //send RTCP SR
                ts_prev = ts;
                // Send the packet
                rtp_send_data(rtp_session, ts, pt, 0, 0, /* contributing sources 		*/
                                0, 												/* contributing sources length 	*/
                                tx->tmp_packet, pkt_len, 0, 0, 0);
                pos += pkt_len;
	} while (pos < data_len);
}

/**
 *  H.264 standard transmission
 */
void tx_send_h264(struct tx *tx, struct video_frame *frame,
		struct rtp *rtp_session) {
        assert(frame->tile_count == 1); // std transmit doesn't handle more than one tile
        assert(!frame->fragment || tx->fec_scheme == FEC_NONE); // currently no support for FEC with fragments
        assert(!frame->fragment || frame->tile_count); // multiple tiles are not currently supported for fragmented send
        uint32_t ts = get_std_video_local_mediatime();
        struct tile *tile = &frame->tiles[0];

	char pt =  PT_DynRTP_Type96;
	unsigned char hdr[2];
	int cc = 0;
	uint32_t csrc = 0;
	int m = 0;
	char *extn = 0;
	uint16_t extn_len = 0;
	uint16_t extn_type = 0;
	const uint8_t *start = (uint8_t *) tile->data;
	int data_len = tile->data_len;
	unsigned maxPacketSize = tx->mtu - 40;

        const unsigned char *endptr = 0;
        const unsigned char *nal = start;

        while ((nal = rtpenc_h264_get_next_nal(nal, data_len - (nal - start), &endptr))) {
                unsigned int nalsize = endptr - nal;
                bool eof = endptr == start + data_len;
                bool lastNALUnitFragment = false; // by default
                unsigned curNALOffset = 0;
                char *nalc = const_cast<char *>(reinterpret_cast<const char *>(nal));

		while(!lastNALUnitFragment){
			// We have NAL unit data in the buffer.  There are three cases to consider:
			// 1. There is a new NAL unit in the buffer, and it's small enough to deliver
			//    to the RTP sink (as is).
			// 2. There is a new NAL unit in the buffer, but it's too large to deliver to
			//    the RTP sink in its entirety.  Deliver the first fragment of this data,
			//    as a FU packet, with one extra preceding header byte (for the "FU header").
			// 3. There is a NAL unit in the buffer, and we've already delivered some
			//    fragment(s) of this.  Deliver the next fragment of this data,
			//    as a FU packet, with two (H.264) extra preceding header bytes
			//    (for the "NAL header" and the "FU header").
			if (curNALOffset == 0) { // case 1 or 2
				if (nalsize	<= maxPacketSize) { // case 1

					if (eof) m = 1;
					if (rtp_send_data(rtp_session, ts, pt, m, cc, &csrc,
							nalc, nalsize,
							extn, extn_len, extn_type) < 0) {
						error_msg("There was a problem sending the RTP packet\n");
					}
					lastNALUnitFragment = true;
				} else { // case 2
					// We need to send the NAL unit data as FU packets.  Deliver the first
					// packet now.  Note that we add "NAL header" and "FU header" bytes to the front
					// of the packet (overwriting the existing "NAL header").
					hdr[0] = (nal[0] & 0xE0) | 28; //FU indicator
					hdr[1] = 0x80 | (nal[0] & 0x1F); // FU header (with S bit)

					if (rtp_send_data_hdr(rtp_session, ts, pt, m, cc, &csrc,
									(char *) hdr, 2,
									nalc + 1, maxPacketSize - 2,
									extn, extn_len, extn_type) < 0) {
										error_msg("There was a problem sending the RTP packet\n");
					}
					curNALOffset += maxPacketSize - 1;
					lastNALUnitFragment = false;
					nalsize -= maxPacketSize - 1;
				}
			} else { // case 3
				// We are sending this NAL unit data as FU packets.  We've already sent the
				// first packet (fragment).  Now, send the next fragment.  Note that we add
				// "NAL header" and "FU header" bytes to the front.  (We reuse these bytes that
				// we already sent for the first fragment, but clear the S bit, and add the E
				// bit if this is the last fragment.)
				hdr[1] = hdr[1] & ~0x80;// FU header (no S bit)

				if (nalsize + 1 > maxPacketSize) {
					// We can't send all of the remaining data this time:
					if (rtp_send_data_hdr(rtp_session, ts, pt, m, cc, &csrc,
							(char *) hdr, 2,
							nalc + curNALOffset,
							maxPacketSize - 2, extn, extn_len,
							extn_type) < 0) {
								error_msg("There was a problem sending the RTP packet\n");
					}
					curNALOffset += maxPacketSize - 2;
					lastNALUnitFragment = false;
					nalsize -= maxPacketSize - 2;

				} else {
					// This is the last fragment:
					if (eof) m = 1;

					hdr[1] |= 0x40;// set the E bit in the FU header

					if (rtp_send_data_hdr(rtp_session, ts, pt, m, cc, &csrc,
									(char *) hdr, 2,
									nalc + curNALOffset,
									nalsize, extn, extn_len, extn_type) < 0) {
										error_msg("There was a problem sending the RTP packet\n");
					}
					lastNALUnitFragment = true;
				}
			}
		}
	}
        if (endptr != start + data_len) {
                error_msg("No NAL found!\n");
        }
}

void tx_send_jpeg(struct tx *tx, struct video_frame *frame,
               struct rtp *rtp_session) {
        uint32_t ts = 0;

        assert(frame->tile_count == 1); // std transmit doesn't handle more than one tile
        assert(!frame->fragment || tx->fec_scheme == FEC_NONE); // currently no support for FEC with fragments
        assert(!frame->fragment || frame->tile_count); // multiple tiles are not currently supported for fragmented send

        ts = get_std_video_local_mediatime();

        struct tile *tile = &frame->tiles[0];
        char pt = PT_JPEG;
        struct jpeg_rtp_data d;

        if (!jpeg_get_rtp_hdr_data((uint8_t *) frame->tiles[0].data, frame->tiles[0].data_len, &d)) {
                exit_uv(1);
                return;
        }

        uint32_t jpeg_hdr[2 /* JPEG hdr */ + 1 /* RM hdr */ + 129 /* QT hdr */];
        int hdr_off = 0;
        unsigned int type_spec = 0u;
        jpeg_hdr[hdr_off++] = htonl(type_spec << 24u);
        jpeg_hdr[hdr_off++] = htonl(d.type << 24u | d.q << 16u | d.width / 8u << 8u | d.height / 8u);
        if (d.restart_interval != 0) {
                // we do not align restart interval on packet boundaries yet
                jpeg_hdr[hdr_off++] = htonl(d.restart_interval << 16u | 1u << 15u | 1u << 14u | 0x3fffu);
        }
        // quantization headers
        if (d.q == 255u) { // we must include the tables
                unsigned int mbz = 0u; // must be zero
                unsigned int precision = 0u;
                unsigned int qt_len = 2 * 64u;
                jpeg_hdr[hdr_off++] = htonl(mbz << 24u | precision << 16u | qt_len);
                memcpy(&jpeg_hdr[hdr_off], d.quantization_tables[0], 64);
                hdr_off += 64 / sizeof(uint32_t);
                memcpy(&jpeg_hdr[hdr_off], d.quantization_tables[1], 64);
                hdr_off += 64 / sizeof(uint32_t);
        }

        char *data = (char *) d.data;
        int bytes_left = tile->data_len - ((char *) d.data - tile->data);
        int max_mtu = tx->mtu - ((rtp_is_ipv6(rtp_session) ? 40 : 20) + 8 + 12); // IP hdr size + UDP hdr size + RTP hdr size

        int fragment_offset = 0;
        do {
                int hdr_len;
                if (fragment_offset == 0) { // include quantization header only in 1st pkt
                        hdr_len = hdr_off * sizeof(uint32_t);
                } else {
                        hdr_len = 8 + (d.restart_interval > 0 ? 4 : 0);
                }
                int data_len = max_mtu - hdr_len;
                int m = 0;
                if (bytes_left <= data_len) {
                        data_len = bytes_left;
                        m = 1;
                }
                jpeg_hdr[0] = htonl(type_spec << 24u | fragment_offset);

                int ret = rtp_send_data_hdr(rtp_session, ts, pt, m, 0, 0,
                                (char *) &jpeg_hdr, hdr_len,
                                data, data_len, 0, 0, 0);
                if (ret < 0) {
                        log_msg(LOG_LEVEL_ERROR, "Error sending RTP/JPEG packet!\n");
                }
                data += data_len;
                bytes_left -= data_len;
                fragment_offset += data_len;
        } while (bytes_left > 0);
}

int tx_get_buffer_id(struct tx *tx)
{
        return tx->buffer;

}

