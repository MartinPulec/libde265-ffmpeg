# libde265-ffmpeg

HEVC/H.265 codec for ffmpeg / libavcodec using libde265

## Building

    git clone https://github.com/FFmpeg/FFmpeg
    cd FFmpeg
    git am ../master-*.patch
    ./configure --enable-libde265
    make -j 10

## Dependencies
In addition to a compiler and the public ffmpeg/libavcodec headers,
a couple of other packages must be installed in order to compile the
codec:
- libde265-dev (>= 0.6)

Copyright (c) 2014-2015 struktur AG
