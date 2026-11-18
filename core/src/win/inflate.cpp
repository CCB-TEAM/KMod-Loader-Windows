// A compact raw-deflate decoder (RFC 1951) behind the few zlib entry points kmod uses for .kmod packages.
#include <zlib.h>

#include <cstdint>
#include <cstring>

namespace
{
struct Huff { uint16_t count[16]; uint16_t symbol[288]; };

struct State
{
    const uint8_t* in; size_t inLen, inPos;
    uint8_t* out; size_t outLen, outPos;
    uint32_t bitBuf; int bitCnt;
    bool bad;
};

int Bits(State& s, int need)
{
    uint32_t v = s.bitBuf;
    while (s.bitCnt < need)
    {
        if (s.inPos >= s.inLen) { s.bad = true; return 0; }
        v |= (uint32_t)s.in[s.inPos++] << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = v >> need;
    s.bitCnt -= need;
    return (int)(v & ((1u << need) - 1));
}

bool Build(Huff& h, const uint16_t* length, int n)
{
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[length[i]]++;
    if (h.count[0] == n) return true;
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h.count[len]; if (left < 0) return false; }
    uint16_t offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++) if (length[i]) h.symbol[offs[length[i]]++] = (uint16_t)i;
    return true;
}

int Decode(State& s, const Huff& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++)
    {
        code |= Bits(s, 1);
        if (s.bad) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

bool Codes(State& s, const Huff& lit, const Huff& dist)
{
    static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint16_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint16_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;)
    {
        int sym = Decode(s, lit);
        if (sym < 0) return false;
        if (sym < 256) { if (s.outPos >= s.outLen) return false; s.out[s.outPos++] = (uint8_t)sym; }
        else if (sym == 256) return true;
        else
        {
            sym -= 257;
            if (sym >= 29) return false;
            int len = lbase[sym] + Bits(s, lext[sym]);
            int ds = Decode(s, dist);
            if (ds < 0 || ds >= 30) return false;
            size_t d = dbase[ds] + Bits(s, dext[ds]);
            if (s.bad || d > s.outPos || s.outPos + len > s.outLen) return false;
            for (int i = 0; i < len; i++) { s.out[s.outPos] = s.out[s.outPos - d]; s.outPos++; }
        }
    }
}

bool Stored(State& s)
{
    s.bitBuf = 0; s.bitCnt = 0;
    if (s.inPos + 4 > s.inLen) return false;
    unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFF) || s.inPos + len > s.inLen || s.outPos + len > s.outLen) return false;
    memcpy(s.out + s.outPos, s.in + s.inPos, len);
    s.inPos += len; s.outPos += len;
    return true;
}

bool Fixed(State& s)
{
    Huff lit, dist; uint16_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    Build(lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    Build(dist, l, 30);
    return Codes(s, lit, dist);
}

bool Dynamic(State& s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = Bits(s, 5) + 257, ndist = Bits(s, 5) + 1, ncode = Bits(s, 4) + 4;
    if (s.bad || nlen > 286 || ndist > 30) return false;
    uint16_t lengths[320] = {};
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint16_t)Bits(s, 3);
    Huff lencode;
    if (!Build(lencode, lengths, 19)) return false;
    int idx = 0;
    uint16_t ll[320] = {};
    while (idx < nlen + ndist)
    {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) ll[idx++] = (uint16_t)sym;
        else
        {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return false; len = ll[idx - 1]; rep = 3 + Bits(s, 2); }
            else if (sym == 17) rep = 3 + Bits(s, 3);
            else rep = 11 + Bits(s, 7);
            if (idx + rep > nlen + ndist) return false;
            while (rep--) ll[idx++] = (uint16_t)len;
        }
    }
    Huff lit, dist;
    if (ll[256] == 0 || !Build(lit, ll, nlen) || !Build(dist, ll + nlen, ndist)) return false;
    return Codes(s, lit, dist);
}
}

int inflateInit2(z_stream* z, int) { z->total_out = 0; z->total_in = 0; z->state = nullptr; return Z_OK; }
int inflateEnd(z_stream*) { return Z_OK; }

int inflate(z_stream* z, int)
{
    State s = {z->next_in, z->avail_in, 0, z->next_out, z->avail_out, 0, 0, 0, false};
    int last;
    do
    {
        last = Bits(s, 1);
        int type = Bits(s, 2);
        if (s.bad) return Z_DATA_ERROR;
        bool ok = type == 0 ? Stored(s) : type == 1 ? Fixed(s) : type == 2 ? Dynamic(s) : false;
        if (!ok || s.bad) return Z_DATA_ERROR;
    } while (!last);
    z->total_out = s.outPos; z->total_in = s.inPos;
    return Z_STREAM_END;
}
