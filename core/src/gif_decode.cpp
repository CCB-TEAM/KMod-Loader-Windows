#include "gif_decode.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace
{
struct Reader
{
    const uint8_t* d; size_t n, p = 0;
    bool ok(size_t k) const { return p + k <= n; }
    int u8() { return p < n ? d[p++] : -1; }
    int u16() { int a = u8(), b = u8(); return (a < 0 || b < 0) ? -1 : (a | (b << 8)); }
    void skip(size_t k) { p = std::min(n, p + k); }
    void skipSub() { for (;;) { int l = u8(); if (l <= 0) return; skip((size_t)l); } }
};

struct Frame
{
    int x = 0, y = 0, w = 0, h = 0;
    bool interlaced = false;
    std::vector<uint8_t> local;       // RGB palette, empty: use the global one
    std::vector<uint8_t> lzw;         // concatenated sub-blocks
    int minCode = 2;
    int delayCs = 10, disposal = 0, transparent = -1;
};

// the LZW stream of one image -> palette indices (w*h)
bool Lzw(const Frame& f, std::vector<uint8_t>& out)
{
    size_t total = (size_t)f.w * f.h;
    out.assign(total, 0);
    int minC = f.minCode;
    if (minC < 2 || minC > 11) return false;
    int clear = 1 << minC, eoi = clear + 1, next = clear + 2, size = minC + 1;
    std::vector<uint16_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), stack(4097);
    for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
    size_t pos = 0, bitPos = 0;
    int prev = -1;
    uint8_t first = 0;
    while (pos < total)
    {
        if ((bitPos + size + 7) / 8 > f.lzw.size() + 1) break;
        int code = 0;
        for (int b = 0; b < size; b++)
        {
            size_t bi = bitPos + b;
            size_t byte = bi >> 3;
            if (byte >= f.lzw.size()) { code = eoi; break; }
            code |= ((f.lzw[byte] >> (bi & 7)) & 1) << b;
        }
        bitPos += size;
        if (code == clear) { next = clear + 2; size = minC + 1; prev = -1; continue; }
        if (code == eoi) break;
        int cur = code;
        int sp = 0;
        if (prev == -1) { if (code >= clear) return false; out[pos++] = suffix[code]; prev = code; first = suffix[code]; continue; }
        if (code >= next)
        {
            if (code > next) return false;
            stack[sp++] = first; cur = prev;
        }
        while (cur >= clear) { if (sp >= 4096) return false; stack[sp++] = suffix[cur]; cur = prefix[cur]; }
        first = suffix[cur];
        stack[sp++] = first;
        while (sp > 0 && pos < total) out[pos++] = stack[--sp];
        if (next < 4096)
        {
            prefix[next] = (uint16_t)prev; suffix[next] = first; next++;
            if (next == (1 << size) && size < 12) size++;
        }
        prev = code;
    }
    return true;
}

uint32_t g_crc[256];
bool g_crcInit = false;
uint32_t Crc(uint32_t c, const uint8_t* p, size_t n)
{
    if (!g_crcInit) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; g_crc[i] = v; } g_crcInit = true; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = g_crc[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
void Be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    Be32(out, (uint32_t)data.size());
    size_t s = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Be32(out, Crc(0, out.data() + s, out.size() - s));
}
// RGBA -> PNG with stored (uncompressed) deflate blocks: big, but trivial and fast
bool WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba)
{
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 4 + 1));
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4); }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    while (pos < raw.size() || raw.empty())
    {
        size_t len = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(len & 255); z.push_back(len >> 8); z.push_back(~len & 255); z.push_back((~len >> 8) & 255);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
        if (last) break;
    }
    Be32(z, (b << 16) | a);
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    Be32(ihdr, w); Be32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}

// box filter shrink
void Shrink(const std::vector<uint8_t>& src, int sw, int sh, std::vector<uint8_t>& dst, int dw, int dh)
{
    dst.assign((size_t)dw * dh * 4, 0);
    for (int y = 0; y < dh; y++)
    {
        int y0 = (int)((int64_t)y * sh / dh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * sh / dh));
        for (int x = 0; x < dw; x++)
        {
            int x0 = (int)((int64_t)x * sw / dw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * sw / dw));
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++)
                {
                    const uint8_t* p = &src[((size_t)yy * sw + xx) * 4];
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; cnt++;
                }
            uint8_t* o = &dst[((size_t)y * dw + x) * 4];
            if (a > 0) { o[0] = (uint8_t)(r / a); o[1] = (uint8_t)(g / a); o[2] = (uint8_t)(b / a); }
            o[3] = cnt ? (uint8_t)(a / cnt) : 0;
        }
    }
}
}   // namespace

bool GifDecodeToPngs(const std::string& path, const std::string& outDir, int maxSide, int maxFrames, const int* crop, GifFrames& out, std::string& err)
{
    std::vector<uint8_t> data;
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { err = "cannot open the file"; return false; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz < 20 || sz > 200 * 1024 * 1024) { fclose(f); err = "bad file size"; return false; }
        data.resize((size_t)sz);
        size_t got = fread(data.data(), 1, data.size(), f);
        fclose(f);
        if (got != data.size()) { err = "cannot read the file"; return false; }
    }
    Reader r{ data.data(), data.size() };
    if (memcmp(data.data(), "GIF8", 4) != 0) { err = "not a GIF file"; return false; }
    r.p = 6;
    int W = r.u16(), H = r.u16();
    int flags = r.u8(); r.u8(); r.u8();
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) { err = "bad size"; return false; }
    std::vector<uint8_t> globalPal;
    if (flags & 0x80) { size_t cnt = (size_t)3 << ((flags & 7) + 1); if (!r.ok(cnt)) { err = "truncated"; return false; } globalPal.assign(data.begin() + r.p, data.begin() + r.p + cnt); r.skip(cnt); }

    // pass 1: read every block into frames (the compressed data stays small)
    std::vector<Frame> frames;
    Frame pending;
    bool haveGce = false;
    for (;;)
    {
        int b = r.u8();
        if (b < 0 || b == 0x3B) break;
        if (b == 0x21)
        {
            int label = r.u8();
            if (label == 0xF9)
            {
                int len = r.u8();
                if (len >= 4 && r.ok(len))
                {
                    int packed = r.u8(); int delay = r.u16(); int tr = r.u8();
                    pending = Frame();
                    pending.disposal = (packed >> 2) & 7; pending.delayCs = delay; pending.transparent = (packed & 1) ? tr : -1;
                    haveGce = true;
                    r.skip((size_t)len - 4);
                }
                else r.skip(len > 0 ? (size_t)len : 0);
                r.skipSub();
            }
            else r.skipSub();
        }
        else if (b == 0x2C)
        {
            Frame f = haveGce ? pending : Frame();
            haveGce = false;
            f.x = r.u16(); f.y = r.u16(); f.w = r.u16(); f.h = r.u16();
            int pk = r.u8();
            if (f.w <= 0 || f.h <= 0 || pk < 0) break;
            f.interlaced = (pk & 0x40) != 0;
            if (pk & 0x80) { size_t cnt = (size_t)3 << ((pk & 7) + 1); if (!r.ok(cnt)) break; f.local.assign(data.begin() + r.p, data.begin() + r.p + cnt); r.skip(cnt); }
            f.minCode = r.u8();
            for (;;) { int l = r.u8(); if (l <= 0) break; if (!r.ok(l)) break; f.lzw.insert(f.lzw.end(), data.begin() + r.p, data.begin() + r.p + l); r.skip(l); }
            frames.push_back(std::move(f));
            if (frames.size() > 5000) break;
        }
        else break;
    }
    if (frames.empty()) { err = "no frames"; return false; }

    int step = (int)((frames.size() + maxFrames - 1) / maxFrames);
    if (step < 1) step = 1;
    int CW = W, CH = H;                                    // the part of the picture that is kept
    if (crop && crop[2] >= 8) { CW = CH = std::min(crop[2], 8192); }
    double scale = std::min(1.0, (double)maxSide / std::max(CW, CH));
    int OW = std::max(1, (int)(CW * scale + 0.5)), OH = std::max(1, (int)(CH * scale + 0.5));
    out.width = OW; out.height = OH; out.delaysMs.clear();

    std::vector<uint8_t> canvas((size_t)W * H * 4, 0), backup, idx, shrunk, region;
    int prevDisposal = 0, px = 0, py = 0, pw = 0, ph = 0;
    int groupDelay = 0, written = 0;
    for (size_t i = 0; i < frames.size(); i++)
    {
        const Frame& f = frames[i];
        // undo the previous frame as its disposal says
        if (prevDisposal == 2) { for (int y = py; y < py + ph && y < H; y++) for (int x = px; x < px + pw && x < W; x++) memset(&canvas[((size_t)y * W + x) * 4], 0, 4); }
        else if (prevDisposal == 3 && !backup.empty()) canvas = backup;
        if (f.disposal == 3) backup = canvas; else backup.clear();
        const std::vector<uint8_t>& pal = f.local.empty() ? globalPal : f.local;
        if (!pal.empty() && Lzw(f, idx))
        {
            // interlaced rows are stored in four passes
            std::vector<int> rowOf(f.h);
            if (f.interlaced)
            {
                int k = 0;
                static const int start[4] = { 0, 4, 2, 1 }, inc[4] = { 8, 8, 4, 2 };
                for (int pass = 0; pass < 4; pass++) for (int y = start[pass]; y < f.h; y += inc[pass]) rowOf[k++] = y;
            }
            else for (int y = 0; y < f.h; y++) rowOf[y] = y;
            for (int sy = 0; sy < f.h; sy++)
            {
                int y = f.y + rowOf[sy];
                if (y < 0 || y >= H) continue;
                for (int sx = 0; sx < f.w; sx++)
                {
                    int x = f.x + sx;
                    if (x < 0 || x >= W) continue;
                    int c = idx[(size_t)sy * f.w + sx];
                    if (c == f.transparent) continue;
                    if ((size_t)c * 3 + 2 >= pal.size()) continue;
                    uint8_t* o = &canvas[((size_t)y * W + x) * 4];
                    o[0] = pal[c * 3]; o[1] = pal[c * 3 + 1]; o[2] = pal[c * 3 + 2]; o[3] = 255;
                }
            }
        }
        prevDisposal = f.disposal; px = f.x; py = f.y; pw = f.w; ph = f.h;
        int d = f.delayCs <= 1 ? 10 : f.delayCs;            // browsers treat 0 / 1 as 10 centiseconds
        groupDelay += d * 10;
        if ((int)i % step == step - 1 || i + 1 == frames.size())
        {
            char name[32];
            snprintf(name, sizeof name, "/f%03d.png", written + 1);
            const std::vector<uint8_t>* srcPix = &canvas;
            if (crop && crop[2] >= 8)
            {
                region.assign((size_t)CW * CH * 4, 0);
                for (int y = 0; y < CH; y++)
                {
                    int sy = crop[1] + y;
                    if (sy < 0 || sy >= H) continue;
                    for (int x = 0; x < CW; x++)
                    {
                        int sx = crop[0] + x;
                        if (sx < 0 || sx >= W) continue;
                        memcpy(&region[((size_t)y * CW + x) * 4], &canvas[((size_t)sy * W + sx) * 4], 4);
                    }
                }
                srcPix = &region;
            }
            if (OW == CW && OH == CH) { if (!WritePng(outDir + name, CW, CH, *srcPix)) { err = "cannot write a frame"; return false; } }
            else { Shrink(*srcPix, CW, CH, shrunk, OW, OH); if (!WritePng(outDir + name, OW, OH, shrunk)) { err = "cannot write a frame"; return false; } }
            out.delaysMs.push_back(groupDelay);
            groupDelay = 0; written++;
        }
    }
    return written > 0;
}
