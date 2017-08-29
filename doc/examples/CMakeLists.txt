cmake_minimum_required (VERSION 3.2.3 FATAL_ERROR)
project (camm)

OPTION(USE_BOOST "use BOOST library" ON)
OPTION(USE_OPENCV "use OpenCV library." ON)
OPTION(USE_FFMPEG "use ffmpeg" ON)

# The version number.
set (camm_VERSION_MAJOR 0)
set (camm_VERSION_MINOR 1)

if (MSVC)
   add_definitions (-D_USE_MATH_DEFINES)

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP ")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}  /MTd" )
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Ot /openmp /MT" )
else (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    set(UPATH "/usr/lib/x86_64-linux-gnu")
    set(MYLIBS ${UPATH}/libtiff.a ${UPATH}/libjpeg.a ${UPATH}/libjbig.a ${UPATH}/libz.a ${UPATH}/libdl.a ${UPATH}/libz.a ${UPATH}/libm.a ${UPATH}/liblzma.a ${UPATH}/libjasper.a /usr/lib/gcc/x86_64-linux-gnu/5/libstdc++.a ${UPATH}/librt.a)
endif (MSVC)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/" ${CMAKE_MODULE_PATH})


#--------------------------------------------------------------------------------BOOST
if(USE_BOOST)
    if (MSVC)
        set(Boost_USE_STATIC_LIBS ON)
        set(Boost_USE_STATIC_RUNTIME ON)
        file(TO_CMAKE_PATH $ENV{BOOST_ROOT} BOOST_ROOT)
        set(BOOST_LIBRARYDIR ${BOOST_ROOT}/lib64-msvc-12.0)
    endif (MSVC)
    find_package(Boost COMPONENTS chrono date_time filesystem system program_options thread timer  REQUIRED)

endif(USE_BOOST)

if (USE_FFMPEG)
    set(FFMPEG "-I/usr/local/include -I/usr/include/x86_64-linux-gnu -L/usr/local/lib -lavdevice -ldl -lxcb -lxcb-xfixes -lxcb-render -lxcb-shape -lxcb -lxcb-shape -lxcb -lm -llzma -lz -pthread -lavfilter -ldl -lxcb -lxcb-xfixes -lxcb-render -lxcb-shape -lxcb -lxcb-shape -lxcb -lm -llzma -lz -pthread -lavformat -ldl -lxcb -lxcb-xfixes -lxcb-render -lxcb-shape -lxcb -lxcb-shape -lxcb -lm -llzma -lz -pthread -lavcodec -ldl -lxcb -lxcb-xfixes -lxcb-render -lxcb-shape -lxcb -lxcb-shape -lxcb -lm -llzma -lz -pthread -lpostproc-ffmpeg -lswresample -lm -lswscale -lm -lavutil -lm")

endif(USE_FFMPEG)

#--------------------------------------------------------------------------------opencv
if(USE_OPENCV)
    if (MSVC)
        file(TO_CMAKE_PATH $ENV{OPENCV_ROOT} OpenCV_DIR)
        IF(NOT OpenCV_DIR)
            MESSAGE( FATAL_ERROR "Please point the environment variable OpenCV_ROOT to the root directory of OpenCV installation.")
        ENDIF()
        set(BUILD_SHARED_LIBS OFF)
        #set(SHARED_LIBS ON)
    else (MSVC)
        set(BUILD_SHARED_LIBS ON)
    endif(MSVC)
    find_package(OpenCV 3.0.0 COMPONENTS core highgui imgproc imgcodecs videoio photo stitching flann ml features2d calib3d objdetect REQUIRED)
endif(USE_OPENCV)

include_directories(include ${PROJECT_SOURCE_DIR})

# LINK_DIRECTORIES(${BOOST_LIBRARYDIR})                   
    
file(GLOB SOURCES "*.cpp")

#SET(HEADERS )

# add the executable
add_executable(camm_muxing "camm_muxing.cpp" ${HEADERS})
add_executable(camm_demuxing "camm_demuxing.cpp" ${HEADERS})

target_include_directories(camm_muxing PUBLIC ${OpenCV_INCLUDE_DIRS} ${Boost_INCLUDE_DIR} ${FFMPEG})
target_link_libraries(camm_muxing ${OpenCV_LIBS} ${Boost_LIBRARIES} ${FFMPEG} ${MYLIBS})

target_include_directories(camm_demuxing PUBLIC ${OpenCV_INCLUDE_DIRS} ${Boost_INCLUDE_DIR} ${FFMPEG})
target_link_libraries(camm_demuxing ${OpenCV_LIBS} ${Boost_LIBRARIES} ${FFMPEG} ${MYLIBS})
