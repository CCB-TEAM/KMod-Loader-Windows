#pragma once
// Just enough of zlib's inflate for reading .kmod (zip) packages: raw deflate streams (inflateInit2 with -15), one-shot Z_FINISH.
// The implementation is in win/inflate.cpp (a compact decoder; no zlib is bundled on Windows).
#include <compat.h>
#include <cstddef>

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;

#define Z_OK 0
#define Z_STREAM_END 1
#define Z_DATA_ERROR (-3)
#define Z_BUF_ERROR (-5)
#define Z_FINISH 4

struct z_stream
{
    Bytef* next_in;
    uInt avail_in;
    uLong total_in;
    Bytef* next_out;
    uInt avail_out;
    uLong total_out;
    void* state;
};

int inflateInit2(z_stream* s, int windowBits);
int inflate(z_stream* s, int flush);
int inflateEnd(z_stream* s);
