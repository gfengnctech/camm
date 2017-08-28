CC=g++

# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \
		opencv				   \
		
	
OpenCV_Libs=    /usr/local/share/OpenCV/3rdparty/lib/libippicv.a \
                /usr/local/share/OpenCV/3rdparty/lib/libIlmImf.a  \
		/usr/lib/x86_64-linux-gnu/libjpeg.a \
		/usr/lib/x86_64-linux-gnu/libjasper.a \
		/usr/local/share/OpenCV/3rdparty/lib/liblibwebp.a \
		/usr/local/share/OpenCV/3rdparty/lib/liblibpng.a \
		/usr/lib/x86_64-linux-gnu/libtiff.a \
		/usr/lib/x86_64-linux-gnu/libjbig.a \


CFLAGS += -Wall -g
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(FFMPEG_LIBS)) $(LDLIBS) $(OpenCV_Libs)
EXAMPLES=       avio_dir_cmd                       \
                avio_reading                       \
                camm_muxing                        \
                camm_demuxing                      \
                decode_audio                       \
                decode_video                       \
                demuxing_decoding                  \
                encode_audio                       \
                encode_video                       \
                extract_mvs                        \
                filtering_video                    \
                filtering_audio                    \
                http_multiclient                   \
                metadata                           \
                muxing                             \
                remuxing                           \
                resampling_audio                   \
                scaling_video                      \
                transcode_aac                      \
                transcoding                        \


OBJS=$(addsuffix .o,$(EXAMPLES))

# the following examples make explicit use of the math library
avcodec:           LDLIBS += -lm
encode_audio:      LDLIBS += -lm
muxing:            LDLIBS += -lm
resampling_audio:  LDLIBS += -lm

.phony: all clean-test clean

all: $(OBJS) $(EXAMPLES)

clean-test:
	$(RM) test*.pgm test.h264 test.mp2 test.sw test.mpg

clean: clean-test
	$(RM) $(EXAMPLES) $(OBJS)
