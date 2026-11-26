#include "pak_mods.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "kmod_common.h"
#include "kmod_log.h"

namespace
{
// ---------------------------------------------------------------------------------------------
// small file helpers
// ---------------------------------------------------------------------------------------------
bool Exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
bool DeleteIfThere(const std::string& p) { if (!Exists(p)) return true; unlink(p.c_str()); return !Exists(p); }
bool MoveOver(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    return kmod_rename(from.c_str(), to.c_str()) == 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}
void MakeDirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
}
bool EndsWithI(const std::string& s, const char* suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}
// A plain pak file name that is not one of the game's own.
bool PakFileName(const std::string& n)
{
    if (n.empty() || n[0] == '.' || n.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
    for (const char* game : {"kards-windows", "kards-android", "pakchunk", "global."})
        if (strncasecmp(n.c_str(), game, strlen(game)) == 0) return false;
    return EndsWithI(n, ".pak") || EndsWithI(n, ".utoc") || EndsWithI(n, ".ucas") || EndsWithI(n, ".sig");
}

// ---------------------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------------------------
struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;

    static uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Block(const uint8_t* p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++)
        {
            uint32_t t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void Add(const uint8_t* p, size_t n)
    {
        total += n;
        while (n)
        {
            if (!used && n >= 64) { Block(p); p += 64; n -= 64; continue; }
            size_t take = std::min(n, (size_t)64 - used);
            memcpy(buf + used, p, take); used += take; p += take; n -= take;
            if (used == 64) { Block(buf); used = 0; }
        }
    }
    std::string Hex()
    {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80, zero = 0, len[8];
        Add(&pad, 1);
        while (used != 56) Add(&zero, 1);
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        Add(len, 8);
        std::string s;
        char b[9];
        for (uint32_t v : h) { snprintf(b, sizeof(b), "%08x", (unsigned)v); s += b; }
        return s;
    }
};

// ---------------------------------------------------------------------------------------------
// Streaming raw-deflate decoder (RFC 1951): pulls compressed bytes from a file, hands out the result in 32 KB pieces.
// Pak entries can be hundreds of megabytes, so nothing here holds a whole entry in memory.
// ---------------------------------------------------------------------------------------------
using Sink = std::function<bool(const uint8_t*, size_t)>;

struct Inflater
{
    FILE* in;
    uint64_t inLeft;
    const Sink& sink;
    uint64_t limit;                       // the size the zip directory announced: never produce more
    uint8_t ib[1 << 15];
    size_t ipos = 0, ilen = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    uint8_t win[1 << 15];
    size_t wpos = 0;
    uint64_t produced = 0;
    bool bad = false;

    Inflater(FILE* f, uint64_t n, const Sink& s, uint64_t max) : in(f), inLeft(n), sink(s), limit(max) {}

    int Byte()
    {
        if (ipos == ilen)
        {
            size_t want = (size_t)std::min<uint64_t>(inLeft, sizeof(ib));
            ilen = want ? fread(ib, 1, want, in) : 0;
            ipos = 0;
            inLeft -= ilen;
            if (!ilen) { bad = true; return 0; }
        }
        return ib[ipos++];
    }
    int Bits(int need)
    {
        uint32_t v = bitBuf;
        while (bitCnt < need)
        {
            v |= (uint32_t)Byte() << bitCnt;
            if (bad) return 0;
            bitCnt += 8;
        }
        bitBuf = v >> need;
        bitCnt -= need;
        return (int)(v & ((1u << need) - 1));
    }
    bool Flush() { bool ok = !wpos || sink(win, wpos); wpos = 0; return ok; }
    bool Put(uint8_t c)
    {
        if (produced >= limit) return false;
        win[wpos++] = c;
        produced++;
        return wpos < sizeof(win) || Flush();
    }

    struct Huff { uint16_t count[16]; uint16_t symbol[288]; };
    static bool Build(Huff& h, const uint16_t* length, int n)
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
    int Decode(const Huff& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++)
        {
            code |= Bits(1);
            if (bad) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        return -1;
    }
    bool Codes(const Huff& lit, const Huff& dist)
    {
        static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint16_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint16_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;)
        {
            int sym = Decode(lit);
            if (sym < 0) return false;
            if (sym < 256) { if (!Put((uint8_t)sym)) return false; }
            else if (sym == 256) return true;
            else
            {
                sym -= 257;
                if (sym >= 29) return false;
                int len = lbase[sym] + Bits(lext[sym]);
                int ds = Decode(dist);
                if (ds < 0 || ds >= 30) return false;
                size_t d = dbase[ds] + Bits(dext[ds]);
                if (bad || d > produced || d > sizeof(win)) return false;
                while (len--) if (!Put(win[(wpos + sizeof(win) - d) % sizeof(win)])) return false;
            }
        }
    }
    bool Stored()
    {
        bitBuf = 0; bitCnt = 0;
        unsigned len = Byte(); len |= Byte() << 8;
        unsigned nlen = Byte(); nlen |= Byte() << 8;
        if (bad || len != (~nlen & 0xFFFF)) return false;
        while (len--) { int c = Byte(); if (bad || !Put((uint8_t)c)) return false; }
        return true;
    }
    bool Fixed()
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
        return Codes(lit, dist);
    }
    bool Dynamic()
    {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        uint16_t lengths[320] = {};
        for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint16_t)Bits(3);
        Huff lencode;
        if (!Build(lencode, lengths, 19)) return false;
        int idx = 0;
        uint16_t ll[320] = {};
        while (idx < nlen + ndist)
        {
            int sym = Decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) ll[idx++] = (uint16_t)sym;
            else
            {
                int len = 0, rep;
                if (sym == 16) { if (idx == 0) return false; len = ll[idx - 1]; rep = 3 + Bits(2); }
                else if (sym == 17) rep = 3 + Bits(3);
                else rep = 11 + Bits(7);
                if (idx + rep > nlen + ndist) return false;
                while (rep--) ll[idx++] = (uint16_t)len;
            }
        }
        Huff lit, dist;
        if (ll[256] == 0 || !Build(lit, ll, nlen) || !Build(dist, ll + nlen, ndist)) return false;
        return Codes(lit, dist);
    }
    bool Run()
    {
        int last;
        do
        {
            last = Bits(1);
            int type = Bits(2);
            if (bad) return false;
            bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
            if (!ok || bad) return false;
        } while (!last);
        return Flush() && produced == limit;
    }
};

// ---------------------------------------------------------------------------------------------
// zip (stored + deflate, no zip64), read straight from the file
// ---------------------------------------------------------------------------------------------
constexpr uint64_t kMaxZip = 1024ull << 20;           // a package with paks
constexpr uint64_t kMaxPakEntry = 512ull << 20;
constexpr uint64_t kMaxEntry = 32ull << 20;           // everything outside paks/
constexpr uint64_t kMaxTotal = 128ull << 20;

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

struct Entry { std::string name; uint16_t method; uint32_t csize, usize, header; };

struct Zip
{
    FILE* f = nullptr;
    uint64_t size = 0;
    std::vector<Entry> entries;
    ~Zip() { if (f) fclose(f); }

    bool ReadAt(uint64_t off, void* to, size_t n) { return off + n <= size && fseek(f, (long)off, SEEK_SET) == 0 && fread(to, 1, n, f) == n; }

    static bool SafeName(const std::string& n)
    {
        if (n.empty() || n[0] == '/' || n.find(':') != std::string::npos) return false;
        size_t i = 0;
        while (i <= n.size())
        {
            size_t j = n.find('/', i);
            if (j == std::string::npos) j = n.size();
            if (n.compare(i, j - i, "..") == 0) return false;     // no path traversal
            i = j + 1;
        }
        return true;
    }

    bool Open(const std::string& path, std::string& err)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) { err = "missing " + path; return false; }
        size = (uint64_t)st.st_size;
        if (size < 22 || size > kMaxZip) { err = "bad zip size"; return false; }
        f = fopen(path.c_str(), "rb");
        if (!f) { err = "cannot read " + path; return false; }
        std::vector<uint8_t> tail((size_t)std::min<uint64_t>(size, 65557));
        if (!ReadAt(size - tail.size(), tail.data(), tail.size())) { err = "cannot read " + path; return false; }
        size_t eocd = std::string::npos;
        for (size_t back = 22; back <= tail.size(); back++)
            if (U32(&tail[tail.size() - back]) == 0x06054b50) { eocd = tail.size() - back; break; }
        if (eocd == std::string::npos) { err = "not a zip file"; return false; }
        const uint16_t count = U16(&tail[eocd + 10]);
        const uint32_t dirSize = U32(&tail[eocd + 12]), dirOff = U32(&tail[eocd + 16]);
        if (dirSize > (8u << 20) || (uint64_t)dirOff + dirSize > size) { err = "corrupt central directory"; return false; }
        std::vector<uint8_t> dir(dirSize);
        if (dirSize && !ReadAt(dirOff, dir.data(), dirSize)) { err = "corrupt central directory"; return false; }
        size_t p = 0;
        for (uint16_t k = 0; k < count; k++)
        {
            if (p + 46 > dir.size() || U32(&dir[p]) != 0x02014b50) { err = "corrupt central directory"; return false; }
            Entry e;
            e.method = U16(&dir[p + 10]);
            e.csize = U32(&dir[p + 20]); e.usize = U32(&dir[p + 24]);
            const uint16_t nlen = U16(&dir[p + 28]), elen = U16(&dir[p + 30]), clen = U16(&dir[p + 32]);
            e.header = U32(&dir[p + 42]);
            if (p + 46 + nlen > dir.size()) { err = "corrupt entry name"; return false; }
            e.name.assign(reinterpret_cast<const char*>(&dir[p + 46]), nlen);
            p += 46 + (size_t)nlen + elen + clen;
            for (char& c : e.name) if (c == '\\') c = '/';
            if (!SafeName(e.name)) { err = "unsafe path in zip: " + e.name; return false; }
            entries.push_back(e);
        }
        return true;
    }

    bool Read(const Entry& e, const Sink& sink, std::string& err)
    {
        uint8_t lh[30];
        if (!ReadAt(e.header, lh, 30) || U32(lh) != 0x04034b50) { err = "corrupt local header"; return false; }
        const uint64_t data = (uint64_t)e.header + 30 + U16(lh + 26) + U16(lh + 28);
        if (data + e.csize > size || fseek(f, (long)data, SEEK_SET) != 0) { err = "truncated entry: " + e.name; return false; }
        if (e.method == 0)
        {
            if (e.csize != e.usize) { err = "corrupt entry: " + e.name; return false; }
            std::vector<uint8_t> b(1 << 16);
            for (uint64_t left = e.csize; left;)
            {
                size_t want = (size_t)std::min<uint64_t>(left, b.size());
                if (fread(b.data(), 1, want, f) != want || !sink(b.data(), want)) { err = "cannot extract " + e.name; return false; }
                left -= want;
            }
            return true;
        }
        if (e.method != 8) { err = "unsupported compression method in " + e.name; return false; }
        std::vector<Inflater> z;                       // 64 KB of buffers: not on the stack
        z.reserve(1);
        z.emplace_back(f, e.csize, sink, e.usize);
        if (!z[0].Run()) { err = "inflate failed (or storage full): " + e.name; return false; }
        return true;
    }
};

bool IsPakEntry(const std::string& n) { return strncasecmp(n.c_str(), "paks/", 5) == 0; }

// Writes one entry (or, without a destination, only reads it) and returns its checksum.
bool ExtractEntry(Zip& z, const Entry& e, const std::string& dest, std::string& sha, std::string& err)
{
    FILE* out = nullptr;
    if (!dest.empty())
    {
        MakeDirs(dest.substr(0, dest.rfind('/')));
        out = fopen(dest.c_str(), "wb");
        if (!out) { err = "cannot write " + dest; return false; }
    }
    Sha256 h;
    Sink sink = [&](const uint8_t* p, size_t n) { h.Add(p, n); return !out || fwrite(p, 1, n, out) == n; };
    bool ok = z.Read(e, sink, err);
    if (out && fclose(out) != 0 && ok) { ok = false; err = "cannot write (storage full?) " + dest; }
    if (!ok && out) unlink(dest.c_str());
    if (ok) sha = h.Hex();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Lua
// ---------------------------------------------------------------------------------------------
int Fail(lua_State* L, const std::string& err) { lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2; }

int L_ZipList(lua_State* L)          // kmod.zip_list(zip) -> {{name=, size=}, ...} | nil, err
{
    Zip z; std::string err;
    if (!z.Open(luaL_checkstring(L, 1), err)) return Fail(L, err);
    lua_newtable(L);
    int i = 0;
    for (const Entry& e : z.entries)
    {
        if (e.name.back() == '/') continue;
        lua_newtable(L);
        lua_pushlstring(L, e.name.data(), e.name.size()); lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)e.usize); lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// kmod.zip_extract(zip, entry [, dest]) -> sha256 | nil, err.   Only pak entries; dest must be a pak file name (optionally with
// ".tmp" / ".disable") directly inside the paks folder or the staged folder. Without dest the entry is only checked.
int L_ZipExtract(lua_State* L)
{
    std::string zip = luaL_checkstring(L, 1), name = luaL_checkstring(L, 2), dest = luaL_optstring(L, 3, "");
    if (!dest.empty())
    {
        size_t slash = dest.rfind('/');
        std::string dir = slash == std::string::npos ? "" : dest.substr(0, slash), file = dest.substr(slash + 1);
        for (const char* suffix : {".tmp", ".disable"})
            if (EndsWithI(file, suffix)) file.resize(file.size() - strlen(suffix));
        if ((dir != KPaksRoot() && dir != KPakStagedRoot()) || !PakFileName(file)) return Fail(L, "refusing destination " + dest);
    }
    Zip z; std::string err, sha;
    if (!z.Open(zip, err)) return Fail(L, err);
    for (const Entry& e : z.entries)
    {
        if (e.name != name) continue;
        if (!IsPakEntry(e.name)) return Fail(L, "not a pak entry: " + name);
        if (e.usize > kMaxPakEntry) return Fail(L, "entry too large: " + name);
        if (!ExtractEntry(z, e, dest, sha, err)) return Fail(L, err);
        lua_pushstring(L, sha.c_str());
        return 1;
    }
    return Fail(L, "no entry " + name);
}

int L_FileSha256(lua_State* L)       // kmod.file_sha256(path) -> sha256 | nil
{
    FILE* f = fopen(luaL_checkstring(L, 1), "rb");
    if (!f) return Fail(L, "cannot read");
    Sha256 h;
    std::vector<uint8_t> b(1 << 16);
    size_t n;
    while ((n = fread(b.data(), 1, b.size(), f)) > 0) h.Add(b.data(), n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return Fail(L, "read error");
    lua_pushstring(L, h.Hex().c_str());
    return 1;
}
}   // namespace

std::string KPaksRoot()
{
#ifdef _WIN32
    std::string p = KModRoot();                                   // <game>/kards/Binaries/Win64/kmod
    for (int i = 0; i < 3; i++) { size_t s = p.rfind('/'); if (s != std::string::npos) p.resize(s); }
    return p + "/Content/Paks";
#else
    return KModRoot() + "/paks";
#endif
}

std::string KPakStagedRoot() { return KModRoot() + "/pak-staged"; }

// Windows sets the pak load order by renaming (the engine takes a pak's priority from its name): X_P.pak becomes
// X_P.kmod_03_P.pak, sidecar files alike. Staged files carry the original name; they replace the file under its current
// name so it keeps its place. (Same rules as modloader.lua's pakLogical / pakTarget and the launcher's pakname.rs.)
static std::string StripOrderTag(std::string stem)
{
    for (;;)
    {
        if (stem.size() < 2 || stem.compare(stem.size() - 2, 2, "_P") != 0) return stem;
        size_t at = stem.rfind(".kmod_");
        if (at == std::string::npos || at == 0) return stem;
        const std::string n = stem.substr(at + 6, stem.size() - 2 - (at + 6));
        if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) return stem;
        stem.resize(at);
    }
}

static std::string LogicalPakName(const std::string& name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return name;
    return StripOrderTag(name.substr(0, dot)) + name.substr(dot);
}

static bool SameI(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
}

// names in the paks folder (without .disable) whose original name is `name`
static std::vector<std::string> PakOnDisk(const std::string& paks, const std::string& name)
{
    std::vector<std::string> out;
    if (DIR* d = opendir(paks.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (EndsWithI(n, ".disable")) n.resize(n.size() - 8);
            if (SameI(LogicalPakName(n), name) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

// the name `name` is written under: its renamed name, else the order tag of its .pak, else the name itself
static std::string PakTarget(const std::string& paks, const std::string& name)
{
    for (const std::string& n : PakOnDisk(paks, name)) if (n != name) return n;
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return name;
    const std::string stem = name.substr(0, dot);
    for (const std::string& n : PakOnDisk(paks, stem + ".pak"))
    {
        const std::string diskStem = n.substr(0, n.rfind('.'));
        if (diskStem.size() > stem.size() && StripOrderTag(diskStem) == stem) return diskStem + name.substr(dot);
    }
    return name;
}

void KMod_ApplyStagedPaks()
{
    const std::string staged = KPakStagedRoot(), paks = KPaksRoot();
    std::vector<std::string> names;
    if (DIR* d = opendir(staged.c_str()))
    {
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
    }
    if (names.empty()) return;
    MakeDirs(paks);
    // .pak files first: their state decides where new sidecar files go
    std::stable_partition(names.begin(), names.end(), [](const std::string& n) { return EndsWithI(n, ".pak"); });
    for (const std::string& n : names)
    {
        const std::string from = staged + "/" + n;
        if (EndsWithI(n, ".tmp")) { unlink(from.c_str()); continue; }              // an extraction that never finished
        if (EndsWithI(n, ".del"))
        {
            const std::string base = n.substr(0, n.size() - 4);
            if (!PakFileName(base)) { unlink(from.c_str()); continue; }
            bool a = true, b = true;
            std::vector<std::string> there = PakOnDisk(paks, base);
            if (there.empty()) there.push_back(base);
            for (const std::string& n : there) { a = DeleteIfThere(paks + "/" + n) && a; b = DeleteIfThere(paks + "/" + n + ".disable") && b; }
            if (a && b) { unlink(from.c_str()); KLog(KLOG_INFO, "core", "pak update: removed %s", base.c_str()); }
            else KLog(KLOG_WARN, "core", "pak update: could not remove %s (in use?); trying again next start", base.c_str());
            continue;
        }
        if (!PakFileName(n)) continue;
        const std::string disk = PakTarget(paks, n);
        std::string to = paks + "/" + disk;
        const std::string set = paks + "/" + disk.substr(0, disk.rfind('.')) + ".pak.disable";
        if (Exists(to + ".disable") || Exists(set)) { DeleteIfThere(to); to += ".disable"; }      // a pak the player switched off stays off
        if (MoveOver(from, to)) KLog(KLOG_INFO, "core", "pak update: installed %s", n.c_str());
        else KLog(KLOG_WARN, "core", "pak update: could not replace %s (in use?); trying again next start", n.c_str());
    }
}

bool KMod_UnzipPackage(const std::string& zipPath, const std::string& dest, std::string& err)
{
    Zip z;
    if (!z.Open(zipPath, err)) return false;
    uint64_t total = 0;
    for (const Entry& e : z.entries)
    {
        if (IsPakEntry(e.name)) continue;
        const std::string full = dest + "/" + e.name;
        if (e.name.back() == '/') { MakeDirs(full.substr(0, full.size() - 1)); continue; }
        if (e.usize > kMaxEntry) { err = "entry too large: " + e.name; return false; }
        total += e.usize;
        if (total > kMaxTotal) { err = "archive too large"; return false; }
        std::string sha;
        if (!ExtractEntry(z, e, full, sha, err)) return false;
    }
    return true;
}

void KModPak_Register(lua_State* L)
{
    lua_pushcfunction(L, L_ZipList); lua_setfield(L, -2, "zip_list");
    lua_pushcfunction(L, L_ZipExtract); lua_setfield(L, -2, "zip_extract");
    lua_pushcfunction(L, L_FileSha256); lua_setfield(L, -2, "file_sha256");
    MakeDirs(KPakStagedRoot());
    lua_getfield(L, -1, "paths");
    if (lua_istable(L, -1))
    {
        lua_pushstring(L, KPaksRoot().c_str()); lua_setfield(L, -2, "paks");
        lua_pushstring(L, KPakStagedRoot().c_str()); lua_setfield(L, -2, "pak_staged");
    }
    lua_pop(L, 1);
}
