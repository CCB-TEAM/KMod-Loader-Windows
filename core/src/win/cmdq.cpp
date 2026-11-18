#include "cmdq.h"

#include <cstring>
#include <deque>
#include <mutex>

namespace
{
std::mutex g_cmdMu, g_logMu;
std::deque<std::string> g_cmds;
std::deque<LogLine> g_ring;
unsigned long long g_seq = 0;
constexpr size_t kRing = 1500;
}

void CmdQueue_Push(const std::string& line)
{
    std::lock_guard<std::mutex> g(g_cmdMu);
    if (g_cmds.size() < 256) g_cmds.push_back(line);
}

int CmdQueue_Pop(char* buf, int cap)
{
    std::lock_guard<std::mutex> g(g_cmdMu);
    if (g_cmds.empty()) return -1;
    std::string s = g_cmds.front();
    g_cmds.pop_front();
    int n = (int)std::min<size_t>(s.size(), (size_t)cap - 1);
    memcpy(buf, s.data(), n);
    buf[n] = 0;
    return n;
}

void LogRing_Add(int level, const std::string& text)
{
    std::lock_guard<std::mutex> g(g_logMu);
    g_ring.push_back({level, ++g_seq, text});
    if (g_ring.size() > kRing) g_ring.pop_front();
}

std::vector<LogLine> LogRing_Snapshot()
{
    std::lock_guard<std::mutex> g(g_logMu);
    return std::vector<LogLine>(g_ring.begin(), g_ring.end());
}
