#include "net_conn.h"

#define SECURITY_WIN32
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
// ---------------------------------------------------------------------------------------------
// base64 + SHA-1 (only for the WebSocket handshake)
// ---------------------------------------------------------------------------------------------
std::string B64(const uint8_t* d, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
        o += i + 1 < n ? t[v >> 6 & 63] : '='; o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

void Sha1(const std::string& in, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = in;
    uint64_t bits = (uint64_t)in.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (i * 8));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t o = 0; o < m.size(); o += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint8_t)m[o + i * 4] << 24 | (uint8_t)m[o + i * 4 + 1] << 16 | (uint8_t)m[o + i * 4 + 2] << 8 | (uint8_t)m[o + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - j * 8));
}

// ---------------------------------------------------------------------------------------------
// Transport: plain socket (Winsock) or TLS (SChannel)
// ---------------------------------------------------------------------------------------------
struct Transport
{
    virtual ~Transport() {}
    virtual bool Connect(const std::string& host, int port, int timeoutMs, std::string& err) = 0;
    virtual int Read(uint8_t* buf, int n, int timeoutMs) = 0;       // >0 bytes, 0 timeout, -1 closed/error
    virtual bool Write(const uint8_t* buf, size_t n) = 0;
    virtual void Close() = 0;
};

struct PlainTransport : Transport
{
    SOCKET fd = INVALID_SOCKET;
    bool udp = false;
    bool Connect(const std::string& host, int port, int timeoutMs, std::string& err) override
    {
        static std::once_flag once;
        std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
        ADDRINFOA hints = {}, *res = nullptr;
        hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (rc != 0 || !res) { err = std::string("cannot resolve ") + host + " (error " + std::to_string(rc) + ")"; return false; }
        int lastErr = 0;
        for (ADDRINFOA* a = res; a; a = a->ai_next)
        {
            SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
            int c = connect(s, a->ai_addr, (int)a->ai_addrlen);
            if (c != 0 && WSAGetLastError() == WSAEWOULDBLOCK)
            {
                WSAPOLLFD p = {s, POLLWRNORM, 0};
                if (WSAPoll(&p, 1, timeoutMs) == 1 && !(p.revents & (POLLERR | POLLHUP)))
                {
                    int e = 0; int l = sizeof(e);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &l);
                    c = e == 0 ? 0 : -1; lastErr = e;
                }
                else { c = -1; lastErr = WSAETIMEDOUT; }
            }
            else if (c != 0) lastErr = WSAGetLastError();
            if (c == 0) { fd = s; int one = 1; if (!udp) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one)); freeaddrinfo(res); return true; }
            closesocket(s);
        }
        freeaddrinfo(res);
        err = std::string("cannot connect to ") + host + ":" + std::to_string(port) + " (Winsock error " + std::to_string(lastErr) + ")";
        return false;
    }
    int Read(uint8_t* buf, int n, int timeoutMs) override
    {
        WSAPOLLFD p = {fd, POLLRDNORM, 0};
        int r = WSAPoll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return -1;
        int got = recv(fd, reinterpret_cast<char*>(buf), n, 0);
        if (got > 0) return got;
        if (got == 0) return udp ? 0 : -1;
        int e = WSAGetLastError();
        return (e == WSAEWOULDBLOCK || e == WSAEINTR || (udp && e == WSAECONNRESET)) ? 0 : -1;
    }
    bool Write(const uint8_t* buf, size_t n) override
    {
        size_t off = 0;
        while (off < n)
        {
            int w = send(fd, reinterpret_cast<const char*>(buf + off), (int)std::min<size_t>(n - off, 1u << 20), 0);
            if (w > 0) { off += w; continue; }
            if (w < 0 && WSAGetLastError() == WSAEWOULDBLOCK) { WSAPOLLFD p = {fd, POLLWRNORM, 0}; WSAPoll(&p, 1, 1000); continue; }
            return false;
        }
        return true;
    }
    void Close() override { if (fd != INVALID_SOCKET) { shutdown(fd, SD_BOTH); closesocket(fd); fd = INVALID_SOCKET; } }
};

// TLS through the operating system (SChannel): the certificate chain and the host name are checked by the system.
struct TlsTransport : Transport
{
    PlainTransport sock;
    CredHandle cred = {};
    CtxtHandle ctx = {};
    bool haveCred = false, haveCtx = false;
    SecPkgContext_StreamSizes sizes = {};
    std::string enc, plain;                       // received ciphertext not yet decrypted / plaintext not yet returned

    bool Connect(const std::string& host, int port, int timeoutMs, std::string& err) override
    {
        if (!sock.Connect(host, port, timeoutMs, err)) return false;
        SCHANNEL_CRED sc = {};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
        TimeStamp ts;
        if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &cred, &ts) != SEC_E_OK)
        { err = "TLS: cannot acquire credentials"; return false; }
        haveCred = true;
        std::wstring whost = KWide(host);
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        DWORD attr = 0;
        SecBuffer outB = {0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outD = {SECBUFFER_VERSION, 1, &outB};
        SECURITY_STATUS st = InitializeSecurityContextW(&cred, nullptr, const_cast<wchar_t*>(whost.c_str()), flags, 0, 0, nullptr, 0, &ctx, &outD, &attr, &ts);
        haveCtx = st == SEC_I_CONTINUE_NEEDED;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (outB.pvBuffer)
            {
                bool w = outB.cbBuffer == 0 || sock.Write(static_cast<uint8_t*>(outB.pvBuffer), outB.cbBuffer);
                FreeContextBuffer(outB.pvBuffer); outB.pvBuffer = nullptr; outB.cbBuffer = 0;
                if (!w) { err = "TLS: connection lost during the handshake"; return false; }
            }
            if (st == SEC_I_CONTINUE_NEEDED && enc.empty() || st == SEC_E_INCOMPLETE_MESSAGE)
            {
                if (std::chrono::steady_clock::now() > deadline) { err = "TLS handshake with " + host + " timed out"; return false; }
                uint8_t tmp[16384];
                int n = sock.Read(tmp, sizeof(tmp), 200);
                if (n < 0) { err = "TLS: connection closed during the handshake"; return false; }
                enc.append(reinterpret_cast<char*>(tmp), n);
                if (n == 0 && enc.empty()) continue;
            }
            SecBuffer inB[2] = {{(ULONG)enc.size(), SECBUFFER_TOKEN, &enc[0]}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc inD = {SECBUFFER_VERSION, 2, inB};
            outB = {0, SECBUFFER_TOKEN, nullptr};
            st = InitializeSecurityContextW(&cred, &ctx, const_cast<wchar_t*>(whost.c_str()), flags, 0, 0, &inD, 0, nullptr, &outD, &attr, &ts);
            if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
            if (inB[1].BufferType == SECBUFFER_EXTRA) enc.erase(0, enc.size() - inB[1].cbBuffer); else enc.clear();
            if (st == SEC_E_OK && outB.pvBuffer)
            {
                bool w = sock.Write(static_cast<uint8_t*>(outB.pvBuffer), outB.cbBuffer);
                FreeContextBuffer(outB.pvBuffer); outB.pvBuffer = nullptr;
                if (!w) { err = "TLS: connection lost during the handshake"; return false; }
            }
        }
        if (outB.pvBuffer) FreeContextBuffer(outB.pvBuffer);
        if (st != SEC_E_OK)
        {
            char b[96]; snprintf(b, sizeof(b), "TLS handshake with %s failed (0x%08lx)", host.c_str(), (unsigned long)st);
            err = b;
            return false;
        }
        haveCtx = true;
        QueryContextAttributes(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
        return true;
    }
    int Read(uint8_t* buf, int n, int timeoutMs) override
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            if (!plain.empty())
            {
                int k = (int)std::min<size_t>(plain.size(), (size_t)n);
                memcpy(buf, plain.data(), k);
                plain.erase(0, k);
                return k;
            }
            if (!enc.empty())
            {
                SecBuffer b[4] = {{(ULONG)enc.size(), SECBUFFER_DATA, &enc[0]}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
                SECURITY_STATUS st = DecryptMessage(&ctx, &d, 0, nullptr);
                if (st == SEC_E_OK)
                {
                    std::string extra;
                    for (int i = 0; i < 4; i++)
                    {
                        if (b[i].BufferType == SECBUFFER_DATA) plain.append(static_cast<char*>(b[i].pvBuffer), b[i].cbBuffer);
                        if (b[i].BufferType == SECBUFFER_EXTRA) extra.assign(static_cast<char*>(b[i].pvBuffer), b[i].cbBuffer);
                    }
                    enc = extra;
                    continue;
                }
                if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;       // closed (SEC_I_CONTEXT_EXPIRED), renegotiation or a broken stream
            }
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left < 0) left = 0;
            uint8_t tmp[16384];
            int got = sock.Read(tmp, sizeof(tmp), left);
            if (got < 0) return -1;
            if (got == 0) return 0;
            enc.append(reinterpret_cast<char*>(tmp), got);
        }
    }
    bool Write(const uint8_t* data, size_t n) override
    {
        std::vector<uint8_t> msg;
        for (size_t off = 0; off < n;)
        {
            size_t k = std::min<size_t>(n - off, sizes.cbMaximumMessage);
            msg.resize(sizes.cbHeader + k + sizes.cbTrailer);
            memcpy(&msg[sizes.cbHeader], data + off, k);
            SecBuffer b[4] = {{sizes.cbHeader, SECBUFFER_STREAM_HEADER, msg.data()}, {(ULONG)k, SECBUFFER_DATA, &msg[sizes.cbHeader]},
                              {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, &msg[sizes.cbHeader + k]}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
            if (EncryptMessage(&ctx, 0, &d, 0) != SEC_E_OK) return false;
            if (!sock.Write(msg.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
            off += k;
        }
        return true;
    }
    void Close() override
    {
        sock.Close();
        if (haveCtx) { DeleteSecurityContext(&ctx); haveCtx = false; }
        if (haveCred) { FreeCredentialsHandle(&cred); haveCred = false; }
    }
};

// ---------------------------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------------------------
struct Event { std::string type, data, extra; bool binary = false; };
struct Outgoing { int opcode; std::string data; };

struct Conn
{
    int id = 0;
    std::string owner, kind, host, path = "/", scheme;
    int port = 0;
    bool tls = false;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 15000;
    std::atomic<bool> stop{false}, done{false};
    std::mutex mu;
    std::deque<Event> events;       // to the game thread
    std::deque<Outgoing> out;       // from the game thread
    std::thread th;
};

WorkerHooks g_hooks{};
std::vector<std::shared_ptr<Conn>> g_conns;        // game thread only
int g_nextId = 1;
const size_t kMaxMessage = 16u << 20;
const size_t kMaxQueue = 4096;

void Push(Conn* c, Event e)
{
    std::lock_guard<std::mutex> g(c->mu);
    if (c->events.size() < kMaxQueue) c->events.push_back(std::move(e));
}

std::string Frame(int opcode, const std::string& payload)      // client frames are masked
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::string f;
    f += (char)(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) f += (char)(0x80 | n);
    else if (n < 65536) { f += (char)(0x80 | 126); f += (char)(n >> 8); f += (char)n; }
    else { f += (char)(0x80 | 127); for (int i = 7; i >= 0; i--) f += (char)((uint64_t)n >> (i * 8)); }
    uint8_t key[4];
    for (auto& k : key) k = (uint8_t)rng();
    f.append(reinterpret_cast<char*>(key), 4);
    for (size_t i = 0; i < n; i++) f += (char)(payload[i] ^ key[i & 3]);
    return f;
}

bool ParseUrl(const std::string& url, Conn* c, std::string& err)
{
    size_t p = url.find("://");
    if (p == std::string::npos) { err = "bad url"; return false; }
    c->scheme = url.substr(0, p);
    if (c->scheme != "ws" && c->scheme != "wss") { err = "WebSocket urls start with ws:// or wss://"; return false; }
    c->tls = c->scheme == "wss";
    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    c->path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) { c->host = hostport.substr(0, colon); c->port = atoi(hostport.c_str() + colon + 1); }
    else { c->host = hostport; c->port = c->tls ? 443 : 80; }
    if (c->host.empty()) { err = "bad url"; return false; }
    return true;
}

void RunConn(std::shared_ptr<Conn> c)
{
    std::unique_ptr<Transport> t;
    std::string err;
    if (c->tls) t.reset(new TlsTransport());
    else { auto* pt = new PlainTransport(); pt->udp = c->kind == "udp"; t.reset(pt); }

    auto fail = [&](const std::string& m) { Push(c.get(), {"error", m, ""}); };
    std::string rbuf;                                  // received bytes not yet parsed (ws handshake / frames)
    bool ok = t->Connect(c->host, c->port, c->timeoutMs, err);
    if (!ok) fail(err);
    uint8_t chunk[16384];

    // WebSocket handshake
    if (ok && c->kind == "ws")
    {
        std::mt19937 rng{std::random_device{}()};
        uint8_t kb[16];
        for (auto& b : kb) b = (uint8_t)rng();
        std::string key = B64(kb, 16);
        std::string req = "GET " + c->path + " HTTP/1.1\r\nHost: " + c->host + ((c->tls && c->port == 443) || (!c->tls && c->port == 80) ? "" : ":" + std::to_string(c->port)) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
        for (auto& h : c->headers) req += h.first + ": " + h.second + "\r\n";
        req += "\r\n";
        ok = t->Write(reinterpret_cast<const uint8_t*>(req.data()), req.size());
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c->timeoutMs);
        size_t end = std::string::npos;
        while (ok && (end = rbuf.find("\r\n\r\n")) == std::string::npos)
        {
            if (std::chrono::steady_clock::now() > deadline || rbuf.size() > 65536 || c->stop) { ok = false; fail("WebSocket handshake timed out"); break; }
            int n = t->Read(chunk, sizeof(chunk), 200);
            if (n < 0) { ok = false; fail("connection closed during the WebSocket handshake"); break; }
            rbuf.append(reinterpret_cast<char*>(chunk), n);
        }
        if (ok)
        {
            std::string head = rbuf.substr(0, end);
            rbuf.erase(0, end + 4);
            uint8_t sha[20];
            Sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", sha);
            std::string accept = B64(sha, 20);
            std::string lower = head;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            if (head.find(" 101") == std::string::npos) { ok = false; fail("the server did not switch protocols: " + head.substr(0, head.find("\r\n"))); }
            else if (lower.find("sec-websocket-accept: " + [&] { std::string a = accept; for (auto& ch : a) ch = (char)tolower((unsigned char)ch); return a; }()) == std::string::npos)
            { ok = false; fail("bad Sec-WebSocket-Accept from the server"); }
        }
    }
    if (ok) Push(c.get(), {"open", "", ""});

    // main loop
    std::string message;           // WebSocket message being assembled
    int msgOpcode = 0;
    int closeCode = 1006;
    std::string closeReason;
    bool sentClose = false;
    while (ok && !c->stop)
    {
        // outgoing
        std::deque<Outgoing> out;
        { std::lock_guard<std::mutex> g(c->mu); out.swap(c->out); }
        for (auto& o : out)
        {
            std::string bytes = c->kind == "ws" ? Frame(o.opcode, o.data) : o.data;
            if (!t->Write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())) { fail("send failed"); ok = false; break; }
            if (c->kind == "ws" && o.opcode == 0x8) { sentClose = true; }
        }
        if (!ok) break;
        int n = t->Read(chunk, sizeof(chunk), 100);
        if (n < 0) { if (c->kind == "ws" && !sentClose) closeCode = 1006; ok = false; break; }
        if (n == 0) continue;
        if (c->kind != "ws") { Push(c.get(), {"data", std::string(reinterpret_cast<char*>(chunk), n), "", true}); continue; }
        rbuf.append(reinterpret_cast<char*>(chunk), n);
        // parse frames
        for (;;)
        {
            if (rbuf.size() < 2) break;
            uint8_t b0 = rbuf[0], b1 = rbuf[1];
            bool fin = b0 & 0x80;
            int op = b0 & 0x0f;
            bool masked = b1 & 0x80;
            uint64_t len = b1 & 0x7f;
            size_t hdr = 2;
            if (len == 126) { if (rbuf.size() < 4) break; len = (uint8_t)rbuf[2] << 8 | (uint8_t)rbuf[3]; hdr = 4; }
            else if (len == 127) { if (rbuf.size() < 10) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | (uint8_t)rbuf[2 + i]; hdr = 10; }
            if (len > kMaxMessage) { fail("message too large"); ok = false; break; }
            size_t mk = masked ? 4 : 0;
            if (rbuf.size() < hdr + mk + len) break;
            std::string payload = rbuf.substr(hdr + mk, len);
            if (masked) for (size_t i = 0; i < len; i++) payload[i] ^= rbuf[hdr + (i & 3)];
            rbuf.erase(0, hdr + mk + len);
            if (op == 0x9) { std::lock_guard<std::mutex> g(c->mu); c->out.push_back({0xA, payload}); }                    // ping -> pong
            else if (op == 0x8)
            {
                closeCode = payload.size() >= 2 ? ((uint8_t)payload[0] << 8 | (uint8_t)payload[1]) : 1005;
                closeReason = payload.size() > 2 ? payload.substr(2) : "";
                if (!sentClose) { std::string echo = Frame(0x8, payload.substr(0, 2)); t->Write(reinterpret_cast<const uint8_t*>(echo.data()), echo.size()); }
                ok = false;
                break;
            }
            else if (op == 0x1 || op == 0x2 || op == 0x0)
            {
                if (op != 0x0) { msgOpcode = op; message.clear(); }
                message += payload;
                if (message.size() > kMaxMessage) { fail("message too large"); ok = false; break; }
                if (fin) { Push(c.get(), {"message", message, "", msgOpcode == 0x2}); message.clear(); }
            }
        }
    }
    if (c->stop && c->kind == "ws" && !sentClose)           // a clean close started by the mod
    {
        std::string f = Frame(0x8, std::string("\x03\xe8", 2));
        t->Write(reinterpret_cast<const uint8_t*>(f.data()), f.size());
        closeCode = 1000;
    }
    t->Close();
    Push(c.get(), {"close", closeReason, std::to_string(closeCode)});
    c->done = true;
}

void Shutdown(const std::shared_ptr<Conn>& c)
{
    c->stop = true;
    for (int i = 0; i < 200 && !c->done.load(); i++) usleep(5000);
    if (c->th.joinable()) { if (c->done.load()) c->th.join(); else c->th.detach(); }
}

std::shared_ptr<Conn> Find(int id) { for (auto& c : g_conns) if (c->id == id) return c; return nullptr; }
}

void NetConn_Init(const WorkerHooks& hooks) { g_hooks = hooks; }

// kmod.net_open(owner, kind, { url=, host=, port=, tls=, headers={}, timeout= }) -> id
int NetConn_Open(lua_State* L)
{
    auto c = std::make_shared<Conn>();
    c->owner = luaL_checkstring(L, 1);
    c->kind = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    if (c->kind != "tcp" && c->kind != "udp" && c->kind != "ws") return luaL_error(L, "net: unknown connection kind '%s'", c->kind.c_str());
    std::string err;
    lua_getfield(L, 3, "timeout"); if (lua_isnumber(L, -1)) { double t = lua_tonumber(L, -1); c->timeoutMs = (int)(t < 1000 ? 1000 : (t > 60000 ? 60000 : t)); } lua_pop(L, 1);
    if (c->kind == "ws")
    {
        lua_getfield(L, 3, "url");
        std::string url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!ParseUrl(url, c.get(), err)) return luaL_error(L, "WebSocket: %s", err.c_str());
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) { lua_pushnil(L); while (lua_next(L, -2)) { if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) c->headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1)); lua_pop(L, 1); } }
        lua_pop(L, 1);
    }
    else
    {
        lua_getfield(L, 3, "host"); if (lua_isstring(L, -1)) c->host = lua_tostring(L, -1); lua_pop(L, 1);
        lua_getfield(L, 3, "port"); if (lua_isnumber(L, -1)) c->port = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, 3, "tls"); c->tls = lua_toboolean(L, -1) != 0 && c->kind == "tcp"; lua_pop(L, 1);
        if (c->host.empty() || c->port <= 0 || c->port > 65535) return luaL_error(L, "net: host and port are required");
    }
    if (g_conns.size() >= 16) return luaL_error(L, "net: too many connections (16 at most)");
    c->id = g_nextId++;
    g_conns.push_back(c);
    c->th = std::thread(RunConn, c);
    lua_pushinteger(L, c->id);
    return 1;
}

int NetConn_Send(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    size_t n; const char* d = luaL_checklstring(L, 2, &n);
    bool binary = lua_toboolean(L, 3) != 0;
    auto c = Find(id);
    if (!c || c->stop.load() || c->done.load()) { lua_pushboolean(L, 0); return 1; }
    {
        std::lock_guard<std::mutex> g(c->mu);
        if (c->out.size() >= kMaxQueue) { lua_pushboolean(L, 0); return 1; }
        c->out.push_back({c->kind == "ws" ? (binary ? 0x2 : 0x1) : 0, std::string(d, n)});
    }
    lua_pushboolean(L, 1);
    return 1;
}

int NetConn_Close(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    if (auto c = Find(id)) c->stop = true;          // the thread closes cleanly; Pump removes it after the close event
    return 0;
}

void NetConn_Pump(lua_State* L)
{
    if (g_conns.empty()) return;
    std::vector<std::shared_ptr<Conn>> list = g_conns;
    for (auto& c : list)
    {
        std::deque<Event> batch;
        {
            std::lock_guard<std::mutex> g(c->mu);
            size_t n = std::min<size_t>(c->events.size(), 256);
            for (size_t i = 0; i < n; i++) { batch.push_back(std::move(c->events.front())); c->events.pop_front(); }
        }
        for (auto& e : batch)
        {
            lua_getglobal(L, "kmod");
            lua_getfield(L, -1, "_net_event");
            lua_remove(L, -2);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
            lua_pushinteger(L, c->id);
            lua_pushstring(L, e.type.c_str());
            lua_pushlstring(L, e.data.data(), e.data.size());
            lua_pushstring(L, e.extra.c_str());
            lua_pushboolean(L, e.binary);
            g_hooks.pcall(L, 5, 0, "network event", c->owner);
        }
    }
    for (size_t i = 0; i < g_conns.size();)       // forget finished connections whose events were all delivered
    {
        auto& c = g_conns[i];
        bool empty;
        { std::lock_guard<std::mutex> g(c->mu); empty = c->events.empty(); }
        if (c->done.load() && empty) { Shutdown(c); g_conns.erase(g_conns.begin() + i); }
        else i++;
    }
}

void NetConn_CloseUser(lua_State*)
{
    for (size_t i = 0; i < g_conns.size();)
    {
        if (!g_conns[i]->owner.empty()) { g_conns[i]->events.clear(); Shutdown(g_conns[i]); g_conns.erase(g_conns.begin() + i); }
        else i++;
    }
}
