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
#include <time.h>
#include <unistd.h>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavutil/avassert.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/intreadwrite.h>
    #include <libavutil/mathematics.h>
    #include <libavutil/opt.h>
    #include <libavutil/timestamp.h>
    #include <libswresample/swresample.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
}

#include <opencv2/opencv.hpp>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>

#include <jpeglib.h>

using namespace cv;
using namespace std;

namespace po = boost::program_options;

#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 1
#define STREAM_PIX_FMT  AV_PIX_FMT_YUV420P
#define SCALE_FLAGS SWS_BICUBIC

#ifdef  __cplusplus 
    static const std::string av_make_error_string(int errnum) { 
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; 
        av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE); 
        return (std::string)errbuf; 
    } 

    #undef av_err2str 
    #define av_err2str(errnum) av_make_error_string(errnum).c_str() 

    static const std::string av_make_time_string(int64_t ts, AVRational *tb) { 
        char *buf = (char *)malloc(AV_TS_MAX_STRING_SIZE * sizeof(char)); 
        buf = av_ts_make_time_string(buf, ts, tb);
        std:string str = std::string(buf);
        free(buf);
        return str; 
    } 
    #undef av_ts2timestr
    #define av_ts2timestr(ts, tb)  av_make_time_string(ts, tb).c_str()

    static const std::string av_make_ts2str(int64_t ts) { 
        char *buf = (char *)malloc(AV_TS_MAX_STRING_SIZE * sizeof(char)); 
        buf = av_ts_make_string(buf, ts);
        st:string str = std::string(buf);
        free(buf);
        return str;
    } 
    #undef av_ts2str
    #define av_ts2str(ts)  av_make_ts2str(ts).c_str()
#endif // __cplusplus 

static int seq = 0;
static string  imageFileExtension = ".jpg";
static string image_fmt = "%010d";
static string workingDirectory = "./";
static bool debug = false;
    
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
    3 * sizeof (float),
    2 * sizeof (uint64_t),
    3 * sizeof (float),
    3 * sizeof (float),
    3 * sizeof (float),
    3 * sizeof (float),
    3 * sizeof (double) + sizeof (uint32_t) + 7 * sizeof (float),
    3 * sizeof (float)
};

static int get_camera_metadata_motion_data_size() {
    int size = 0;
    int i = 0;
    for (i = 0; i < sizeof (metadata_type_sizes) / sizeof (int); i++) {
        size += metadata_type_sizes[i];
    }
    return size;
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt) {
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    av_log(NULL, AV_LOG_INFO, "pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
      av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
      av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
      av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
      pkt->stream_index);
}

static int write_packet(AVFormatContext *fmt_ctx, const AVRational *time_base,
        AVStream *st, AVPacket *pkt) {
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;
    log_packet(fmt_ctx, pkt);
    return av_write_frame(fmt_ctx, pkt);
}

static void add_video_stream(OutputStream *ost, AVFormatContext *oc,
        AVCodec **codec, enum AVCodecID codec_id, const int width, const int height) {
    AVCodecContext *c;

    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        av_log(NULL, AV_LOG_ERROR, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
        exit(1);
    }
    av_log(NULL, AV_LOG_INFO, "Found encoder for '%s'\n", avcodec_get_name(codec_id));
    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams - 1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;
    c->codec_id = codec_id;
    c->bit_rate = 400000;
    c->width = width;
    c->height = height;
    ost->st->time_base = (AVRational){1, STREAM_FRAME_RATE};
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

static void add_camm_stream(OutputStream *ost, AVFormatContext *oc) {
    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams - 1;
    ost->st->time_base = (AVRational){1, STREAM_FRAME_RATE};
    ost->time_base = (AVRational){1, STREAM_FRAME_RATE};
    ost->next_pts = 0;
    ost->tmp_data = (uint16_t*) av_malloc(get_camera_metadata_motion_data_size());
    if (ost->tmp_data == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate memory for CAMM data buffer.\n");
        exit(1);
    }
    ost->st->codecpar->codec_id = AV_CODEC_ID_CAMERA_MOTION_METADATA;
    ost->st->codecpar->codec_tag = MKTAG('c', 'a', 'm', 'm');
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

static int write_camm_packet_data(AVFormatContext *oc, OutputStream *ost) {
    uint16_t packet_type;
    uint16_t *camm_data;
    AVPacket pkt = {0};
    int ret;

    av_init_packet(&pkt);
            
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

    srand((unsigned)time(NULL));
    int random = (int)(rand())/10;
    
    AV_WL32(camm_data, /* X angle axis */ float_to_bytes(random * M_PI / 2));
    AV_WL32(camm_data + 2, /* Y angle axis */ float_to_bytes(-M_PI / 2));
    AV_WL32(camm_data + 4, /* Z angle axis */
                    float_to_bytes(fmod(ost->current_packet_type * random * M_PI / 20,
                    2 * M_PI) - M_PI));

    AV_WL64(camm_data, /* Pixel exposure time in nanoseconds */ 500);
    AV_WL64(camm_data + 4, /* Rolling shutter skew time in nanoseconds */ 300);
 
    AV_WL32(camm_data, /* X gyro */ float_to_bytes(M_PI / 20));
    AV_WL32(camm_data + 2, /* Y gyro */ float_to_bytes(2 * random * M_PI / 20));
    AV_WL32(camm_data + 4, /* Z gyro */ float_to_bytes(3 * random * M_PI / 20));

    AV_WL32(camm_data, /* X acceleration */ float_to_bytes(0.1));
    AV_WL32(camm_data + 2, /* Y acceleration */ float_to_bytes(0.2));
    AV_WL32(camm_data + 4, /* Z acceleration */ float_to_bytes(0.3));

    AV_WL32(camm_data, /* X position */ 0);
    AV_WL32(camm_data + 2, /* Y position */ 0);
    AV_WL32(camm_data + 4, /* Z position */ 0);

    AV_WL32(camm_data, /* latitude in degrees */ float_to_bytes(37.454356 + .001 * ost->current_packet_type));
    AV_WL32(camm_data + 2, /* longitude in degrees */ float_to_bytes(-122.167477 + .001 * ost->current_packet_type));
    AV_WL32(camm_data + 4, /* altitude in meters */ 0);

    AV_WL64(camm_data, /* time GPS epoch in seconds */ double_to_bytes(1500507374.825
                    + ((double) 1) / STREAM_FRAME_RATE));
    camm_data = (uint16_t*) (((double*) camm_data) + 1);
    AV_WL32(camm_data, /* GPS fix type */ 0);
    camm_data = (uint16_t*) (((int32_t*) camm_data) + 1);
    AV_WL64(camm_data, /* latitude in degrees */ double_to_bytes(37.454356 + .001 * ost->current_packet_type));
    camm_data = (uint16_t*) (((double*) camm_data) + 1);
    AV_WL64(camm_data, /* longitude in degrees */ double_to_bytes(-122.167477 + .001 * ost->current_packet_type));
    camm_data = (uint16_t*) (((double*) camm_data) + 1);
    AV_WL32(camm_data, /* altitude in meters */ 0);
    camm_data = (uint16_t*) (((float*) camm_data) + 1);
    AV_WL32(camm_data, /* horizontal accuracy in meters */ float_to_bytes(7.5));
    camm_data = (uint16_t*) (((float*) camm_data) + 1);
    AV_WL32(camm_data, /* vertical accuracy in meters */ float_to_bytes(10.5));
    camm_data = (uint16_t*) (((float*) camm_data) + 1);
    AV_WL32(camm_data, /* vertical east velocity in m/s */ float_to_bytes(1.1));
    camm_data = (uint16_t*) (((float*) camm_data) + 1);
    AV_WL32(camm_data, /* vertical north velocity in m/s */ float_to_bytes(1.1));
    camm_data = (uint16_t*) (((float*) camm_data) + 1);
    AV_WL32(camm_data, /* vertical up velocity in m/s */ 0);
    camm_data = (uint16_t*) (((float*) camm_data) + 1);
    AV_WL32(camm_data, /* speed accuracy in m/s */ float_to_bytes(2.5));

    AV_WL32(camm_data, /* X magnetic field in micro teslas */ float_to_bytes(0.01));
    AV_WL32(camm_data + 2, /* Y magnetic field in micro teslas */ float_to_bytes(0.01));
    AV_WL32(camm_data + 4, /* Z magnetic field in micro teslas */ float_to_bytes(0.01));

    if ((ret = write_packet(oc, &ost->time_base, ost->st, &pkt)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while writing camm data: %s\n", av_err2str(ret));
        exit(1);
    }
           
    return 0;
}

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, const int width, const int height) {
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;
    picture->format = pix_fmt;
    picture->width = width;
    picture->height = height;
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate frame data.\n");
        exit(1);
    }
    return picture;
}

static void open_video_codec(AVFormatContext *oc, AVCodec *codec,
        OutputStream *ost) {
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
    if (c->pix_fmt != STREAM_PIX_FMT) {
        ost->tmp_frame = alloc_picture(STREAM_PIX_FMT, c->width, c->height);
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

static char * getSequenceFileName(int seq){
    string file_fmt = workingDirectory + image_fmt + imageFileExtension;

    char * imageFileName = (char*) malloc(256*sizeof(char));
    sprintf(imageFileName, file_fmt.c_str(), seq);
    
    if (access(imageFileName, F_OK) == -1) {
        av_log(NULL, AV_LOG_ERROR, "file doesn't exist %s\n", imageFileName);
        
        free(imageFileName);
        
        return NULL;
    }
    
    return imageFileName;
}

static void writeJpg(string szFilename, AVFrame* frame, int width, int height) {
    struct jpeg_compress_struct cinfo;

    struct jpeg_error_mgr jerr;

    int row_stride;

    FILE *fp;

    JSAMPROW row_pointer[1];   // A bitmap

    cinfo.err = jpeg_std_error(&jerr);

    jpeg_create_compress(&cinfo);

    fp = fopen(szFilename.c_str(), "wb");

    if(fp == NULL)
        return;

    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = width;    // For width and height, in pixels 
    cinfo.image_height = height;
    cinfo.input_components = 3;   // In 1, said the gray level, if the color bitmap, is 3 
    cinfo.in_color_space = JCS_RGB; //JCS_GRAYSCALE said the grayscale, JCS_RGB color image

    jpeg_set_defaults(&cinfo); 
    jpeg_set_quality (&cinfo, 80, 1);

    jpeg_start_compress(&cinfo, 1);

    row_stride = cinfo.image_width * 3;//The number of bytes in each line, if not the index map, here need to be multiplied by 3

    // For each row compression
    while (cinfo.next_scanline <cinfo.image_height) {
        row_pointer[0] = &(frame->data[0][cinfo.next_scanline * row_stride]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    fclose(fp);
}

static AVFrame *openImage(OutputStream *ost, const int dstWidth, const int dstHeight) {
    char * imageFileName = getSequenceFileName(seq);
    
    if (!imageFileName)
        return NULL;
    
    Mat mat = imread(imageFileName, 1);
    
    free(imageFileName);
    
    cv::Size s = mat.size();
    const int height = s.height;
    const int width = s.width;

    AVFrame *pFrameYUV = av_frame_alloc();
    AVFrame *pFrameBGR = av_frame_alloc();

    pFrameYUV->height = dstHeight;
    pFrameYUV->width = dstWidth;
    pFrameYUV->format = STREAM_PIX_FMT;

    pFrameBGR->height = height;
    pFrameBGR->width = width;
    pFrameBGR->format = AV_PIX_FMT_BGR24;

    int numBytesYUV = av_image_get_buffer_size(STREAM_PIX_FMT, width, height, 1);

    av_log(NULL, AV_LOG_INFO, "numBytesYUV=%d\n", numBytesYUV);
    
    int ret = av_image_fill_arrays(pFrameBGR->data, pFrameBGR->linesize, mat.data, AV_PIX_FMT_BGR24, width, height, 1);
    if(ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "pFrameBGRV=%d\n", ret);
        return NULL;
    }
    
    if (debug)
        writeJpg("writeBGR-" + std::to_string(seq) + ".jpg", pFrameBGR, width, height);
    
    uint8_t* bufferYUV = (uint8_t *) av_malloc(numBytesYUV * sizeof (uint8_t));
    
    ret = av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, bufferYUV, STREAM_PIX_FMT, dstWidth, dstHeight, 1);

    if(ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "pFrameYUV=%d\n", ret);
        return NULL;
    }
    
    if (debug)
        writeJpg("writeYUV-" + std::to_string(seq) + ".jpg", pFrameYUV, dstWidth, dstHeight);
    
    // Initialise Software scaling context
    struct SwsContext *sws_ctx = sws_getContext(width,
            height,
            AV_PIX_FMT_BGR24,
            dstWidth,
            dstHeight,
            STREAM_PIX_FMT,
            SCALE_FLAGS,
            NULL,
            NULL,
            NULL
            );

    // Convert the image from its BGR to YUV
    sws_scale(sws_ctx, (uint8_t const * const *) pFrameBGR->data,
            pFrameBGR->linesize, 0, height,
                pFrameYUV->data, pFrameYUV->linesize);

    ost->frame->pts = ost->next_pts++;
    
    seq++;
    
    //av_frame_free(&pFrameBGR);
    //free(bufferYUV);
    
    //sws_freeContext(sws_ctx);
    
    return pFrameYUV;
}

static int write_video_frame(AVFormatContext *oc, OutputStream *ost, const int dstWidth, const int dstHeight) {
    int ret;
    AVCodecContext *c;
    AVFrame *frame;
    int got_packet = 0;
    AVPacket pkt = {0};

    c = ost->enc;
    frame = openImage(ost, dstWidth, dstHeight);
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
    
    ret = (frame) ? 1 : 0 ;
    
    av_frame_free(&frame);
    
    return (ret || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
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

int main(int argc, char **argv) {
    
    OutputStream video_st = {0}, camm_st = {0};
    const char *filename;
    AVOutputFormat *fmt;
    AVFormatContext *oc;
    AVCodec *video_codec;
    int ret;
    int write_video = 1, write_camm = 1;
    char creation_time[40];
    time_t timer;
    struct tm *tm_info;
    
    try {
        po::options_description desc("Allowed options");
	desc.add_options()
		("help,h", "Help Screen")
                ("file-name-format,f", po::value<std::string>()->default_value("%010d"), "Image file name format")
        	("working-directory,d", po::value<std::string>()->default_value("./"), "Working directory")
		("image-file-extension,i", po::value<std::string>()->default_value(".jpg"), "The type of image file including .")
                ("debug", po::value<bool>()->default_value(false), "Print out the intermediate frames");

        if (argc < 2) {
            av_log(NULL, AV_LOG_INFO, "usage: %s output_file\nThis program generates synthetic camm data and video streams and\nmuxes them into a file named output_file.\n\n", argv[0]);
        
            std::cout << desc << std::endl;
        
            return 1;
        }
        
        po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);
                
	if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
	} else {
            if (vm.count("file-name-format")) {
		image_fmt = vm["file-name-format"].as<std::string>();
            }
            
            if (vm.count("working-directory")) {
		workingDirectory = vm["working-directory"].as<std::string>();
            }
            
            if (vm.count("image-file-extension")) {
		imageFileExtension = vm["image-file-extension"].as<std::string>();
            }
            
            if (vm.count("debug")) {
		debug = vm["debug"].as<bool>();
            }
        }
    } catch (const po::error &ex) {
	std::cerr << ex.what() << std::endl;
        return 2;
    }
    
    char * imageFileName = getSequenceFileName(0);
    if (!imageFileName) {
        av_log(NULL, AV_LOG_ERROR, "file doesn't exist %s\n", imageFileName);
        return 1;
    }
    
    Mat frame = imread(imageFileName, 1);
    
    free(imageFileName);
    
    Size s = frame.size();
    const int height = s.height;
    const int width = s.width;
    
    
#ifndef __STDC_IEC_559__
    av_log(NULL, AV_LOG_INFO, stderr,
    //     "Please ensure your compiler uses IEEE 754 \n"
    //     "floating point representation. If so, you may safely comment out \n"
    //     "this guard.\n");
    exit(1);
#endif

    av_register_all();
    
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
    if (!oc->metadata) {
        av_log(NULL, AV_LOG_ERROR, "There is no metadata for the format context.\n");
    }
    // Set the creation time metadata.
    time(&timer);
    tm_info = localtime(&timer);
    strftime(creation_time, 40, "%Y-%m-%dT%H:%M:%SZ", tm_info);
    av_log(NULL, AV_LOG_INFO, "Setting creation time: %s\n", creation_time);
    av_dict_set(&oc->metadata, "creation_time", creation_time, 0);
    av_dict_set(&oc->metadata, "creation_by", "NCTech Ltd", 0);
    add_video_stream(&video_st, oc, &video_codec, fmt->video_codec, width, height);
    add_camm_stream(&camm_st, oc);
    open_video_codec(oc, video_codec, &video_st);
    av_log(NULL, AV_LOG_INFO, "Opening the output file.\n");
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open '%s': %s\n", filename, av_err2str(ret));
            return 1;
        }
    }
    av_log(NULL, AV_LOG_INFO, "Writing the stream header. nb_streams: %d\n", oc->nb_streams);
    ret = avformat_write_header(oc, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when writing the stream header: %s\n", av_err2str(ret));
        return 1;
    }
    av_log(NULL, AV_LOG_INFO, "Wrote the stream header.\n");
    av_log(NULL, AV_LOG_INFO, "Writing the streams.\n");
    while (write_video || write_camm) {
        if (write_video
                && (write_camm && av_compare_mod(video_st.next_pts, camm_st.next_pts, 10000000000) == 0)) {
            write_video = !write_video_frame(oc, &video_st, width, height);
            
            av_log(NULL, AV_LOG_INFO, "Write image data %d.\n", write_video);
            
            if (!write_video)
                break;
        } else {
            write_camm = !write_camm_packet_data(oc, &camm_st);
            av_log(NULL, AV_LOG_INFO, "Write camm data %d.\n", write_camm);
            
            if (!write_camm)
                break;
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
