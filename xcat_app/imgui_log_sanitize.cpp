#include "imgui_log_sanitize.h"

#include <cstring>
#include <string>
#include <string_view>

namespace xcat::app {
namespace {

unsigned char AsciiLower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
    return c;
}

bool AsciiIEquals(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (AsciiLower(static_cast<unsigned char>(a[i])) !=
            AsciiLower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool IsHostChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-';
}

bool IsUrlTokenChar(unsigned char c) { return c > 32 && c != 127; }

bool TokenStart(const char* s, size_t i) {
    if (i == 0) return true;
    return !IsHostChar(static_cast<unsigned char>(s[i - 1]));
}

bool MatchIEqualsAt(const char* s, size_t n, size_t i, const char* lit) {
    const size_t len = std::strlen(lit);
    if (i + len > n) return false;
    return AsciiIEquals(s + i, lit, len);
}

bool MatchSchemeUrl(const char* s, size_t n, size_t i, size_t* end) {
    const char* schemes[] = {"https://", "http://", "wss://", "ws://"};
    for (const char* sch : schemes) {
        const size_t len = std::strlen(sch);
        if (i + len > n) continue;
        if (!AsciiIEquals(s + i, sch, len)) continue;
        size_t j = i + len;
        while (j < n && IsUrlTokenChar(static_cast<unsigned char>(s[j]))) ++j;
        if (j == i + len) continue;
        *end = j;
        return true;
    }
    return false;
}

bool ParseDecOctet(const char* s, size_t n, size_t i, size_t* next, int* value) {
    if (i >= n || s[i] < '0' || s[i] > '9') return false;
    int v = 0;
    size_t j = i;
    while (j < n && s[j] >= '0' && s[j] <= '9') {
        v = v * 10 + (s[j] - '0');
        if (v > 255) return false;
        ++j;
        if (j - i > 3) return false;
    }
    if (j == i) return false;
    *value = v;
    *next = j;
    return true;
}

bool MatchIPv4(const char* s, size_t n, size_t i, size_t* end) {
    if (!TokenStart(s, i)) return false;
    size_t cur = i;
    for (int part = 0; part < 4; ++part) {
        int octet = 0;
        size_t next = 0;
        if (!ParseDecOctet(s, n, cur, &next, &octet)) return false;
        cur = next;
        if (part < 3) {
            if (cur >= n || s[cur] != '.') return false;
            ++cur;
        }
    }
    if (cur < n) {
        const unsigned char c = static_cast<unsigned char>(s[cur]);
        if ((c >= '0' && c <= '9') || IsHostChar(c)) return false;
    }
    *end = cur;
    return true;
}

bool HostEndsWith(const char* host, size_t hostLen, const char* suffix) {
    const size_t sl = std::strlen(suffix);
    if (hostLen < sl) return false;
    if (!AsciiIEquals(host + hostLen - sl, suffix, sl)) return false;
    if (hostLen == sl) return true;
    return host[hostLen - sl - 1] == '.';
}

bool MatchKnownHostUrl(const char* s, size_t n, size_t i, size_t* end) {
    if (!TokenStart(s, i) || !IsHostChar(static_cast<unsigned char>(s[i]))) return false;
    size_t j = i;
    while (j < n && IsHostChar(static_cast<unsigned char>(s[j]))) ++j;
    const size_t hostLen = j - i;
    if (hostLen < 8) return false;  // shortest: xcat.work
    static const char* kSuffixes[] = {
        "xcat.work", "beanfun.com.tw", "beanfun.com", "gamania.com",
        "alidns.com", "aliyuncs.com", "deepseek.com",
    };
    bool hit = false;
    for (const char* suf : kSuffixes) {
        if (HostEndsWith(s + i, hostLen, suf)) {
            hit = true;
            break;
        }
    }
    if (!hit) return false;
    while (j < n && IsUrlTokenChar(static_cast<unsigned char>(s[j]))) ++j;
    *end = j;
    return true;
}

bool MatchOtt(const char* s, size_t n, size_t i, size_t* end) {
    if (!MatchIEqualsAt(s, n, i, "OTT:")) return false;
    size_t j = i + 4;
    if (j >= n || s[j] < '0' || s[j] > '9') return false;
    while (j < n && s[j] >= '0' && s[j] <= '9') ++j;
    if (!MatchIEqualsAt(s, n, j, ":Login:")) return false;
    j += 7;
    while (j < n && IsUrlTokenChar(static_cast<unsigned char>(s[j])) && s[j] != '&') ++j;
    *end = j;
    return true;
}

bool MatchKeyedSecret(const char* s, size_t n, size_t i, const char* key, size_t* end) {
    if (!MatchIEqualsAt(s, n, i, key)) return false;
    const size_t len = std::strlen(key);
    size_t j = i + len;
    if (j >= n || !IsUrlTokenChar(static_cast<unsigned char>(s[j]))) return false;
    while (j < n && IsUrlTokenChar(static_cast<unsigned char>(s[j])) && s[j] != '&') ++j;
    *end = j;
    return true;
}

}  // namespace

std::string SanitizeImGuiLogLine(std::string_view raw) {
    if (raw.empty()) return {};
    std::string out;
    out.reserve(raw.size());
    const char* s = raw.data();
    const size_t n = raw.size();
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        if (MatchSchemeUrl(s, n, i, &end)) {
            out += "[url]";
            i = end;
            continue;
        }
        if (MatchIPv4(s, n, i, &end)) {
            out += "[ip]";
            i = end;
            continue;
        }
        if (MatchKnownHostUrl(s, n, i, &end)) {
            out += "[url]";
            i = end;
            continue;
        }
        if (MatchOtt(s, n, i, &end)) {
            out += "OTT:***";
            i = end;
            continue;
        }
        if (MatchKeyedSecret(s, n, i, "access_token=", &end)) {
            out += "access_token=***";
            i = end;
            continue;
        }
        if (MatchKeyedSecret(s, n, i, "WebToken=", &end)) {
            out += "WebToken=***";
            i = end;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

}  // namespace xcat::app
