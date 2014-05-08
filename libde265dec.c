/*
 * H.265 decoder
 *
 * Copyright (c) 2013, Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * H.265 decoder based on libde265
 */

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>

#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#ifdef __cplusplus
}
#endif

#include <libde265/de265.h>

#if !defined(LIBDE265_NUMERIC_VERSION) || LIBDE265_NUMERIC_VERSION < 0x00060000
#error "You need libde265 0.6 or newer to compile this plugin."
#endif

#include "libde265dec.h"

typedef struct DE265DecoderContext {
    de265_decoder_context* decoder;

    int check_extra;
    int packetized;
    int length_size;
} DE265Context;

static int ff_libde265dec_decode(AVCodecContext *avctx,
                                 void *data, int *got_frame, AVPacket *avpkt)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    AVFrame *picture = (AVFrame *) data;
    const struct de265_image *img;
    de265_error err;
    int ret;
    int64_t pts;
    int more = 0;

    const uint8_t* src[4];
    int stride[4];

    if (ctx->check_extra) {
        int extradata_size = avctx->extradata_size;
        ctx->check_extra = 0;
        if (extradata_size > 0) {
            unsigned char *extradata = (unsigned char *) avctx->extradata;
            if (extradata_size > 3 && extradata != NULL && (extradata[0] || extradata[1] || extradata[2] > 1)) {
                ctx->packetized = 1;
                if (extradata_size > 21) {
                    ctx->length_size = (extradata[21] & 3) + 1;
                }
                av_log(avctx, AV_LOG_DEBUG, "Assuming packetized data (%d bytes length)\n", ctx->length_size);
            } else {
                ctx->packetized = 0;
                av_log(avctx, AV_LOG_DEBUG, "Assuming non-packetized data\n");
                err = de265_push_data(ctx->decoder, extradata, extradata_size, 0, NULL);
                if (!de265_isOK(err)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to push extra data: %s (%d)\n", de265_get_error_text(err), err);
                    return AVERROR_INVALIDDATA;
                }
            }
#if LIBDE265_NUMERIC_VERSION >= 0x00070000
            de265_push_end_of_NAL(ctx->decoder);
#endif
            do {
                err = de265_decode(ctx->decoder, &more);
                switch (err) {
                case DE265_OK:
                    break;

                case DE265_ERROR_IMAGE_BUFFER_FULL:
                case DE265_ERROR_WAITING_FOR_INPUT_DATA:
                    // not really an error
                    more = 0;
                    break;

                default:
                    if (!de265_isOK(err)) {
                        av_log(avctx, AV_LOG_ERROR, "Failed to decode extra data: %s (%d)\n", de265_get_error_text(err), err);
                        return AVERROR_INVALIDDATA;
                    }
                }
            } while (more);
        }
    }

    if (avpkt->size > 0) {
        if (avpkt->pts != AV_NOPTS_VALUE) {
            pts = avpkt->pts;
        } else {
            pts = avctx->reordered_opaque;
        }

        if (ctx->packetized) {
            uint8_t* avpkt_data = avpkt->data;
            uint8_t* avpkt_end = avpkt->data + avpkt->size;
            while (avpkt_data + ctx->length_size <= avpkt_end) {
                int nal_size = 0;
                int i;
                for (i=0; i<ctx->length_size; i++) {
                    nal_size = (nal_size << 8) | avpkt_data[i];
                }
                err = de265_push_NAL(ctx->decoder, avpkt_data + ctx->length_size, nal_size, pts, NULL);
                if (err != DE265_OK) {
                    const char *error = de265_get_error_text(err);
                    av_log(avctx, AV_LOG_ERROR, "Failed to push data: %s\n", error);
                    return AVERROR_INVALIDDATA;
                }
                avpkt_data += ctx->length_size + nal_size;
            }
        } else {
            err = de265_push_data(ctx->decoder, avpkt->data, avpkt->size, pts, NULL);
            if (err != DE265_OK) {
                const char *error = de265_get_error_text(err);
                av_log(avctx, AV_LOG_ERROR, "Failed to push data: %s\n", error);
                return AVERROR_INVALIDDATA;
            }
        }
    } else {
        de265_flush_data(ctx->decoder);
    }

    // decode as much as possible
    do {
        err = de265_decode(ctx->decoder, &more);
    } while (more && err == DE265_OK);

    switch (err) {
    case DE265_OK:
    case DE265_ERROR_IMAGE_BUFFER_FULL:
    case DE265_ERROR_WAITING_FOR_INPUT_DATA:
        break;

    default:
        {
            const char *error  = de265_get_error_text(err);

            av_log(avctx, AV_LOG_ERROR, "Failed to decode frame: %s\n", error);
            return AVERROR_INVALIDDATA;
        }
    }

    if ((img = de265_get_next_picture(ctx->decoder)) != NULL) {
        int width;
        int height;
        if (de265_get_chroma_format(img) != de265_chroma_420) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d)\n",
                   de265_get_chroma_format(img));
            return AVERROR_INVALIDDATA;
        }

        width  = de265_get_image_width(img,0);
        height = de265_get_image_height(img,0);
        if (width != avctx->width || height != avctx->height) {
            if (avctx->width != 0)
                av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                       avctx->width, avctx->height, width, height);

            if (av_image_check_size(width, height, 0, avctx)) {
                return AVERROR_INVALIDDATA;
            }

            avcodec_set_dimensions(avctx, width, height);
        }

        picture->width = avctx->width;
        picture->height = avctx->height;
        picture->format = avctx->pix_fmt;
        if ((ret = av_frame_get_buffer(picture, 32)) < 0) {
            return ret;
        }

        for (int i=0;i<4;i++) {
            src[i] = de265_get_image_plane(img,i, &stride[i]);
        }

        av_image_copy(picture->data, picture->linesize, src, stride,
                      avctx->pix_fmt, width, height);

        *got_frame = 1;

        picture->reordered_opaque = de265_get_image_PTS(img);
        picture->pkt_pts = de265_get_image_PTS(img);
    }
    return avpkt->size;
}


static av_cold int ff_libde265dec_free(AVCodecContext *avctx)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    de265_free_decoder(ctx->decoder);
    return 0;
}


static av_cold void ff_libde265dec_flush(AVCodecContext *avctx)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    de265_reset(ctx->decoder);
}


static av_cold void ff_libde265dec_static_init(struct AVCodec *codec)
{
    // No initialization required
}


static av_cold int ff_libde265dec_ctx_init(AVCodecContext *avctx)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    ctx->decoder = de265_new_decoder();
    // XXX: always decode multiple threads for now
    if (1 || avctx->active_thread_type & FF_THREAD_SLICE) {
        int threads = avctx->thread_count;
        if (threads <= 0) {
            threads = av_cpu_count();
        }
        if (threads > 0) {
            // XXX: We start more threads than cores for now, as some threads
            // might get blocked while waiting for dependent data. Having more
            // threads increases decoding speed by about 10%
            threads *= 2;
            if (threads > 32) {
                // TODO: this limit should come from the libde265 headers
                threads = 32;
            }
            de265_start_worker_threads(ctx->decoder, threads);
        }
    }
    ctx->check_extra = 1;
    ctx->packetized = 1;
    ctx->length_size = 4;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    return 0;
}

static void ff_libde265dec_unregister_codecs(enum AVCodecID id)
{
    AVCodec *prev = NULL;
    AVCodec *codec = av_codec_next(NULL);
    while (codec != NULL) {
        AVCodec *next = av_codec_next(codec);
        if (codec->id == id) {
            if (prev != NULL) {
                // remove previously registered codec with the same id
                // NOTE: this won't work for the first registered codec
                //       which is fine for this use case
                prev->next = next;
            } else {
                prev = codec;
            }
        } else {
            prev = codec;
        }
        codec = next;
    }
}


AVCodec ff_libde265_decoder;

void libde265dec_register(void)
{
    static int registered = 0;

    if (registered) {
        return;
    }

    registered = 1;
    ff_libde265dec_unregister_codecs(AV_CODEC_ID_H265);
    memset(&ff_libde265_decoder, 0, sizeof(AVCodec));
    ff_libde265_decoder.name           = "libde265";
    ff_libde265_decoder.type           = AVMEDIA_TYPE_VIDEO;
    ff_libde265_decoder.id             = AV_CODEC_ID_H265;
    ff_libde265_decoder.priv_data_size = sizeof(DE265Context);
    ff_libde265_decoder.init_static_data = ff_libde265dec_static_init;
    ff_libde265_decoder.init           = ff_libde265dec_ctx_init;
    ff_libde265_decoder.close          = ff_libde265dec_free;
    ff_libde265_decoder.decode         = ff_libde265dec_decode;
    ff_libde265_decoder.flush          = ff_libde265dec_flush;
    ff_libde265_decoder.capabilities   = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS | CODEC_CAP_DR1 |
                                         CODEC_CAP_SLICE_THREADS;
    ff_libde265_decoder.long_name      = "libde265 H.265/HEVC decoder";

    avcodec_register(&ff_libde265_decoder);
}
