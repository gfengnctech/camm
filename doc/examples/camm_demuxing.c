/*
 * Copyright (c) 2017 Louis O'Bryan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <inttypes.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <stdarg.h>

static int open_codec_context(int *stream_idx,
                              AVCodecContext **dec_ctx,
                              AVFormatContext *fmt_ctx,
                              enum AVMediaType type,
                              const char *src_filename,
                              int find_decoder)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;
    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    av_log(NULL, AV_LOG_INFO, "best stream: %d\n", ret);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        exit(1);
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];
        av_log(NULL, AV_LOG_INFO, "Codec id: %d, tag: %d\n", st->codecpar->codec_id, st->codecpar->codec_tag);
        if (!find_decoder) {
            *stream_idx = stream_index;
            return 0;
        }
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }
        av_dict_set(&opts, "refcounted_frames", "0", 0);
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }
    return 0;
}

static void write_2_longs(FILE *f, void *data, const char *name1, const char *name2) {
    fputs(name1, f);
    fputs(": ", f);
    fprintf(f, "%" PRId64 " ", AV_RB64(data));
    fputs(name2, f);
    fputs(": ", f);
    fprintf(f, "%" PRId64 "\n", AV_RB64(((uint32_t*)data) + 2));
}

static void write_3_floats(FILE *file, void *data, ...) {
    va_list valist;
    float f;
    uint32_t i;
    int j;
    va_start(valist, data);
    for (j = 0; j < 3; ++j) {
        fputs(va_arg(valist, const char*), file);
        fprintf(file, "[%d]: ", j);
        i = AV_RB32(((uint32_t*)data) + j);
        memcpy(&f, &i, 4);
        fprintf(file, "%f ", f);
    }
    fprintf(file, "\n");
    va_end(valist);
}

static void read_double(void **data, double *d) {
    uint64_t i = AV_RB64(*data);
    memcpy(d, &i, 8);
    *data = ((uint32_t*)(*data)) + 2;
}

static void read_uint32(void **data, uint32_t *i) {
    *i = AV_RB32(*data);
    *data = ((uint32_t*)(*data)) + 1;
}

static void read_float(void **data, float *f) {
    uint32_t i = AV_RB32(*data);
    memcpy(f, &i, 4);
    *data = ((uint32_t*)(*data)) + 1;
}

int main (int argc, char **argv)
{
    int ret = 0;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *video_dec_ctx = NULL;
    int width, height;
    enum AVPixelFormat pix_fmt;
    AVStream *video_stream = NULL, *camm_stream = NULL;
    const char *src_filename = NULL;
    const char *video_dst_filename = NULL;
    const char *camm_dst_filename = NULL;
    FILE *video_dst_file = NULL;
    FILE *camm_dst_file = NULL;
    uint8_t *video_dst_data[4] = {NULL};
    int video_dst_linesize[4];
    int video_dst_bufsize;
    int video_stream_idx = -1, camm_stream_idx = -1;
    AVFrame *frame = NULL;
    AVPacket pkt;
    int video_frame_count = 0;
    int camm_frame_count = 0;
    uint16_t pkt_type;
    void *camm_data;
    float f1, f2, f3, f4, f5, f6, f7;
    uint32_t gps_fix_type;
    double d1, d2, d3;

#ifndef __STDC_IEC_559__
  av_log(NULL, AV_LOG_INFO, stderr,
       "Please ensure your compiler uses IEEE 754 \n"
       "floating point representation. If so, you may safely comment out \n"
       "this guard.\n");
  exit(1);
#endif

    if (argc != 4) {
        av_log(NULL, AV_LOG_ERROR, "usage: %s input_file video_output_file camm_output_file\n"
                "This program shows how to demux video and camm data from a \n"
                "sample mp4 file generated by camm_muxing.c. \n", argv[0]);
        exit(1);
    }
    src_filename = argv[1];
    video_dst_filename = argv[2];
    camm_dst_filename = argv[3];
    av_register_all();
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open source file %s\n", src_filename);
        exit(1);
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find stream information\n");
        exit(1);
    }
    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO, src_filename, 1 /* find_decoder */) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];
        video_dst_file = fopen(video_dst_filename, "wb");
        if (!video_dst_file) {
            av_log(NULL, AV_LOG_ERROR, "Could not open destination file %s\n", video_dst_filename);
            exit(1);
        }
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        ret = av_image_alloc(video_dst_data, video_dst_linesize,
                             width, height, pix_fmt, 1);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate raw video buffer\n");
            exit(1);
        }
        video_dst_bufsize = ret;
    } else {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open video codec.\n"
               "Please make sure this file is formatted correctly.\n");
        exit(1);
    }
    if (open_codec_context(&camm_stream_idx, NULL, fmt_ctx, AVMEDIA_TYPE_DATA, src_filename, 0 /* find_decoder */) >= 0) {
        camm_stream = fmt_ctx->streams[camm_stream_idx];
        camm_dst_file = fopen(camm_dst_filename, "wb");
        if (!camm_dst_file) {
            av_log(NULL, AV_LOG_ERROR, "Could not open destination file %s\n", camm_dst_filename);
            exit(1);
        }
    }
    frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate frame\n");
        exit(1);
    }
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    if (video_stream)
        av_log(NULL, AV_LOG_INFO, "Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
    if (camm_stream)
        av_log(NULL, AV_LOG_INFO, "Demuxing camm data from file '%s' into '%s'\n", src_filename, camm_dst_filename);
    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == video_stream_idx) {
            ret = avcodec_send_packet(video_dec_ctx, &pkt);
            if (ret == 0) {
                while ((ret = avcodec_receive_frame(video_dec_ctx, frame)) == 0) {
                    if (frame->width != width || frame->height != height ||
                        frame->format != pix_fmt) {
                        av_log(NULL, AV_LOG_ERROR,
                            "Error: Width, height and pixel format have to be "
                            "constant in a rawvideo file, but the width, height or "
                            "pixel format of the input video changed:\n"
                            "old: width = %d, height = %d, format = %s\n"
                            "new: width = %d, height = %d, format = %s\n",
                            width, height, av_get_pix_fmt_name(pix_fmt),
                            frame->width, frame->height,
                            av_get_pix_fmt_name(frame->format));
                        exit(1);
                    }
                    av_log(NULL, AV_LOG_INFO, "video_frame_n:%d coded_n:%d\n", video_frame_count++, frame->coded_picture_number);
                    av_image_copy(video_dst_data, video_dst_linesize,
                                  (const uint8_t **)(frame->data), frame->linesize,
                                  pix_fmt, width, height);
                    fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
                }
            }
        } else if (pkt.stream_index == camm_stream_idx) {
            // Writes the camm stream to a file in a human-readable form.
            // Change this if you wish.
            if (pkt.size < 4) {
                av_log(NULL, AV_LOG_ERROR,
                       "camm packet size too small\n"
                       "file contents are formatted incorrectly\n");
                exit(1);
            }
            pkt_type = AV_RB16(((uint16_t*)pkt.data) + 1);
            av_log(NULL, AV_LOG_INFO, "camm_frame_n:%d pkt_size: %d pkt_type: %d\n", camm_frame_count++, pkt.size, pkt_type);
            camm_data = (void*)(((uint32_t*)pkt.data) + 1);
            switch (pkt_type) {
                case 0:
                    write_3_floats(camm_dst_file, camm_data, "angle_axis", "angle_axis", "angle_axis");
                    break;
                case 1:
                    write_2_longs(camm_dst_file, camm_data, "pixel_exposure_time", "rolling_shutter_skew_time");
                    break;
                case 2:
                    write_3_floats(camm_dst_file, camm_data, "gyro", "gyro", "gyro");
                    break;
                case 3:
                    write_3_floats(camm_dst_file, camm_data, "acc", "acc", "acc");
                    break;
                case 4:
                    write_3_floats(camm_dst_file, camm_data, "position", "position", "position");
                    break;
                case 5:
                    write_3_floats(camm_dst_file, camm_data, "latitude", "longitude", "altitude");
                    break;
                case 6:
                    read_double(&camm_data, &d1);
                    read_uint32(&camm_data, &gps_fix_type);
                    read_double(&camm_data, &d2);
                    read_double(&camm_data, &d3);
                    read_float(&camm_data, &f1);
                    read_float(&camm_data, &f2);
                    read_float(&camm_data, &f3);
                    read_float(&camm_data, &f4);
                    read_float(&camm_data, &f5);
                    read_float(&camm_data, &f6);
                    read_float(&camm_data, &f7);
                    fprintf(camm_dst_file,
                            "time_gps_epoch: %f gps_fix_type: %d latitude: %f "
                            "longitude: %f altitude: %f "
                            "horizontal_accuracy: %f vertical_accuracy: %f "
                            "vertical_east: %f vertical_north: %f "
                            "vertical_up: %f speed_accuracy: %f\n", d1,
                            gps_fix_type, d2, d3, f1, f2, f3, f4, f5, f6, f7);
                    break;
                case 7:
                    write_3_floats(camm_dst_file, camm_data, "magnetic_field", "magnetic_field", "magnetic_field");
                    break;
                default:
                    av_log(NULL, AV_LOG_ERROR,
                           "There is a camm packet with invalid type:%d\n",
                           pkt_type);
                    exit(1);
            }
        } else {
            av_log(NULL, AV_LOG_ERROR, "There is a packet from an unrecognized stream.\n");
        }
        av_packet_unref(&pkt);
    }
    av_log(NULL, AV_LOG_INFO, "Demuxing succeeded.\n");
    avcodec_free_context(&video_dec_ctx);
    avformat_close_input(&fmt_ctx);
    if (video_dst_file)
        fclose(video_dst_file);
    if (camm_dst_file)
        fclose(camm_dst_file);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);
    return 0;
}
