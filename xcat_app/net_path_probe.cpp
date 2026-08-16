#include <winsock2.h>
#include <ws2tcpip.h>

#include "net_path_probe.h"

#include "process_util.h"
#include "xcat_log.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace xcat::app::net_path {
namespace {

constexpr wchar_t kClassicExe[] = L"Maplestory_Classic.exe";
constexpr ULONGLONG kTickIntervalMs = 8000;
// 路径没变也定期落一行，给断线时刻留对齐锚点（断线记在 kick.log，两边按时间戳对）。
constexpr ULONGLONG kHeartbeatMs = 120000;
// 兜底闸门：三项都是「宁可少观测，也不能拖累面板」。
constexpr size_t kMaxTableBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxLogLines = 8;
constexpr int kMaxFaults = 3;

struct NicInfo {
    std::string label;
    bool tunnelish = false;
};

struct Conn {
    DWORD pid = 0;
    ULONG localAddr = 0;
    ULONG remoteAddr = 0;
    uint16_t localPort = 0;
    uint16_t remotePort = 0;
};

bool HasLower(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle) return false;
    std::wstring h(hay);
    for (auto& c : h) c = static_cast<wchar_t>(towlower(c));
    return h.find(needle) != std::wstring::npos;
}

// 加速器普遍以 TUN/TAP 虚拟网卡接管路由：此时对端仍是真实台服 IP，只有本地绑定
// 这一侧能看出来，所以判据放在网卡上而不是对端地址上。
// 本机实测命中样本：`Netease UU TAP-Win32 Adapter V9.21`（tap-）、`Meta Tunnel`（tun）。
bool LooksTunnelNic(const IP_ADAPTER_ADDRESSES* a) {
    if (!a) return false;
    if (a->IfType == IF_TYPE_TUNNEL) return true;
    for (const wchar_t* kw : {L"tap-", L"tap ", L"tun", L"vpn", L"virtual", L"加速", L"accel",
                              L"wintun", L"openvpn", L"wireguard", L"proxy"}) {
        if (HasLower(a->Description, kw) || HasLower(a->FriendlyName, kw)) return true;
    }
    return false;
}

bool IsLoopback(ULONG netOrder) {
    return reinterpret_cast<const unsigned char*>(&netOrder)[0] == 127;
}

// 不含环回：环回另判。对端落在这些段说明流量被本地/内网转发接走。
bool IsPrivateOrFakeIp(ULONG netOrder) {
    const auto* b = reinterpret_cast<const unsigned char*>(&netOrder);
    if (b[0] == 10) return true;
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    // 198.18.0.0/15：Clash / mihomo 的 fake-ip 段，本机 Meta Tunnel 网卡即用此段。
    if (b[0] == 198 && (b[1] == 18 || b[1] == 19)) return true;
    return false;
}

std::string FormatV4(ULONG netOrder) {
    const auto* b = reinterpret_cast<const unsigned char*>(&netOrder);
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

uint16_t PortFromRow(DWORD raw) {
    // MIB 行里端口是网络字节序，且只占低 16 位。
    return static_cast<uint16_t>(((raw >> 8) & 0xFF) | ((raw & 0xFF) << 8));
}

// 多开也要覆盖：每行都带 pid，漏掉第二个实例会让取证结论片面。
std::vector<DWORD> CollectClassicPids() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, kClassicExe) == 0) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 每次都重新枚举：加速器可能在会话中途才开，虚拟网卡是那时才出现的。
// 不跳过 down 的网卡——UU 的 TAP 在实测里状态是 Disconnected 但仍持有地址。
std::vector<std::pair<ULONG, NicInfo>> CollectLocalV4Nics() {
    std::vector<std::pair<ULONG, NicInfo>> out;
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return out;
    }
    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != ERROR_SUCCESS) return out;

    for (auto* a = addrs; a; a = a->Next) {
        NicInfo info{};
        info.label = a->FriendlyName ? xcat::WideToUtf8(a->FriendlyName) : std::string("?");
        info.tunnelish = LooksTunnelNic(a);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            out.emplace_back(sin->sin_addr.S_un.S_addr, info);
        }
    }
    return out;
}

const NicInfo* FindNic(const std::vector<std::pair<ULONG, NicInfo>>& nics, ULONG localAddr) {
    for (const auto& kv : nics) {
        if (kv.first == localAddr) return &kv.second;
    }
    return nullptr;
}

bool ReadTcpTable(std::vector<char>& buf) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    // 上限只为挡住异常返回值——真实 TCP 表远小于此，宁可不观测也不做大额分配。
    if (size == 0 || size > kMaxTableBytes) return false;
    // 表可能在两次调用之间变大，给几次机会。
    for (int attempt = 0; attempt < 3; ++attempt) {
        buf.assign(size, 0);
        const DWORD rc =
            GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        if (rc == NO_ERROR) return true;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return false;
        if (size > kMaxTableBytes) return false;
    }
    return false;
}

struct State {
    ULONGLONG lastTickMs = 0;
    ULONGLONG lastLogMs = 0;
    std::string lastSig;
    int faults = 0;
    bool disabled = false;
};

State g{};

void TickImpl(ULONGLONG now) {
    const auto pids = CollectClassicPids();
    if (pids.empty()) {
        // 游戏没在跑：清态，下次开游戏重新落一行完整路径。
        g.lastSig.clear();
        g.lastLogMs = 0;
        return;
    }

    std::vector<char> buf;
    if (!ReadTcpTable(buf)) return;
    constexpr size_t kHeadBytes = offsetof(MIB_TCPTABLE_OWNER_PID, table);
    if (buf.size() < kHeadBytes) return;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    // 不信任 dwNumEntries：按实际缓冲区能装下的条数夹一次，避免越界读。
    const size_t roomEntries = (buf.size() - kHeadBytes) / sizeof(MIB_TCPROW_OWNER_PID);
    const size_t entries = (std::min)(static_cast<size_t>(table->dwNumEntries), roomEntries);

    std::vector<Conn> conns;
    for (size_t i = 0; i < entries; ++i) {
        const auto& row = table->table[i];
        if (row.dwState != MIB_TCP_STATE_ESTAB) continue;
        if (std::find(pids.begin(), pids.end(), row.dwOwningPid) == pids.end()) continue;
        conns.push_back({row.dwOwningPid, row.dwLocalAddr, row.dwRemoteAddr,
                         PortFromRow(row.dwLocalPort), PortFromRow(row.dwRemotePort)});
    }

    std::vector<std::string> lines;
    std::vector<std::string> sigParts;
    int httpsN = 0;
    std::string httpsNic = "?";
    const char* httpsTun = "?";
    if (!conns.empty()) {
        const auto nics = CollectLocalV4Nics();
        for (const auto& c : conns) {
            const NicInfo* nic = FindNic(nics, c.localAddr);
            // 判不了就必须写 ?，不能报 tunnel=0/1：把「未知」说成「已确认」正是本模块
            // 要避免的错误结论（2026-08-13 那轮排查就栽在这类过度自信的判据上）。
            const char* verdict = "?";
            const char* why = " why=本地地址未匹配到网卡，隧道判定不可靠";
            if (IsLoopback(c.remoteAddr) || IsLoopback(c.localAddr)) {
                // 环回端口每次重连都变；不进签名、不落盘（NGM IPC / 本地转发）。
                continue;
            } else if (nic && nic->tunnelish) {
                verdict = "1";
                why = " why=本地绑在虚拟网卡（TUN/TAP 式加速器）";
            } else if (IsPrivateOrFakeIp(c.remoteAddr)) {
                verdict = "1";
                why = " why=对端为私网/fake-ip（本地代理转发式加速器）";
            } else if (nic) {
                verdict = "0";
                why = "";
            }

            const char* nicLabel = nic ? nic->label.c_str() : "?";
            if (c.remotePort == 443) {
                ++httpsN;
                httpsNic = nicLabel;
                httpsTun = verdict;
                continue;
            }

            const bool gamePort =
                c.remotePort == 8585 || c.remotePort == 8586 || c.remotePort == 8587;
            const bool phoneHome = c.remotePort == 58880;
            const bool direct = verdict[0] == '0' && verdict[1] == '\0';
            if (!gamePort && !phoneHome && !direct) continue;

            char sk[192]{};
            std::snprintf(sk, sizeof(sk), "%lu|%s:%u|%s|%s", static_cast<unsigned long>(c.pid),
                          FormatV4(c.remoteAddr).c_str(), c.remotePort, nicLabel, verdict);
            sigParts.emplace_back(sk);

            char line[768]{};
            std::snprintf(line, sizeof(line), "pid=%lu %s:%u -> %s:%u nic=%s tunnel=%s%s",
                          static_cast<unsigned long>(c.pid), FormatV4(c.localAddr).c_str(),
                          c.localPort, FormatV4(c.remoteAddr).c_str(), c.remotePort, nicLabel,
                          verdict, why);
            lines.emplace_back(line);
        }
        if (httpsN) {
            char sk[96]{};
            std::snprintf(sk, sizeof(sk), "443|%s|%s", httpsNic.c_str(), httpsTun);
            sigParts.emplace_back(sk);
        }
        std::sort(sigParts.begin(), sigParts.end());
        std::sort(lines.begin(), lines.end());
    }

    std::string sig;
    if (sigParts.empty()) {
        sig = conns.empty() ? "none" : "https-or-loop-only";
    } else {
        for (const auto& p : sigParts) {
            sig += p;
            sig.push_back('|');
        }
    }

    const bool changed = sig != g.lastSig;
    const bool heartbeat = g.lastLogMs == 0 || now - g.lastLogMs >= kHeartbeatMs;
    if (!changed && !heartbeat) return;
    g.lastSig = sig;
    g.lastLogMs = now;

    if (conns.empty()) {
        // 只查 IPv4：若在图内仍长期落这行，说明该走 IPv6，届时再补 GetExtendedTcpTable(AF_INET6)。
        xcat::log::Info("NetPath", "游戏在跑但无已建立 IPv4 连接（登录中 / 刚启动 / 已断线）");
        return;
    }
    if (httpsN) {
        xcat::log::Info("NetPath",
                        "https x%d nic=%s tunnel=%s（CDN/fake-ip，已折叠；本地端口抖动不记）",
                        httpsN, httpsNic.c_str(), httpsTun);
    }
    const size_t shown = (std::min)(lines.size(), kMaxLogLines);
    for (size_t i = 0; i < shown; ++i) xcat::log::Info("NetPath", "%s", lines[i].c_str());
    if (lines.size() > shown) {
        xcat::log::Info("NetPath", "另有 %zu 条连接未列出（本轮已截断）", lines.size() - shown);
    }
    if (lines.empty() && httpsN == 0) {
        xcat::log::Info("NetPath", "仅环回 IPC，无游戏/探测端点");
    }
}

}  // namespace

void Tick() {
    // 纯观测功能：任何失败都只能让它自己不干活，绝不允许影响面板与其他模块。
    if (g.disabled) return;
    const ULONGLONG now = GetTickCount64();
    if (g.lastTickMs != 0 && now - g.lastTickMs < kTickIntervalMs) return;
    g.lastTickMs = now;  // 先记节流再干活：抛异常也不会退化成每帧重试

    try {
        TickImpl(now);
        g.faults = 0;  // 偶发抖动不累积到停用
    } catch (...) {
        if (++g.faults < kMaxFaults) return;
        g.disabled = true;
        try {
            xcat::log::Warn("NetPath", "连续 %d 次异常，已永久停用端点观测（诊断功能，不影响其他模块）",
                            kMaxFaults);
        } catch (...) {
        }
    }
}

}  // namespace xcat::app::net_path
