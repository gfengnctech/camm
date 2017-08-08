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
 *
 *
 * This sample shows how to mux camera motion metadata (camm) data from
 * camera sensors with a video stream into the same mp4 file.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <libavcodec/avcodec.h>

#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25
#define STREAM_PIX_FMT  AV_PIX_FMT_YUV420P
#define SCALE_FLAGS SWS_BICUBIC

typedef struct OutputStream {
  AVStream *st;
  AVCodecContext *enc;
  int64_t next_pts;
  AVRational time_base;
  AVFrame *frame;
  AVFrame *tmp_frame;
  uint8_t *frame_data;
  struct SwsContext *sws_ctx;
  int current_packet_type;
  uint16_t *tmp_data;
} OutputStream;

static const int metadata_type_sizes[] = {
  3 * sizeof(float),
  2 * sizeof(uint64_t),
  3 * sizeof(float),
  3 * sizeof(float),
  3 * sizeof(float),
  3 * sizeof(float),
  3 * sizeof(double) + sizeof(uint32_t) + 7 * sizeof(float),
  3 * sizeof(float)
};

static int get_camera_metadata_motion_data_size() {
  int size = 0;
  int i = 0;
  for (i = 0; i < sizeof(metadata_type_sizes) / sizeof(int); i++) {
    size += metadata_type_sizes[i];
  }
  return size;
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
  AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
  av_log(NULL, AV_LOG_INFO, "pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
    av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
    av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
    av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
    pkt->stream_index);
}

static int write_packet(AVFormatContext *fmt_ctx, const AVRational *time_base,
                        AVStream *st, AVPacket *pkt)
{
  av_packet_rescale_ts(pkt, *time_base, st->time_base);
  pkt->stream_index = st->index;
  log_packet(fmt_ctx, pkt);
  return av_write_frame(fmt_ctx, pkt);
}

static void add_video_stream(OutputStream *ost, AVFormatContext *oc,
               AVCodec **codec,
               enum AVCodecID codec_id)
{
  AVCodecContext *c;

  *codec = avcodec_find_encoder(codec_id);
  if (!(*codec)) {
    av_log(NULL, AV_LOG_ERROR, "Could not find encoder for '%s'\n",
        avcodec_get_name(codec_id));
    exit(1);
  }
  av_log(NULL, AV_LOG_INFO, "Found encoder for '%s'\n", avcodec_get_name(codec_id));
  ost->st = avformat_new_stream(oc, NULL);
  if (!ost->st) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate stream\n");
    exit(1);
  }
  ost->st->id = oc->nb_streams-1;
  c = avcodec_alloc_context3(*codec);
  if (!c) {
    av_log(NULL, AV_LOG_ERROR, "Could not alloc an encoding context\n");
    exit(1);
  }
  ost->enc = c;
  c->codec_id = codec_id;
  c->bit_rate = 400000;
  c->width = 352;
  c->height = 288;
  ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
  c->time_base = ost->st->time_base;
  c->gop_size = 12;
  c->pix_fmt = STREAM_PIX_FMT;
  if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
    c->max_b_frames = 2;
  }
  if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
    c->mb_decision = 2;
  }
  if (oc->oformat->flags & AVFMT_GLOBALHEADER)
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

static void add_camm_stream(OutputStream *ost, AVFormatContext *oc)
{
  ost->st = avformat_new_stream(oc, NULL);
  if (!ost->st) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate stream\n");
    exit(1);
  }
  ost->st->id = oc->nb_streams-1;
  ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
  ost->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
  ost->next_pts = 0;
  ost->tmp_data =
    (uint16_t*) av_malloc(get_camera_metadata_motion_data_size());
  if (ost->tmp_data == NULL) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate memory for CAMM data buffer.\n");
    exit(1);
  }
  ost->st->codecpar->codec_id = AV_CODEC_ID_CAMERA_MOTION_METADATA;
  ost->st->codecpar->codec_tag = MKTAG('c','a','m','m');
}

static uint64_t double_to_bytes(double d) {
  uint64_t result;
  memcpy(&result, &d, 8);
  return result;
}

static uint32_t float_to_bytes(float f) {
  uint32_t result;
  memcpy(&result, &f, 4);
  return result;
}

static int write_camm_packet_data(AVFormatContext *oc, OutputStream *ost)
{
  uint16_t packet_type;
  uint16_t *camm_data;
  AVPacket pkt = { 0 };
  int ret;

  av_init_packet(&pkt);
  if (av_compare_ts(ost->next_pts, ost->time_base,
            STREAM_DURATION, (AVRational){ 1, 1 }) >= 0) {
    av_log(NULL, AV_LOG_INFO, "Finished writing camm stream.\n");
    return 1;
  }
  pkt.pts = ost->next_pts;
  pkt.flags = AV_PKT_FLAG_KEY;
  pkt.duration = 1;
  pkt.dts = pkt.pts;
  ost->next_pts += 1;
  packet_type = ost->current_packet_type++ % 8;
  pkt.size = 4 + metadata_type_sizes[packet_type];
  memset(ost->tmp_data, 0, pkt.size);
  pkt.data = (uint8_t*) ost->tmp_data;
  AV_WL16(ost->tmp_data + 1, packet_type);
  camm_data = ost->tmp_data + 2;
  switch (packet_type) {
    case 0:
      AV_WL32(camm_data,     /* X angle axis */ float_to_bytes(M_PI / 2));
      AV_WL32(camm_data + 2, /* Y angle axis */ float_to_bytes(-M_PI / 2));
      AV_WL32(camm_data + 4, /* Z angle axis */
              float_to_bytes(fmod(ost->current_packet_type * M_PI / 20,
                                  2 * M_PI) - M_PI));
      break;
    case 1:
      AV_WL64(camm_data, /* Pixel exposure time in nanoseconds */ 500);
      AV_WL64(camm_data + 4,
              /* Rolling shutter skew time in nanoseconds */ 300);
      break;
    case 2:
      AV_WL32(camm_data,     /* X gyro */ 0);
      AV_WL32(camm_data + 2, /* Y gyro */ 0);
      AV_WL32(camm_data + 4, /* Z gyro */ float_to_bytes(M_PI / 20));
      break;
    case 3:
      AV_WL32(camm_data,     /* X acceleration */ 0);
      AV_WL32(camm_data + 2, /* Y acceleration */ 0);
      AV_WL32(camm_data + 4, /* Z acceleration */ 0);
      break;
    case 4:
      AV_WL32(camm_data,     /* X position */ 0);
      AV_WL32(camm_data + 2, /* Y position */ 0);
      AV_WL32(camm_data + 4, /* Z position */ 0);
      break;
    case 5:
      AV_WL32(camm_data, /* latitude in degrees */
              float_to_bytes(37.454356 + .001 * ost->current_packet_type));
      AV_WL32(camm_data + 2, /* longitude in degrees */
              float_to_bytes(-122.167477 + .001 * ost->current_packet_type));
      AV_WL32(camm_data + 4, /* altitude in meters */ 0);
      break;
    case 6:
      AV_WL64(camm_data, /* time GPS epoch in seconds */
              double_to_bytes(1500507374.825
                              + ((double)1) / STREAM_FRAME_RATE));
      camm_data = (uint16_t*) (((double*)camm_data) + 1);
      AV_WL32(camm_data, /* GPS fix type */ 0);
      camm_data = (uint16_t*) (((int32_t*)camm_data) + 1);
      AV_WL64(camm_data, /* latitude in degrees */
              double_to_bytes(37.454356 + .001 * ost->current_packet_type));
      camm_data = (uint16_t*) (((double*)camm_data) + 1);
      AV_WL64(camm_data, /* longitude in degrees */
              double_to_bytes(-122.167477 + .001 * ost->current_packet_type));
      camm_data = (uint16_t*) (((double*)camm_data) + 1);
      AV_WL32(camm_data, /* altitude in meters */ 0);
      camm_data = (uint16_t*) (((float*)camm_data) + 1);
      AV_WL32(camm_data,
              /* horizontal accuracy in meters */ float_to_bytes(7.5));
      camm_data = (uint16_t*) (((float*)camm_data) + 1);
      AV_WL32(camm_data,
              /* vertical accuracy in meters */ float_to_bytes(10.5));
      camm_data = (uint16_t*) (((float*)camm_data) + 1);
      AV_WL32(camm_data,
              /* vertical east velocity in m/s */ float_to_bytes(1.1));
      camm_data = (uint16_t*) (((float*)camm_data) + 1);
      AV_WL32(camm_data,
              /* vertical north velocity in m/s */ float_to_bytes(1.1));
      camm_data = (uint16_t*) (((float*)camm_data) + 1);
      AV_WL32(camm_data, /* vertical up velocity in m/s */ 0);
      camm_data = (uint16_t*) (((float*)camm_data) + 1);
      AV_WL32(camm_data, /* speed accuracy in m/s */ float_to_bytes(2.5));
      break;
    case 7:
      AV_WL32(camm_data,     /* X magnetic field in micro teslas */ float_to_bytes(0.01));
      AV_WL32(camm_data + 2, /* Y magnetic field in micro teslas */ float_to_bytes(0.01));
      AV_WL32(camm_data + 4, /* Z magnetic field in micro teslas */ float_to_bytes(0.01));
      break;
    default:
      break;
  }
  if ((ret = write_packet(oc, &ost->time_base, ost->st, &pkt)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error while writing camm data: %s\n", av_err2str(ret));
    exit(1);
  }
  return 0;
}

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width,
                              int height)
{
  AVFrame *picture;
  int ret;

  picture = av_frame_alloc();
  if (!picture)
    return NULL;
  picture->format = pix_fmt;
  picture->width  = width;
  picture->height = height;
  ret = av_frame_get_buffer(picture, 32);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate frame data.\n");
    exit(1);
  }
  return picture;
}

static void open_video_codec(AVFormatContext *oc, AVCodec *codec,
                             OutputStream *ost)
{
  int ret;
  AVCodecContext *c = ost->enc;
  AVDictionary *opt = NULL;

  av_dict_copy(&opt, NULL, 0);
  ret = avcodec_open2(c, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not open video codec: %s\n", av_err2str(ret));
    exit(1);
  }
  ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
  if (!ost->frame) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate video frame\n");
    exit(1);
  }
  ost->tmp_frame = NULL;
  if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
    ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
    if (!ost->tmp_frame) {
      av_log(NULL, AV_LOG_ERROR, "Could not allocate temporary picture\n");
      exit(1);
    }
  }
  ret = avcodec_parameters_from_context(ost->st->codecpar, c);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not copy the stream parameters\n");
    exit(1);
  }
}

static void fill_yuv_image(AVFrame *pict, int frame_index,
               int width, int height)
{
  int x, y, i;

  i = frame_index;
  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;
  for (y = 0; y < height / 2; y++) {
    for (x = 0; x < width / 2; x++) {
      pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
      pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
    }
  }
}

static AVFrame *get_video_frame(OutputStream *ost)
{
  AVCodecContext *c = ost->enc;

  if (av_compare_ts(ost->next_pts, c->time_base,
            STREAM_DURATION, (AVRational){ 1, 1 }) >= 0)
    return NULL;
  if (av_frame_make_writable(ost->frame) < 0)
    exit(1);
  if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
    if (!ost->sws_ctx) {
      ost->sws_ctx = sws_getContext(c->width, c->height,
                      AV_PIX_FMT_YUV420P,
                      c->width, c->height,
                      c->pix_fmt,
                      SCALE_FLAGS, NULL, NULL, NULL);
      if (!ost->sws_ctx) {
        av_log(NULL, AV_LOG_ERROR,
            "Could not initialize the conversion context\n");
        exit(1);
      }
    }
    fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
    sws_scale(ost->sws_ctx,
      (const uint8_t * const *)ost->tmp_frame->data, ost->tmp_frame->linesize,
      0, c->height, ost->frame->data, ost->frame->linesize);
  } else {
    fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
  }
  ost->frame->pts = ost->next_pts++;
  return ost->frame;
}

static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
  int ret;
  AVCodecContext *c;
  AVFrame *frame;
  int got_packet = 0;
  AVPacket pkt = { 0 };

  c = ost->enc;
  frame = get_video_frame(ost);
  av_init_packet(&pkt);
  ret = avcodec_send_frame(c, frame);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error encoding video frame: %s\n", av_err2str(ret));
    exit(1);
  }
  while (1) {
    ret = avcodec_receive_packet(c, &pkt);
    if (ret == 0) {
      if ((ret = write_packet(oc, &c->time_base, ost->st, &pkt)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while writing video frame: %s\n", av_err2str(ret));
        exit(1);
      }
      got_packet = 1;
    } else if (ret == AVERROR(EINVAL)) {
      av_log(NULL, AV_LOG_ERROR, "Error while encoding video frame :%s\n", av_err2str(ret));
      exit(1);
    } else {
      break;
    }
  }
  return (frame || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
  if (ost->enc) {
    avcodec_free_context(&ost->enc);
  }
  if (ost->frame) {
    av_frame_free(&ost->frame);
  }
  if (ost->tmp_frame) {
    av_frame_free(&ost->tmp_frame);
  }
  if (ost->sws_ctx) {
    sws_freeContext(ost->sws_ctx);
  }
  if (ost->tmp_data) {
    av_free(ost->tmp_data);
  }
}

int main(int argc, char **argv)
{
  OutputStream video_st = { 0 }, camm_st = { 0 };
  const char *filename;
  AVOutputFormat *fmt;
  AVFormatContext *oc;
  AVCodec *video_codec;
  int ret;
  int write_video = 1, write_camm = 1;

#ifndef __STDC_IEC_559__
  av_log(NULL, AV_LOG_INFO, stderr,
       "Please ensure your compiler uses IEEE 754 \n"
       "floating point representation. If so, you may safely comment out \n"
       "this guard.\n");
  exit(1);
#endif

  av_register_all();
  if (argc != 2) {
    av_log(NULL, AV_LOG_INFO, "usage: %s output_file\n"
         "This program generates synthetic camm data and video streams and\n"
         "muxes them into a file named output_file.\n"
         "\n", argv[0]);
    return 1;
  }
  filename = argv[1];
  av_log_set_level(AV_LOG_DEBUG);
  avformat_alloc_output_context2(&oc, NULL, "mp4", NULL);
  if (!oc) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate output context.\n");
    return 1;
  }
  if (oc->oformat == NULL) {
    av_log(NULL, AV_LOG_ERROR, "Output context format is null.\n");
    return 1;
  }
  fmt = oc->oformat;
  if (!fmt->video_codec) {
    av_log(NULL, AV_LOG_ERROR, "Could not find video codec for file format.\n");
    return 1;
  }
  add_video_stream(&video_st, oc, &video_codec, fmt->video_codec);
  add_camm_stream(&camm_st, oc);
  open_video_codec(oc, video_codec, &video_st);
  av_log(NULL, AV_LOG_INFO, "Opening the output file.\n");
  if (!(fmt->flags & AVFMT_NOFILE)) {
    ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      av_log(NULL, AV_LOG_ERROR, "Could not open '%s': %s\n", filename,
          av_err2str(ret));
      return 1;
    }
  }
  av_log(NULL, AV_LOG_INFO, "Writing the stream header. nb_streams: %d\n", oc->nb_streams);
  ret = avformat_write_header(oc, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error occurred when writing the stream header: %s\n",
        av_err2str(ret));
    return 1;
  }
  av_log(NULL, AV_LOG_INFO, "Wrote the stream header.\n");
  av_log(NULL, AV_LOG_INFO, "Writing the streams.\n");
  while (write_video || write_camm) {
    if (write_video
        && (!write_camm || av_compare_ts(video_st.next_pts,
                                         video_st.enc->time_base,
                                         camm_st.next_pts,
                                         camm_st.time_base) <= 0)) {
      write_video = !write_video_frame(oc, &video_st);
    } else {
      write_camm = !write_camm_packet_data(oc, &camm_st);
    }
  }
  av_log(NULL, AV_LOG_INFO, "Wrote the streams.\n");
  av_log(NULL, AV_LOG_INFO, "Writing the trailer.\n");
  av_write_trailer(oc);
  av_log(NULL, AV_LOG_INFO, "Wrote the trailer.\n");
  close_stream(oc, &video_st);
  av_log(NULL, AV_LOG_INFO, "Closed video stream.\n");
  close_stream(oc, &camm_st);
  av_log(NULL, AV_LOG_INFO, "Closed camm stream.\n");
  if (!(fmt->flags & AVFMT_NOFILE))
    avio_closep(&oc->pb);
  av_log(NULL, AV_LOG_INFO, "Closed the output file.\n");
  avformat_free_context(oc);
  av_log(NULL, AV_LOG_INFO, "Freed the format context.\n");
  return 0;
}
