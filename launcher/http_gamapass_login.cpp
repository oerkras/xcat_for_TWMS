#include "http_gamapass_login.h"

#include "gamapass_cdp_login.h"
#include "http_client.h"
#include "ott_ticket_fetch.h"

#include <bcrypt.h>
#include <rpc.h>
#include <wincrypt.h>

#include <ShlObj.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace msc::launcher {
namespace {

constexpr wchar_t kGalaxyLoginMstc[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/mstc"
    L"?redirect_url=https://maplestoryclassic.beanfun.com/Main";

constexpr wchar_t kGamaPassOpenId[] =
    L"https://openid.beanfun.com/login/index"
    L"?clientid=17599671-8530-4a00-9825-1c2b346eab4d"
    L"&redirectUri=https%3A%2F%2Fgalaxy.games.gamania.com%2Fwebapi%2Fview%2Flogin%2Fresult%2Fmstc%2Fbeanfun";

constexpr wchar_t kGalaxyResultBeanfun[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/result/mstc/beanfun";

constexpr wchar_t kClassicMain[] = L"https://maplestoryclassic.beanfun.com/Main";

constexpr char kApiBase[] = "https://api.accounts.gamania.com";
constexpr wchar_t kOauthAuthorizeApi[] =
    L"https://accounts.gamania.com/api/v1/oauth2/authorize";

// Nuxt public.gtwClientId（Galaxy Classic OAuth client，base64 形态原样作 x-client-id）
constexpr char kGtwClientId[] = "Yzk2MWRhMmEtOTEwNy00YmIzLTkwOTYtN2NjZGY2MDI3MTNl";

// requireEncryptedTokenForBE：AES-CFB key（Base64）
constexpr char kBeKeyB64[] = "1lMoWEWGyfufSJEtrMm0kQ5lj8phAC3H";
// isServerSide：CryptoJS OpenSSL salted（key/iv WordArray 的 Base64）
constexpr char kServerKeyB64[] = "Z2FtYS1vYXV0aC13ZWItcHJvZC1zZXJ2ZXItc2lkZS10b2tlbi1rZXkK";
constexpr char kServerIvB64[] = "Z2FtYS1vYXV0aC13ZWItcHJvZC1zZXJ2ZXItc2lkZS10b2tlbi1pdgo=";

void Log(const HttpLoginLogFn& log, const std::wstring& line) {
    if (log) log(line);
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

HttpLoginResult Fail(HttpLoginError e, const std::string& msg) {
    HttpLoginResult r;
    r.ok = false;
    r.error = e;
    r.message = msg;
    return r;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            continue;
        } else {
            o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

std::string NewUuid() {
    UUID u{};
    if (UuidCreate(&u) != RPC_S_OK) {
        // 退化：时间戳伪随机
        char buf[40]{};
        snprintf(buf, sizeof(buf), "%08lx-%04lx-4%03lx-8%03lx-%012llx",
                 GetTickCount() ^ GetCurrentProcessId(), (GetTickCount() >> 3) & 0xffff,
                 GetTickCount() & 0xfff, (GetTickCount() >> 7) & 0xfff,
                 (unsigned long long)(GetTickCount64() ^ 0x9e3779b97f4a7c15ULL));
        return buf;
    }
    RPC_CSTR str = nullptr;
    if (UuidToStringA(&u, &str) != RPC_S_OK || !str) return NewUuid();
    std::string out(reinterpret_cast<char*>(str));
    RpcStringFreeA(&str);
    return out;
}

std::vector<uint8_t> B64Decode(const char* b64) {
    DWORD need = 0;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, nullptr, &need, nullptr, nullptr) ||
        need == 0) {
        return {};
    }
    std::vector<uint8_t> out(need);
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, out.data(), &need, nullptr, nullptr)) {
        return {};
    }
    out.resize(need);
    return out;
}

std::string B64Encode(const uint8_t* data, size_t len) {
    DWORD need = 0;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(len),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &need)) {
        return {};
    }
    std::string out(need, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(len),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &need)) {
        return {};
    }
    if (!out.empty() && out.back() == '\0') out.pop_back();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '\0'))
        out.pop_back();
    return out;
}

bool BcryptRandom(uint8_t* dst, size_t n) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, dst, static_cast<ULONG>(n),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::vector<uint8_t> Md5(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> out(16);
    DWORD cb = 0, objLen = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
        return {};
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen),
                      &cb, 0);
    std::vector<uint8_t> obj(objLen);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(hash, out.data(), 16, 0))) {
        out.clear();
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// OpenSSL / CryptoJS EvpKDF (MD5)
void EvpBytesToKey(const std::vector<uint8_t>& pass, const uint8_t salt[8], uint8_t key[32],
                   uint8_t iv[16]) {
    std::vector<uint8_t> prev;
    std::vector<uint8_t> out;
    while (out.size() < 48) {
        std::vector<uint8_t> block;
        block.insert(block.end(), prev.begin(), prev.end());
        block.insert(block.end(), pass.begin(), pass.end());
        block.insert(block.end(), salt, salt + 8);
        prev = Md5(block.data(), block.size());
        out.insert(out.end(), prev.begin(), prev.end());
    }
    memcpy(key, out.data(), 32);
    memcpy(iv, out.data() + 32, 16);
}

std::string BytesToHex(const std::vector<uint8_t>& v) {
    static const char* hex = "0123456789abcdef";
    std::string o;
    o.resize(v.size() * 2);
    for (size_t i = 0; i < v.size(); ++i) {
        o[i * 2] = hex[v[i] >> 4];
        o[i * 2 + 1] = hex[v[i] & 0xf];
    }
    return o;
}

bool AesCfbEncrypt(const std::vector<uint8_t>& key, const uint8_t iv[16], const uint8_t* plain,
                   size_t plainLen, std::vector<uint8_t>& cipher) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hkey = nullptr;
    bool ok = false;
    cipher.assign(plainLen, 0);
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
        return false;
    const wchar_t* mode = BCRYPT_CHAIN_MODE_CFB;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode,
                                          static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t)),
                                          0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &hkey, nullptr, 0,
                                                   const_cast<PUCHAR>(key.data()),
                                                   static_cast<ULONG>(key.size()), 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    ULONG cb = 0;
    std::vector<uint8_t> ivCopy(iv, iv + 16);
    NTSTATUS st =
        BCryptEncrypt(hkey, const_cast<PUCHAR>(plain), static_cast<ULONG>(plainLen), nullptr,
                      ivCopy.data(), 16, cipher.data(), static_cast<ULONG>(cipher.size()), &cb, 0);
    ok = BCRYPT_SUCCESS(st) && cb == plainLen;
    BCryptDestroyKey(hkey);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool AesCbcEncryptPkcs7(const uint8_t key[32], const uint8_t iv[16], const uint8_t* plain,
                        size_t plainLen, std::vector<uint8_t>& cipher) {
    // PKCS7 pad
    const size_t pad = 16 - (plainLen % 16);
    std::vector<uint8_t> padded(plain, plain + plainLen);
    padded.insert(padded.end(), pad, static_cast<uint8_t>(pad));

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hkey = nullptr;
    cipher.assign(padded.size(), 0);
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
        return false;
    const wchar_t* mode = BCRYPT_CHAIN_MODE_CBC;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode,
                                          static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t)),
                                          0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &hkey, nullptr, 0, const_cast<PUCHAR>(key),
                                                   32, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    ULONG cb = 0;
    std::vector<uint8_t> ivCopy(iv, iv + 16);
    NTSTATUS st =
        BCryptEncrypt(hkey, padded.data(), static_cast<ULONG>(padded.size()), nullptr, ivCopy.data(),
                      16, cipher.data(), static_cast<ULONG>(cipher.size()), &cb, 0);
    const bool ok = BCRYPT_SUCCESS(st) && cb == padded.size();
    BCryptDestroyKey(hkey);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Authorization for api.accounts… requireEncryptedTokenForBE
std::string MakeBeAuthToken() {
    auto key = B64Decode(kBeKeyB64);
    if (key.size() != 24) return {};
    uint8_t iv[16]{};
    if (!BcryptRandom(iv, 16)) return {};
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    const int64_t unixMs =
        static_cast<int64_t>(uli.QuadPart / 10000ULL) - 11644473600000LL;
    char plain[64]{};
    snprintf(plain, sizeof(plain), "{\"time\":%lld}", static_cast<long long>(unixMs));
    std::vector<uint8_t> cipher;
    if (!AesCfbEncrypt(key, iv, reinterpret_cast<const uint8_t*>(plain), strlen(plain), cipher))
        return {};
    std::vector<uint8_t> packed;
    packed.insert(packed.end(), iv, iv + 16);
    packed.insert(packed.end(), cipher.begin(), cipher.end());
    return B64Encode(packed.data(), packed.size());
}

// Authorization for accounts.gamania.com Nuxt isServerSide（对齐 CryptoJS pL）
std::string MakeServerSideAuthToken() {
    auto keyWa = B64Decode(kServerKeyB64);
    if (keyWa.empty()) return {};
    // CryptoJS: AES.encrypt(json, wordArray.toString(/*hex*/), {iv, CBC, Pkcs7}).toString()
    // key 为字符串 → PasswordBasedCipher：随机 salt + EvpKDF 派生 key/iv（显式 iv 实际被忽略）
    const std::string passHex = BytesToHex(keyWa);
    std::vector<uint8_t> pass(passHex.begin(), passHex.end());
    uint8_t salt[8]{};
    if (!BcryptRandom(salt, 8)) return {};
    uint8_t key[32]{}, iv[16]{};
    EvpBytesToKey(pass, salt, key, iv);

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    const int64_t unixMs =
        static_cast<int64_t>(uli.QuadPart / 10000ULL) - 11644473600000LL;
    char plain[64]{};
    snprintf(plain, sizeof(plain), "{\"time\":%lld}", static_cast<long long>(unixMs));
    std::vector<uint8_t> cipher;
    if (!AesCbcEncryptPkcs7(key, iv, reinterpret_cast<const uint8_t*>(plain), strlen(plain), cipher))
        return {};
    std::vector<uint8_t> openssl;
    openssl.insert(openssl.end(), {'S', 'a', 'l', 't', 'e', 'd', '_', '_'});
    openssl.insert(openssl.end(), salt, salt + 8);
    openssl.insert(openssl.end(), cipher.begin(), cipher.end());
    return B64Encode(openssl.data(), openssl.size());
}

bool IsGalaxyInitUrl(const std::wstring& u) {
    return u.find(L"/login/init/") != std::wstring::npos;
}

bool UrlHasSelectGameAccount(const std::wstring& u) {
    std::wstring lower = u;
    for (auto& c : lower) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return lower.find(L"selectgameaccount") != std::wstring::npos;
}

std::wstring JoinUrlLocal(const std::wstring& base, const std::wstring& loc) {
    if (loc.empty()) return base;
    if (loc.rfind(L"http://", 0) == 0 || loc.rfind(L"https://", 0) == 0) return loc;
    // scheme://host/...
    const size_t scheme = base.find(L"://");
    if (scheme == std::wstring::npos) return loc;
    const size_t pathStart = base.find(L'/', scheme + 3);
    if (!loc.empty() && loc[0] == L'/') {
        if (pathStart == std::wstring::npos) return base + loc;
        return base.substr(0, pathStart) + loc;
    }
    if (pathStart == std::wstring::npos) return base + L"/" + loc;
    const size_t slash = base.find_last_of(L'/');
    return (slash == std::wstring::npos ? base + L"/" : base.substr(0, slash + 1)) + loc;
}

std::string HtmlTagAttr(const std::string& tag, const char* attr) {
    const std::string pat =
        std::string(attr) + "\\s*=\\s*[\"']([^\"']*)[\"']";
    return msc::http::RegexGroup1(tag, pat.c_str());
}

std::string ExtractWebTokenFromJump(const msc::http::Response& jump) {
    std::string webToken;
    for (const auto& u : jump.redirectChain) {
        webToken = msc::http::RegexGroup1(NarrowUtf8(u), "[Ww]eb[Tt]oken=([^&]+)");
        if (!webToken.empty()) return webToken;
    }
    webToken = msc::http::RegexGroup1(NarrowUtf8(jump.finalUrl), "[Ww]eb[Tt]oken=([^&]+)");
    if (!webToken.empty()) return webToken;
    webToken = msc::http::RegexGroup1(jump.body, "[Ww]eb[Tt]oken=([^&\"']+)");
    return webToken;
}

// beanfun「選擇您的帳號」：按 GamaPass 昵称槽（1-based）勾选并 POST
// （radio / checkbox / select / __doPostBack；与 CDP 主路径共用 GetGamaPassNickSlot）
bool TrySubmitSelectGameAccount(msc::http::Client& http, const msc::http::Response& page,
                                const HttpLoginLogFn& log, msc::http::Response& out) {
    if (!UrlHasSelectGameAccount(page.finalUrl) &&
        !msc::http::ContainsI(page.body, "SelectGameAccount") &&
        !msc::http::ContainsI(page.body, "選擇您的帳號") &&
        !msc::http::ContainsI(page.body, "选择您的账号") &&
        !msc::http::ContainsI(page.body, "選擇遊戲帳號") &&
        !msc::http::ContainsI(page.body, "选择游戏账号")) {
        return false;
    }

    const int wantSlot = GetGamaPassNickSlot();  // 1..16
    std::string pickName, pickValue;
    int usedSlot = 0;
    int goodsCount = 0;
    bool clamped = false;
    auto lowerType = [](std::string t) {
        for (auto& c : t)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return t;
    };

    try {
        std::regex re("<input\\b([^>]*)>", std::regex::icase);
        std::vector<std::pair<std::string, std::string>> radios;
        for (std::sregex_iterator it(page.body.begin(), page.body.end(), re), end; it != end; ++it) {
            const std::string tag = (*it)[0].str();
            const std::string tl = lowerType(HtmlTagAttr(tag, "type"));
            if (tl != "radio" && tl != "checkbox") continue;
            const std::string name = HtmlTagAttr(tag, "name");
            const std::string value = HtmlTagAttr(tag, "value");
            if (name.empty()) continue;
            radios.emplace_back(name, value);
        }
        goodsCount = static_cast<int>(radios.size());
        if (!radios.empty()) {
            int idx = wantSlot - 1;
            if (idx < 0) {
                idx = 0;
                clamped = true;
            }
            if (idx >= goodsCount) {
                idx = goodsCount - 1;
                clamped = true;
            }
            usedSlot = idx + 1;
            pickName = radios[static_cast<size_t>(idx)].first;
            pickValue = radios[static_cast<size_t>(idx)].second;
        }
    } catch (...) {
    }

    // <select>：第 N 个非空 option（与昵称槽对齐）
    if (pickName.empty()) {
        try {
            std::regex reSel("<select\\b([^>]*)>([\\s\\S]*?)</select>", std::regex::icase);
            std::smatch sm;
            if (std::regex_search(page.body, sm, reSel)) {
                pickName = HtmlTagAttr(sm[1].str(), "name");
                std::regex reOpt("<option\\b([^>]*)>", std::regex::icase);
                const std::string inner = sm[2].str();
                std::vector<std::string> opts;
                for (std::sregex_iterator it(inner.begin(), inner.end(), reOpt), end; it != end;
                     ++it) {
                    const std::string tag = (*it)[0].str();
                    const std::string value = HtmlTagAttr(tag, "value");
                    if (value.empty()) continue;
                    opts.push_back(value);
                }
                goodsCount = static_cast<int>(opts.size());
                if (!opts.empty()) {
                    int idx = wantSlot - 1;
                    if (idx < 0) {
                        idx = 0;
                        clamped = true;
                    }
                    if (idx >= goodsCount) {
                        idx = goodsCount - 1;
                        clamped = true;
                    }
                    usedSlot = idx + 1;
                    pickValue = opts[static_cast<size_t>(idx)];
                } else {
                    pickName.clear();
                }
            }
        } catch (...) {
        }
    }

    // ASP.NET __doPostBack：无 radio/select 时退回第一项（无法可靠按槽枚举）
    std::string postBackTarget;
    if (pickName.empty()) {
        postBackTarget =
            msc::http::RegexGroup1(page.body, "__doPostBack\\(\\s*['\"]([^'\"]+)['\"]");
        if (!postBackTarget.empty()) {
            pickName = "__EVENTTARGET";
            pickValue = postBackTarget;
            usedSlot = 1;
            goodsCount = 1;
            clamped = (wantSlot != 1);
            Log(log, L"[gamapass-http] SelectGameAccount：仅有 __doPostBack，无法按槽选取，退回第一项"
                     L"（wantSlot=" +
                         std::to_wstring(wantSlot) + L"）");
        }
    }

    if (pickName.empty()) {
        Log(log, L"[gamapass-http] SelectGameAccount 页无 radio/select/doPostBack，无法自动选昵称"
                 L"（bodyBytes=" +
                     std::to_wstring(page.body.size()) + L"）");
        return false;
    }

    std::string action = msc::http::RegexGroup1(
        page.body, "<form\\b[^>]*\\baction\\s*=\\s*[\"']([^\"']+)[\"']");
    if (action.empty()) {
        action = NarrowUtf8(page.finalUrl);
    }
    const std::wstring postUrl = JoinUrlLocal(page.finalUrl, WidenUtf8(action));

    std::vector<std::pair<std::string, std::string>> fields;
    auto pushUnique = [&](const std::string& name, const std::string& value) {
        if (name.empty()) return;
        for (auto& kv : fields) {
            if (kv.first == name) {
                kv.second = value;
                return;
            }
        }
        fields.emplace_back(name, value);
    };

    try {
        std::regex re("<input\\b([^>]*)>", std::regex::icase);
        for (std::sregex_iterator it(page.body.begin(), page.body.end(), re), end; it != end; ++it) {
            const std::string tag = (*it)[0].str();
            const std::string tl = lowerType(HtmlTagAttr(tag, "type"));
            const std::string name = HtmlTagAttr(tag, "name");
            if (name.empty()) continue;
            if (tl == "radio" || tl == "checkbox") {
                if (name == pickName) pushUnique(name, pickValue);
                continue;
            }
            if (tl == "submit" || tl == "button" || tl == "image") continue;
            pushUnique(name, HtmlTagAttr(tag, "value"));
        }
    } catch (...) {
    }

    if (!postBackTarget.empty()) {
        pushUnique("__EVENTTARGET", postBackTarget);
        pushUnique("__EVENTARGUMENT", "");
    } else {
        pushUnique(pickName, pickValue);
        bool hasEventTarget = false;
        for (const auto& kv : fields) {
            if (kv.first == "__EVENTTARGET") {
                hasEventTarget = true;
                break;
            }
        }
        if (hasEventTarget) pushUnique("__EVENTTARGET", pickName);
    }

    Log(log, L"[gamapass-http] SelectGameAccount：提交昵称 slot=" + std::to_wstring(wantSlot) +
                 L"|use=" + std::to_wstring(usedSlot) + L"|goods=" + std::to_wstring(goodsCount) +
                 (clamped ? L"|clamped" : L"") + L" name=" + WidenUtf8(pickName) + L" value=" +
                 WidenUtf8(pickValue.substr(0, 48)));
    out = http.PostForm(postUrl, fields, /*follow=*/true, page.finalUrl);
    return true;
}

bool IsTicketOtt(const std::wstring& ott) {
    return HttpIsTicketOtt(ott);
}

std::wstring ExtractLoginSessionOtt(const std::vector<std::wstring>& chain,
                                    const std::wstring& finalUrl, const std::string& body) {
    auto fromUrl = [](const std::wstring& u) -> std::wstring {
        if (!IsGalaxyInitUrl(u)) return {};
        std::wstring ott = ExtractOttToken(u);
        if (ott.rfind(L"OTT:", 0) != 0) return {};
        if (ott.find(L":Login:") == std::wstring::npos) return {};
        return ott;
    };
    for (const auto& u : chain) {
        std::wstring ott = fromUrl(u);
        if (!ott.empty()) return ott;
    }
    std::wstring ott = fromUrl(finalUrl);
    if (!ott.empty()) return ott;
    const std::string raw =
        msc::http::RegexGroup1(body, "(OTT:[0-9]+:Login:[A-Za-z0-9_\\-+/=]+)");
    if (!raw.empty()) {
        std::wstring w = WidenUtf8(raw);
        if (w.find(L":Login:") != std::wstring::npos) return w;
    }
    return {};
}

std::map<std::string, std::string> ParseQuery(const std::wstring& url) {
    std::map<std::string, std::string> out;
    const size_t q = url.find(L'?');
    if (q == std::wstring::npos) return out;
    std::string qs = NarrowUtf8(url.substr(q + 1));
    size_t i = 0;
    while (i < qs.size()) {
        size_t amp = qs.find('&', i);
        if (amp == std::string::npos) amp = qs.size();
        std::string pair = qs.substr(i, amp - i);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        i = amp + 1;
    }
    return out;
}

std::string UrlDecode(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            char* end = nullptr;
            const long v = strtol(hex, &end, 16);
            if (end && end != hex) {
                o.push_back(static_cast<char>(v));
                i += 2;
                continue;
            }
        }
        o.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return o;
}

std::wstring RedactOtt(const std::wstring& ott) { return HttpRedactOtt(ott); }

bool JsonBool(const std::string& json, const char* key, bool* present = nullptr) {
    const std::string pat = std::string("\"") + key + "\"\\s*:\\s*(true|false)";
    const std::string v = msc::http::RegexGroup1(json, pat.c_str());
    if (present) *present = !v.empty();
    return v == "true";
}

std::string JsonString(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)\"";
    return msc::http::RegexGroup1(json, pat.c_str());
}

int JsonInt(const std::string& json, const char* key, int def = 0) {
    const std::string pat = std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)";
    const std::string v = msc::http::RegexGroup1(json, pat.c_str());
    if (v.empty()) return def;
    return atoi(v.c_str());
}

int64_t JsonInt64(const std::string& json, const char* key, int64_t def = 0) {
    const std::string pat = std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)";
    const std::string v = msc::http::RegexGroup1(json, pat.c_str());
    if (v.empty()) return def;
    return _strtoi64(v.c_str(), nullptr, 10);
}

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash);
    return p;
}

std::wstring SessionStorePath() { return ModuleDir() + L"\\gamapass_session.dpapi"; }
std::wstring SessionStorePathLegacy() { return ModuleDir() + L"\\gamapass_session.json"; }

std::string MaskEmail(const std::string& email) {
    const size_t at = email.find('@');
    if (at == std::string::npos || at < 2) return "(hidden)";
    return email.substr(0, 1) + "****" + email.substr(at - 1);
}

std::string MaskOpenId(const std::string& id) {
    if (id.size() <= 6) return "(id)";
    return id.substr(0, 2) + "…" + id.substr(id.size() - 2);
}

bool DpapiProtect(const std::string& plain, std::vector<uint8_t>& out) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, L"xcat-gamapass-session", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        return false;
    }
    out.assign(blob.pbData, blob.pbData + blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

bool DpapiUnprotect(const uint8_t* data, size_t len, std::string& plain) {
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(data);
    in.cbData = static_cast<DWORD>(len);
    DATA_BLOB blob{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &blob)) {
        return false;
    }
    plain.assign(reinterpret_cast<char*>(blob.pbData), blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

struct GamaPassAccount {
    std::string openId;
    std::string deviceId;
    std::string refreshToken;
    std::string userToken;
    std::string basicInfoToken;
    std::string email;
    std::string nickname;
    int64_t loginTime = 0;          // ms
    int64_t refreshExpiredIn = 0;   // unix sec
};

struct GamaPassSessionFile {
    std::vector<GamaPassAccount> accounts;
};

bool B64UrlDecode(const std::string& in, std::string& out) {
    std::string s = in;
    for (char& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (s.size() % 4) s.push_back('=');
    auto bin = B64Decode(s.c_str());
    if (bin.empty()) return false;
    out.assign(reinterpret_cast<const char*>(bin.data()), bin.size());
    return true;
}

bool ParseJwtClaims(const std::string& jwt, std::string& sub, std::string& deviceId) {
    const size_t d1 = jwt.find('.');
    if (d1 == std::string::npos) return false;
    const size_t d2 = jwt.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    std::string payload;
    if (!B64UrlDecode(jwt.substr(d1 + 1, d2 - d1 - 1), payload)) return false;
    sub = JsonString(payload, "sub");
    deviceId = JsonString(payload, "device_id");
    return !sub.empty();
}

bool ExtractJsonObjectAround(const std::string& text, size_t anchor, std::string& jsonOut) {
    if (anchor >= text.size()) return false;
    size_t i = anchor;
    while (i > 0 && text[i] != '{') --i;
    if (text[i] != '{') return false;
    int depth = 0;
    for (size_t j = i; j < text.size() && j < i + 4000; ++j) {
        if (text[j] == '{') ++depth;
        else if (text[j] == '}') {
            --depth;
            if (depth == 0) {
                jsonOut = text.substr(i, j - i + 1);
                return true;
            }
        }
    }
    return false;
}

bool ParseAccountFromSessionJson(const std::string& json, GamaPassAccount& out) {
    out.refreshToken = JsonString(json, "refreshToken");
    out.userToken = JsonString(json, "token");
    if (out.userToken.empty()) out.userToken = JsonString(json, "userToken");
    out.basicInfoToken = JsonString(json, "basicInfoToken");
    out.email = JsonString(json, "email");
    out.nickname = JsonString(json, "nickname");
    out.loginTime = JsonInt64(json, "loginTime", 0);
    out.refreshExpiredIn = JsonInt64(json, "refreshExpiredIn", 0);
    if (out.openId.empty()) {
        out.openId = JsonString(json, "openId");
        if (out.openId.empty()) out.openId = JsonString(json, "openID");
    }
    if (out.deviceId.empty()) {
        out.deviceId = JsonString(json, "deviceId");
        if (out.deviceId.empty()) out.deviceId = JsonString(json, "deviceID");
    }
    if (out.refreshToken.empty()) return false;
    std::string sub, dev;
    if (!out.userToken.empty() && ParseJwtClaims(out.userToken, sub, dev)) {
        if (out.openId.empty()) out.openId = sub;
        if (out.deviceId.empty()) out.deviceId = dev;
    }
    // refreshToken JWT 也常带 sub / device_id
    if ((out.openId.empty() || out.deviceId.empty()) && !out.refreshToken.empty() &&
        ParseJwtClaims(out.refreshToken, sub, dev)) {
        if (out.openId.empty()) out.openId = sub;
        if (out.deviceId.empty()) out.deviceId = dev;
    }
    return !out.openId.empty() && !out.refreshToken.empty();
}

int64_t NowUnixSec() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(uli.QuadPart / 10000000ULL) - 11644473600LL;
}

// userToken JWT 未过期才可用于 authorize；过期则禁止调用 refresh/token（会轮换 refreshToken 冲掉浏览器免登录）。
bool UserTokenStillFresh(const std::string& jwt, int skewSec = 90) {
    if (jwt.empty()) return false;
    const size_t d1 = jwt.find('.');
    if (d1 == std::string::npos) return false;
    const size_t d2 = jwt.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    std::string payload;
    if (!B64UrlDecode(jwt.substr(d1 + 1, d2 - d1 - 1), payload)) return false;
    const int64_t exp = JsonInt64(payload, "exp", 0);
    if (exp <= 0) return true;  // 无 exp 时保守复用，避免误调 refresh
    return exp > NowUnixSec() + skewSec;
}

bool AccountUsable(const GamaPassAccount& a) {
    if (a.openId.empty() || a.refreshToken.empty()) return false;
    if (a.refreshExpiredIn > 0 && a.refreshExpiredIn + 60 < NowUnixSec()) return false;
    return true;
}

void SortAccountsFirst(std::vector<GamaPassAccount>& acc) {
    // 与官网选号页一致：loginTime 升序，第一项优先
    std::sort(acc.begin(), acc.end(), [](const GamaPassAccount& a, const GamaPassAccount& b) {
        return a.loginTime < b.loginTime;
    });
}

bool ParseAccountsFromJsonBody(const std::string& body, GamaPassSessionFile& out) {
    out.accounts.clear();
    size_t pos = 0;
    while ((pos = body.find("\"refreshToken\"", pos)) != std::string::npos) {
        std::string json;
        if (ExtractJsonObjectAround(body, pos, json)) {
            GamaPassAccount a;
            a.openId = JsonString(json, "openId");
            if (a.openId.empty()) a.openId = JsonString(json, "openID");
            a.deviceId = JsonString(json, "deviceId");
            if (a.deviceId.empty()) a.deviceId = JsonString(json, "deviceID");
            if (ParseAccountFromSessionJson(json, a) && AccountUsable(a)) {
                out.accounts.push_back(std::move(a));
            }
        }
        pos += 14;
    }
    SortAccountsFirst(out.accounts);
    return !out.accounts.empty();
}

bool LoadSessionFile(GamaPassSessionFile& out) {
    out.accounts.clear();
    auto tryPath = [&](const std::wstring& path, bool dpapi) -> bool {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.empty()) return false;
        std::string body;
        if (dpapi) {
            static const char kMagic[] = "GPDP1";
            if (raw.size() < 5 || raw.compare(0, 5, kMagic) != 0) return false;
            if (!DpapiUnprotect(reinterpret_cast<const uint8_t*>(raw.data() + 5), raw.size() - 5,
                                body)) {
                return false;
            }
        } else {
            body = std::move(raw);
        }
        return ParseAccountsFromJsonBody(body, out);
    };
    if (tryPath(SessionStorePath(), true)) return true;
    // 兼容旧明文 json：读入后下次 Save 会写成 DPAPI
    if (tryPath(SessionStorePathLegacy(), false)) return true;
    return false;
}

bool SaveSessionFile(const GamaPassSessionFile& store) {
    std::ostringstream oss;
    oss << "{\n  \"accounts\": [\n";
    for (size_t i = 0; i < store.accounts.size(); ++i) {
        const auto& a = store.accounts[i];
        if (i) oss << ",\n";
        oss << "    {"
            << "\"openId\":\"" << JsonEscape(a.openId) << "\","
            << "\"deviceId\":\"" << JsonEscape(a.deviceId) << "\","
            << "\"refreshToken\":\"" << JsonEscape(a.refreshToken) << "\","
            << "\"token\":\"" << JsonEscape(a.userToken) << "\","
            << "\"basicInfoToken\":\"" << JsonEscape(a.basicInfoToken) << "\","
            << "\"email\":\"" << JsonEscape(a.email) << "\","
            << "\"nickname\":\"" << JsonEscape(a.nickname) << "\","
            << "\"loginTime\":" << a.loginTime << ","
            << "\"refreshExpiredIn\":" << a.refreshExpiredIn
            << "}";
    }
    oss << "\n  ]\n}\n";
    const std::string plain = oss.str();
    std::vector<uint8_t> blob;
    if (!DpapiProtect(plain, blob)) return false;
    const std::wstring path = SessionStorePath();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("GPDP1", 5);
    f.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
    // 迁移成功后尽量去掉旧明文
    DeleteFileW(SessionStorePathLegacy().c_str());
    return static_cast<bool>(f);
}

void UpsertAccount(GamaPassSessionFile& store, GamaPassAccount acc) {
    for (auto& a : store.accounts) {
        if (a.openId == acc.openId) {
            a = std::move(acc);
            SortAccountsFirst(store.accounts);
            return;
        }
    }
    store.accounts.push_back(std::move(acc));
    SortAccountsFirst(store.accounts);
}

bool IsChromiumProfileDirName(const wchar_t* name) {
    if (!name || !name[0] || name[0] == L'.') return false;
    if (_wcsicmp(name, L"Default") == 0) return true;
    if (_wcsnicmp(name, L"Profile ", 8) == 0) return true;
    return false;
}

bool DirExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void PushUniquePath(std::vector<std::wstring>& out, const std::wstring& p) {
    if (p.empty()) return;
    for (const auto& e : out) {
        if (_wcsicmp(e.c_str(), p.c_str()) == 0) return;
    }
    out.push_back(p);
}

std::wstring ParentDirW(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0) return {};
    return path.substr(0, pos);
}

std::wstring ExpandEnvAndApp(const std::wstring& in, const std::wstring& appDir) {
    std::wstring s = in;
    while (!s.empty() && (s.front() == L'"' || s.front() == L'\'')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L'"' || s.back() == L'\'')) s.pop_back();
    // %app% → chrome.exe 所在目录（Chrome++）
    for (;;) {
        size_t f = std::wstring::npos;
        size_t fl = 0;
        const wchar_t* keys[] = {L"%app%", L"%APP%", L"%App%"};
        for (const wchar_t* k : keys) {
            size_t p = 0;
            while ((p = s.find(k, p)) != std::wstring::npos) {
                s.replace(p, wcslen(k), appDir);
                f = p;
                fl = appDir.size();
                p += fl;
            }
        }
        if (f == std::wstring::npos) break;
    }
    wchar_t expanded[2048]{};
    if (ExpandEnvironmentStringsW(s.c_str(), expanded, 2048) > 0) s = expanded;
    wchar_t full[MAX_PATH]{};
    if (GetFullPathNameW(s.c_str(), MAX_PATH, full, nullptr) > 0) s = full;
    return s;
}

std::string ReadFileBytesNarrow(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::wstring BytesToIniText(const std::string& raw) {
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        std::wstring w;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            wchar_t ch = (wchar_t)((unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8));
            if (ch == 0) continue;
            w.push_back(ch);
        }
        return w;
    }
    // UTF-16LE without BOM（chrome++.ini 常见）
    size_t nulls = 0;
    for (size_t i = 0; i < raw.size() && i < 64; ++i)
        if (raw[i] == 0) ++nulls;
    if (nulls >= 8) {
        std::wstring w;
        for (size_t i = 0; i + 1 < raw.size(); i += 2) {
            wchar_t ch = (wchar_t)((unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8));
            w.push_back(ch);
        }
        return w;
    }
    return std::wstring(raw.begin(), raw.end());
}

bool ParseChromePlusDataDir(const std::wstring& appDir, std::wstring& outDataDir) {
    outDataDir.clear();
    const std::wstring iniNames[] = {appDir + L"\\chrome++.ini", appDir + L"\\chrome++.INI"};
    std::wstring text;
    for (const auto& ini : iniNames) {
        if (!FileExistsW(ini)) continue;
        text = BytesToIniText(ReadFileBytesNarrow(ini));
        if (!text.empty()) break;
    }
    if (text.empty()) return false;

    auto lower = text;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    // data_dir=...
    size_t pos = 0;
    while ((pos = lower.find(L"data_dir", pos)) != std::wstring::npos) {
        // 行首或换行后；跳过注释行
        size_t lineStart = text.find_last_of(L"\r\n", pos);
        lineStart = (lineStart == std::wstring::npos) ? 0 : lineStart + 1;
        std::wstring linePrefix = text.substr(lineStart, pos - lineStart);
        bool commented = false;
        for (wchar_t c : linePrefix) {
            if (c == L';') {
                commented = true;
                break;
            }
            if (c != L' ' && c != L'\t') break;
        }
        if (commented) {
            pos += 8;
            continue;
        }
        size_t eq = text.find(L'=', pos);
        if (eq == std::wstring::npos) break;
        size_t lineEnd = text.find_first_of(L"\r\n", eq + 1);
        std::wstring val = text.substr(eq + 1, lineEnd == std::wstring::npos ? std::wstring::npos
                                                                             : lineEnd - (eq + 1));
        while (!val.empty() && (val.front() == L' ' || val.front() == L'\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == L' ' || val.back() == L'\t')) val.pop_back();
        if (val.empty()) return false;  // 留空 = Chrome++ 内置默认，下面用候选目录
        std::wstring vl = val;
        for (auto& c : vl) c = (wchar_t)towlower(c);
        if (vl == L"none") return false;  // 官方默认 LocalAppData
        outDataDir = ExpandEnvAndApp(val, appDir);
        return DirExistsW(outDataDir);
    }
    // command_line 里的 --user-data-dir=
    pos = lower.find(L"--user-data-dir=");
    if (pos != std::wstring::npos) {
        size_t start = pos + 15;
        size_t end = text.find_first_of(L" \t\r\n", start);
        std::wstring val = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        outDataDir = ExpandEnvAndApp(val, appDir);
        return DirExistsW(outDataDir);
    }
    return false;
}

bool IsChromiumFamilyProcessName(const wchar_t* name) {
    if (!name || !name[0]) return false;
    return _wcsicmp(name, L"chrome.exe") == 0 || _wcsicmp(name, L"msedge.exe") == 0 ||
           _wcsicmp(name, L"chromium.exe") == 0 || _wcsicmp(name, L"360chrome.exe") == 0 ||
           _wcsicmp(name, L"360chromex.exe") == 0 || _wcsicmp(name, L"360se.exe") == 0 ||
           _wcsicmp(name, L"360browser.exe") == 0 || _wcsicmp(name, L"chrome_proxy.exe") == 0;
}

bool IsChromiumBrowserExeName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    const wchar_t* base = (slash == std::wstring::npos) ? path.c_str() : path.c_str() + slash + 1;
    return IsChromiumFamilyProcessName(base);
}

void CollectRunningChromiumExes(std::vector<std::wstring>& exes) {
    // 进程反查：正在跑的 Chromium 系（Chrome++ / 原版 / Edge / 360）
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return;
    }
    do {
        const wchar_t* name = pe.szExeFile;
        if (!IsChromiumFamilyProcessName(name)) continue;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!proc) continue;
        wchar_t path[MAX_PATH]{};
        DWORD n = MAX_PATH;
        const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &n);
        CloseHandle(proc);
        if (!ok || !path[0]) continue;
        // chrome_proxy → 同目录 chrome.exe
        std::wstring full = path;
        if (_wcsicmp(name, L"chrome_proxy.exe") == 0) {
            const std::wstring dir = ParentDirW(full);
            const std::wstring chrome = dir + L"\\chrome.exe";
            if (FileExistsW(chrome)) full = chrome;
            else continue;
        }
        if (FileExistsW(full)) PushUniquePath(exes, full);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
}

bool ParseShellOpenCommandExe(const std::wstring& cmd, std::wstring& outExe) {
    outExe.clear();
    std::wstring s = cmd;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    if (s.empty()) return false;
    if (s.front() == L'"') {
        size_t end = s.find(L'"', 1);
        if (end == std::wstring::npos) return false;
        outExe = s.substr(1, end - 1);
    } else {
        size_t sp = s.find_first_of(L" \t");
        outExe = (sp == std::wstring::npos) ? s : s.substr(0, sp);
    }
    wchar_t expanded[MAX_PATH]{};
    if (ExpandEnvironmentStringsW(outExe.c_str(), expanded, MAX_PATH) > 0) outExe = expanded;
    return FileExistsW(outExe);
}

void CollectDefaultHttpBrowserExe(std::vector<std::wstring>& exes) {
    // 系统「默认浏览器」：Https UserChoice → ProgId → shell\open\command
    auto readProgId = [](const wchar_t* urlAssoc) -> std::wstring {
        HKEY k = nullptr;
        std::wstring sub =
            std::wstring(L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\") +
            urlAssoc + L"\\UserChoice";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
            return {};
        wchar_t prog[256]{};
        DWORD typ = 0, cb = sizeof(prog);
        const LONG st =
            RegQueryValueExW(k, L"ProgId", nullptr, &typ, reinterpret_cast<LPBYTE>(prog), &cb);
        RegCloseKey(k);
        if (st != ERROR_SUCCESS || (typ != REG_SZ && typ != REG_EXPAND_SZ) || !prog[0]) return {};
        return prog;
    };
    auto readOpenCommand = [](const std::wstring& progId) -> std::wstring {
        if (progId.empty()) return {};
        // HKCU 优先，再 HKCR
        const std::wstring rel = progId + L"\\shell\\open\\command";
        auto tryKey = [&](HKEY root, const wchar_t* prefix) -> std::wstring {
            HKEY k = nullptr;
            const std::wstring path = prefix ? (std::wstring(prefix) + L"\\" + rel) : rel;
            if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
            wchar_t buf[1024]{};
            DWORD typ = 0, cb = sizeof(buf);
            const LONG st =
                RegQueryValueExW(k, nullptr, nullptr, &typ, reinterpret_cast<LPBYTE>(buf), &cb);
            RegCloseKey(k);
            if (st != ERROR_SUCCESS || (typ != REG_SZ && typ != REG_EXPAND_SZ)) return {};
            return buf;
        };
        std::wstring cmd = tryKey(HKEY_CURRENT_USER, L"Software\\Classes");
        if (cmd.empty()) cmd = tryKey(HKEY_CLASSES_ROOT, nullptr);
        return cmd;
    };

    for (const wchar_t* scheme : {L"https", L"http"}) {
        const std::wstring prog = readProgId(scheme);
        const std::wstring cmd = readOpenCommand(prog);
        std::wstring exe;
        if (!ParseShellOpenCommandExe(cmd, exe)) continue;
        if (!IsChromiumBrowserExeName(exe)) continue;  // 只收 Chromium 系（能扫 LS）
        PushUniquePath(exes, exe);
        break;
    }
}

void CollectChromeExeCandidates(std::vector<std::wstring>& exes) {
    // ① 进程反查（最准：用户正在用的那份 Chrome++ / 原版 / Edge / 360）
    CollectRunningChromiumExes(exes);
    // ② 系统默认浏览器（若是 Chromium 系）
    CollectDefaultHttpBrowserExe(exes);

    const wchar_t* kCands[] = {
        // Chrome++ / 精简便携（用户 VM：C:\Program Files\Chrome\App）
        L"%ProgramFiles%\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles(x86)%\\Chrome\\App\\chrome.exe",
        L"%LocalAppData%\\Chrome\\App\\chrome.exe",
        L"C:\\Program Files\\Chrome\\App\\chrome.exe",
        L"C:\\Program Files (x86)\\Chrome\\App\\chrome.exe",
        L"D:\\Chrome\\App\\chrome.exe",
        L"E:\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles%\\Google\\Chrome\\App\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\App\\chrome.exe",
        // 官方 Chrome
        L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
        L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe",
        // Edge
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%LocalAppData%\\Microsoft\\Edge\\Application\\msedge.exe",
        // 360 极速 / 极速 X / 安全浏览器（Chromium 内核，可开 CDP）
        L"%LocalAppData%\\360ChromeX\\Chrome\\Application\\360ChromeX.exe",
        L"%LocalAppData%\\360Chrome\\Chrome\\Application\\360chrome.exe",
        L"%LocalAppData%\\360Chrome\\Chrome\\Application\\360Chrome.exe",
        L"%ProgramFiles%\\360\\360ChromeX\\Chrome\\Application\\360ChromeX.exe",
        L"%ProgramFiles(x86)%\\360\\360ChromeX\\Chrome\\Application\\360ChromeX.exe",
        L"%ProgramFiles%\\360\\360chrome\\Chrome\\Application\\360chrome.exe",
        L"%ProgramFiles(x86)%\\360\\360chrome\\Chrome\\Application\\360chrome.exe",
        L"%ProgramFiles(x86)%\\360\\360se6\\360se.exe",
        L"%ProgramFiles%\\360\\360se6\\360se.exe",
        L"%LocalAppData%\\360Chromeese\\Chrome\\Application\\360ChromeX.exe",
    };
    for (const wchar_t* cand : kCands) {
        wchar_t expanded[MAX_PATH]{};
        if (!ExpandEnvironmentStringsW(cand, expanded, MAX_PATH)) continue;
        if (FileExistsW(expanded)) PushUniquePath(exes, expanded);
    }
    // App Paths 注册表
    auto readAppPath = [&](HKEY root, const wchar_t* sub) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        wchar_t buf[MAX_PATH]{};
        DWORD typ = 0, cb = sizeof(buf);
        if (RegQueryValueExW(k, nullptr, nullptr, &typ, reinterpret_cast<LPBYTE>(buf), &cb) ==
                ERROR_SUCCESS &&
            (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf[0]) {
            wchar_t exp[MAX_PATH]{};
            if (ExpandEnvironmentStringsW(buf, exp, MAX_PATH) && FileExistsW(exp)) {
                PushUniquePath(exes, exp);
            } else if (FileExistsW(buf)) {
                PushUniquePath(exes, buf);
            }
        }
        RegCloseKey(k);
    };
    readAppPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe");
    readAppPath(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe");
    readAppPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe");
    readAppPath(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe");
    readAppPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\360chrome.exe");
    readAppPath(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\360chrome.exe");
    readAppPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\360ChromeX.exe");
    readAppPath(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\360ChromeX.exe");
    readAppPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\360se.exe");
}

void CollectUserDataBasesNearExe(const std::wstring& exePath, std::vector<std::wstring>& bases) {
    const std::wstring appDir = ParentDirW(exePath);
    if (appDir.empty()) return;
    std::wstring fromIni;
    if (ParseChromePlusDataDir(appDir, fromIni)) {
        PushUniquePath(bases, fromIni);
    }
    // Chrome++ 常见便携目录（ini 缺省/解析失败时的兜底）
    const std::wstring siblings[] = {
        ExpandEnvAndApp(L"%app%\\..\\Data", appDir),
        ExpandEnvAndApp(L"%app%\\..\\profile", appDir),
        ExpandEnvAndApp(L"%app%\\..\\User Data", appDir),
        ExpandEnvAndApp(L"%app%\\Data", appDir),
        ExpandEnvAndApp(L"%app%\\User Data", appDir),
        ExpandEnvAndApp(L"%app%\\..\\Chrome\\User Data", appDir),
        // 360：Application 同级的 User Data
        ExpandEnvAndApp(L"%app%\\..\\User Data", appDir),
    };
    for (const auto& s : siblings) {
        if (DirExistsW(s)) PushUniquePath(bases, s);
    }

    // 官方 / 360 标准 LocalAppData（按 exe 名/路径启发式）
    wchar_t localApp[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) {
        std::wstring leaf = exePath;
        size_t slash = leaf.find_last_of(L"\\/");
        if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
        std::wstring pathLower = exePath;
        for (auto& c : pathLower) c = (wchar_t)towlower(c);
        for (auto& c : leaf) c = (wchar_t)towlower(c);

        auto pushIf = [&](const std::wstring& p) {
            if (DirExistsW(p)) PushUniquePath(bases, p);
        };
        if (leaf.find(L"msedge") != std::wstring::npos) {
            pushIf(std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data");
        } else if (leaf.find(L"360chromex") != std::wstring::npos ||
                   pathLower.find(L"360chromex") != std::wstring::npos) {
            pushIf(std::wstring(localApp) + L"\\360ChromeX\\Chrome\\User Data");
        } else if (leaf.find(L"360chrome") != std::wstring::npos ||
                   pathLower.find(L"\\360chrome\\") != std::wstring::npos) {
            pushIf(std::wstring(localApp) + L"\\360Chrome\\Chrome\\User Data");
        } else if (leaf.find(L"360se") != std::wstring::npos) {
            pushIf(std::wstring(localApp) + L"\\360se6\\User Data");
            pushIf(std::wstring(localApp) + L"\\360se\\User Data");
        } else if (leaf == L"chrome.exe" || leaf == L"chromium.exe") {
            pushIf(std::wstring(localApp) + L"\\Google\\Chrome\\User Data");
            pushIf(std::wstring(localApp) + L"\\Chromium\\User Data");
        }
    }
}

void AddLevelDbFromUserDataBase(const std::wstring& base, std::vector<std::wstring>& out) {
    if (!DirExistsW(base)) return;
    // 有的便携包 data_dir 直接就是「User Data」；也有的再包一层 User Data
    const std::wstring roots[] = {base, base + L"\\User Data"};
    for (const auto& root : roots) {
        if (!DirExistsW(root)) continue;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!IsChromiumProfileDirName(fd.cFileName)) continue;
            const std::wstring ls = root + L"\\" + fd.cFileName + L"\\Local Storage\\leveldb";
            if (DirExistsW(ls)) PushUniquePath(out, ls);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void CollectLevelDbDirs(std::vector<std::wstring>& out) {
    wchar_t localApp[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp))) return;

    // 1) 官方 / 360 默认
    const std::wstring stdBases[] = {
        std::wstring(localApp) + L"\\Google\\Chrome\\User Data",
        std::wstring(localApp) + L"\\Microsoft\\Edge\\User Data",
        std::wstring(localApp) + L"\\Chromium\\User Data",
        std::wstring(localApp) + L"\\360ChromeX\\Chrome\\User Data",
        std::wstring(localApp) + L"\\360Chrome\\Chrome\\User Data",
        std::wstring(localApp) + L"\\360se6\\User Data",
    };
    for (const auto& base : stdBases) AddLevelDbFromUserDataBase(base, out);

    // 2) 按本机 chrome/msedge.exe（含 Chrome++ C:\Program Files\Chrome\App）推 User Data
    std::vector<std::wstring> exes;
    CollectChromeExeCandidates(exes);
    std::vector<std::wstring> bases;
    for (const auto& exe : exes) CollectUserDataBasesNearExe(exe, bases);
    for (const auto& base : bases) AddLevelDbFromUserDataBase(base, out);
}

bool PreferredBrowserExeImpl(std::wstring& outExe) {
    outExe.clear();
    std::vector<std::wstring> exes;
    CollectChromeExeCandidates(exes);  // 已含：进程反查 → 默认浏览器 → 固定路径

    auto isPlus = [](const std::wstring& exe) {
        const std::wstring app = ParentDirW(exe);
        return FileExistsW(app + L"\\chrome++.ini") || FileExistsW(app + L"\\version.dll");
    };
    auto leafOf = [](const std::wstring& exe) -> std::wstring {
        size_t slash = exe.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? exe : exe.substr(slash + 1);
    };
    auto isChrome = [&](const std::wstring& exe) {
        return _wcsicmp(leafOf(exe).c_str(), L"chrome.exe") == 0;
    };
    auto isEdge = [&](const std::wstring& exe) {
        return _wcsicmp(leafOf(exe).c_str(), L"msedge.exe") == 0;
    };
    auto is360 = [&](const std::wstring& exe) {
        std::wstring leaf = leafOf(exe);
        for (auto& c : leaf) c = (wchar_t)towlower(c);
        return leaf.find(L"360chrome") != std::wstring::npos ||
               leaf.find(L"360se") != std::wstring::npos ||
               leaf.find(L"360browser") != std::wstring::npos;
    };

    // 优先级：Chrome++ > Chrome > Edge > 360（同档：正在跑优先于仅安装）。
    // ★ 禁止「任意正在跑的浏览器压过已安装的 Chrome」——Edge 常驻后台时会选 Edge，
    //   而 HasUsableSession / LS 往往来自 Chrome，CDP 副本无 GamaPass SSO（O5HKKC1 实锤）。
    std::vector<std::wstring> running;
    CollectRunningChromiumExes(running);

    auto findIn = [&](const std::vector<std::wstring>& list, auto pred) -> bool {
        for (const auto& exe : list) {
            if (pred(exe)) {
                outExe = exe;
                return true;
            }
        }
        return false;
    };

    if (findIn(running, isPlus) || findIn(exes, isPlus)) return true;
    if (findIn(running, [&](const std::wstring& e) { return isChrome(e) && !isPlus(e); }) ||
        findIn(exes, [&](const std::wstring& e) { return isChrome(e) && !isPlus(e); }))
        return true;
    if (findIn(running, isEdge) || findIn(exes, isEdge)) return true;
    if (findIn(running, is360) || findIn(exes, is360)) return true;
    if (!running.empty()) {
        outExe = running.front();
        return true;
    }
    if (!exes.empty()) {
        outExe = exes.front();
        return true;
    }
    return false;
}

bool ReadBrowserFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!out.empty()) return true;
        }
    }
    // Chrome/Edge 打开时 LevelDB 常被锁：先 CopyFile 到临时文件再读
    wchar_t tmpDir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmpDir)) return false;
    wchar_t tmpFile[MAX_PATH]{};
    if (!GetTempFileNameW(tmpDir, L"gpls", 0, tmpFile)) return false;
    if (!CopyFileW(path.c_str(), tmpFile, FALSE)) {
        DeleteFileW(tmpFile);
        return false;
    }
    std::ifstream f(tmpFile, std::ios::binary);
    if (f) {
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    DeleteFileW(tmpFile);
    return !out.empty();
}

// Chromium Local Storage 值常为 UTF-16LE；去掉 0x00 后 ASCII 针可命中（本机实测有纯 u16 无 ASCII 的 ldb）。
std::string FlattenLevelDbBlob(const std::string& raw) {
    std::string flat;
    flat.reserve(raw.size() + raw.size() / 2 + 8);
    flat.append(raw);
    flat.push_back('\n');
    for (unsigned char c : raw) {
        if (c != 0) flat.push_back(static_cast<char>(c));
    }
    return flat;
}

void HarvestRefreshTokensFromBlob(const std::string& blob, std::vector<GamaPassAccount>& out,
                                  bool& any) {
    const char* kNeedles[] = {
        "_https://accounts.gamania.com",
        "https://accounts.gamania.com",
        "accounts.gamania.com",
    };
    for (const char* needle : kNeedles) {
        size_t pos = 0;
        const size_t nlen = strlen(needle);
        while ((pos = blob.find(needle, pos)) != std::string::npos) {
            const size_t end = (std::min)(blob.size(), pos + 8000);
            const std::string win = blob.substr(pos, end - pos);
            // openId：数字或 UUID（官网 localStorage key = user_<id>）
            std::string openId = msc::http::RegexGroup1(win, "user_([0-9]{10,})");
            if (openId.empty()) {
                openId = msc::http::RegexGroup1(
                    win, "user_([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");
            }
            size_t rt = win.find("\"refreshToken\"");
            if (rt == std::string::npos) {
                pos += nlen;
                continue;
            }
            std::string json;
            if (!ExtractJsonObjectAround(win, rt, json)) {
                pos += nlen;
                continue;
            }
            GamaPassAccount a;
            a.openId = openId;
            if (ParseAccountFromSessionJson(json, a)) {
                out.push_back(std::move(a));
                any = true;
            }
            pos += nlen;
        }
    }
    size_t pos = 0;
    while ((pos = blob.find("\"refreshToken\"", pos)) != std::string::npos) {
        std::string json;
        if (ExtractJsonObjectAround(blob, pos, json) &&
            (msc::http::ContainsI(json, "deviceID") || msc::http::ContainsI(json, "deviceId") ||
             msc::http::ContainsI(json, "openID") || msc::http::ContainsI(json, "openId") ||
             msc::http::ContainsI(json, "refreshExpiredIn"))) {
            GamaPassAccount a;
            if (ParseAccountFromSessionJson(json, a)) {
                out.push_back(std::move(a));
                any = true;
            }
        }
        pos += 14;
    }
}

bool ImportAccountsFromLevelDbDir(const std::wstring& dir, std::vector<GamaPassAccount>& out,
                                  size_t* filesScanned, size_t* originHits) {
    // Chrome 打开时单文件 Copy 常不完整：整目录拷到临时目录再扫
    wchar_t tmpRoot[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmpRoot)) return false;
    wchar_t tmpDir[MAX_PATH]{};
    if (!GetTempFileNameW(tmpRoot, L"gpld", 0, tmpDir)) return false;
    DeleteFileW(tmpDir);  // GetTempFileName 创建的是文件，改成目录
    if (!CreateDirectoryW(tmpDir, nullptr)) {
        DeleteFileW(tmpDir);
        return false;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(tmpDir);
        return false;
    }
    size_t copied = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        const std::wstring src = dir + L"\\" + name;
        const std::wstring dst = std::wstring(tmpDir) + L"\\" + name;
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++copied;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    const std::wstring scanDir = copied > 0 ? std::wstring(tmpDir) : dir;
    bool any = false;
    h = FindFirstFileW((scanDir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = fd.cFileName;
            if (name.size() < 4) continue;
            const bool isLdb = name.compare(name.size() - 4, 4, L".ldb") == 0;
            const bool isLog = name.compare(name.size() - 4, 4, L".log") == 0;
            if (!isLdb && !isLog) continue;
            std::string raw;
            if (!ReadBrowserFileBytes(scanDir + L"\\" + name, raw)) continue;
            if (filesScanned) ++(*filesScanned);
            if (originHits &&
                (raw.find("accounts.gamania.com") != std::string::npos ||
                 FlattenLevelDbBlob(raw).find("accounts.gamania.com") != std::string::npos)) {
                ++(*originHits);
            }
            HarvestRefreshTokensFromBlob(raw, out, any);
            // 再扫去 null 后的扁平串（UTF-16LE → 可搜 ASCII）
            HarvestRefreshTokensFromBlob(FlattenLevelDbBlob(raw), out, any);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 清临时目录
    h = FindFirstFileW((std::wstring(tmpDir) + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DeleteFileW((std::wstring(tmpDir) + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(tmpDir);
    return any;
}

bool ImportFromBrowsers(std::vector<GamaPassAccount>& out, const HttpLoginLogFn& log) {
    std::vector<std::wstring> dirs;
    CollectLevelDbDirs(dirs);
    size_t before = out.size();
    size_t filesScanned = 0;
    size_t originHits = 0;
    for (const auto& d : dirs) {
        ImportAccountsFromLevelDbDir(d, out, &filesScanned, &originHits);
    }
    if (out.size() == before) {
        Log(log, L"[gamapass-http] 未从 Chrome/Edge Local Storage 找到 Gama Pass 会话"
                 L"（profiles=" +
                     std::to_wstring(dirs.size()) + L" files=" + std::to_wstring(filesScanned) +
                     L" originHits=" + std::to_wstring(originHits) +
                     L"）。若已在浏览器登完：请确认登录的是我们打开的同一 Chrome"
                     L"（含 Chrome++ 便携版 C:\\Program Files\\Chrome\\App），"
                     L"并停在 accounts.gamania.com 选号页勾选记住后再等；"
                     L"仅点游戏昵称不等于写出 refreshToken。");
        return false;
    }
    // 去重 openId，保留 loginTime 更新的
    std::map<std::string, GamaPassAccount> uniq;
    for (auto& a : out) {
        if (!AccountUsable(a)) continue;
        auto it = uniq.find(a.openId);
        if (it == uniq.end() || a.loginTime >= it->second.loginTime) uniq[a.openId] = a;
    }
    out.clear();
    for (auto& kv : uniq) out.push_back(std::move(kv.second));
    SortAccountsFirst(out);
    Log(log, L"[gamapass-http] 已从浏览器导入 " + std::to_wstring(out.size()) +
                 L" 个 Gama Pass 账号（将选第一项）");
    return !out.empty();
}

// ★ 硬红线：禁止调用 refresh/token。服务端轮换 refreshToken 会立刻让浏览器 LS 免登录失效。
bool RefreshUserToken(msc::http::Client& /*http*/, GamaPassAccount& /*acc*/,
                      const HttpLoginLogFn& log) {
    Log(log, L"[gamapass-http] 已禁止 refresh/token（保护浏览器免登录态不被冲掉）");
    return false;
}

bool TryPickFreshUserToken(msc::http::Client& /*http*/, GamaPassSessionFile& store,
                           GamaPassAccount& picked, const HttpLoginLogFn& log) {
    const size_t n = store.accounts.size();
    for (size_t i = 0; i < n; ++i) {
        auto& a = store.accounts[i];
        if (!AccountUsable(a)) continue;
        if (!UserTokenStillFresh(a.userToken)) {
            Log(log, L"[gamapass-http] 账号 " + std::to_wstring(i + 1) + L"/" +
                         std::to_wstring(n) + L" id=" + WidenUtf8(MaskOpenId(a.openId)) +
                         L" 的 userToken 已过期/缺失；跳过（不调用 refresh）");
            continue;
        }
        // 补齐 openId/deviceId（JWT claims）
        GamaPassAccount tryAcc = a;
        std::string sub, dev;
        if (ParseJwtClaims(tryAcc.userToken, sub, dev)) {
            if (!sub.empty()) tryAcc.openId = sub;
            if (!dev.empty()) tryAcc.deviceId = dev;
        }
        if (tryAcc.openId.empty() || tryAcc.deviceId.empty()) {
            Log(log, L"[gamapass-http] 账号缺 openId/deviceId，跳过");
            continue;
        }
        picked = tryAcc;
        UpsertAccount(store, tryAcc);
        SaveSessionFile(store);
        Log(log, L"[gamapass-http] 复用浏览器/本地 userToken（未调用 refresh）第 " +
                     std::to_wstring(i + 1) + L"/" + std::to_wstring(n) + L" 项 id=" +
                     WidenUtf8(MaskOpenId(picked.openId)) +
                     (picked.email.empty() ? L"" : (L" " + WidenUtf8(MaskEmail(picked.email)))));
        return true;
    }
    return false;
}

bool ResolveSessionAccount(msc::http::Client& http, GamaPassAccount& picked,
                           const HttpLoginLogFn& log) {
    GamaPassSessionFile store;
    LoadSessionFile(store);

    auto mergeImport = [&]() {
        std::vector<GamaPassAccount> imported;
        if (!ImportFromBrowsers(imported, log)) return false;
        for (auto& a : imported) UpsertAccount(store, a);
        SaveSessionFile(store);
        return !store.accounts.empty();
    };

    if (store.accounts.empty()) {
        mergeImport();
    }
    if (!store.accounts.empty() && TryPickFreshUserToken(http, store, picked, log)) {
        return true;
    }

    // 本地 userToken 过期：再扫浏览器（用户若刚在网页活动过，LS 会有新 token）
    Log(log, L"[gamapass-http] 本地 userToken 不可用，重新扫描 Chrome/Edge（仍不调用 refresh）…");
    if (mergeImport() && TryPickFreshUserToken(http, store, picked, log)) {
        return true;
    }

    Log(log, L"[gamapass-http] 无可用 userToken。请用 Chrome 自己打开 "
             L"accounts.gamania.com 选号页（勾选记住）刷新网页会话后，再点启动。"
             L"本程序绝不调用 refresh/token、也绝不自动打开 OpenID，以免冲掉免登录。");
    return false;
}

bool HasUsableSessionImpl() {
    GamaPassSessionFile store;
    if (LoadSessionFile(store)) {
        for (const auto& a : store.accounts) {
            if (AccountUsable(a) && UserTokenStillFresh(a.userToken)) return true;
        }
    }
    std::vector<GamaPassAccount> imported;
    if (!ImportFromBrowsers(imported, nullptr)) return false;
    for (const auto& a : imported) {
        if (AccountUsable(a) && UserTokenStillFresh(a.userToken)) return true;
    }
    return false;
}

}  // namespace

bool HttpGamaPassHasUsableSession() { return HasUsableSessionImpl(); }

bool HttpGamaPassPreferredBrowserExe(std::wstring& outExe) { return PreferredBrowserExeImpl(outExe); }

bool HttpGamaPassResolveUserDataDir(const std::wstring& exePath, std::wstring& outUserDataDir) {
    outUserDataDir.clear();
    std::vector<std::wstring> bases;
    CollectUserDataBasesNearExe(exePath, bases);
    for (const auto& b : bases) {
        // 优先像 User Data 的根：含 Default 子目录
        if (DirExistsW(b + L"\\Default") || DirExistsW(b + L"\\User Data\\Default")) {
            outUserDataDir = DirExistsW(b + L"\\User Data\\Default") ? (b + L"\\User Data") : b;
            return true;
        }
    }
    if (!bases.empty()) {
        outUserDataDir = bases.front();
        return true;
    }
    return false;
}

HttpLoginResult HttpGamaPassLoginToOtt(const std::wstring& /*user*/, const std::wstring& /*pass*/,
                                       HttpLoginLogFn log, int timeoutMs) {
    msc::http::Client http(timeoutMs);
    http.ClearCookies();

    // ⓪ 先确认浏览器/本地 refresh 可用，再碰 Galaxy/OpenID。
    // 无会话时空跑 OpenID 会在服务端留下短时 OAuth 态，用户再开浏览器易「登入流程逾時(01004)」。
    std::string userToken;
    std::string deviceId;
    {
        GamaPassAccount sess;
        if (!ResolveSessionAccount(http, sess, log)) {
            return Fail(HttpLoginError::BadInput,
                        "未找到可用的 Gama Pass userToken（未过期）。请用 Chrome 打开 "
                        "accounts.gamania.com 选号页勾选记住以刷新网页会话，再启动。"
                        "本模式不调用 refresh/token、不自动打开登录页，以免冲掉免登录。");
        }
        userToken = sess.userToken;
        deviceId = sess.deviceId;
    }
    if (deviceId.empty()) deviceId = NewUuid();

    // ① Galaxy 会话 OTT
    Log(log, L"[gamapass-http] GET Galaxy login/mstc 取会话 OTT…");
    auto gLogin = http.Get(kGalaxyLoginMstc, true);
    std::wstring loginOtt =
        ExtractLoginSessionOtt(gLogin.redirectChain, gLogin.finalUrl, gLogin.body);
    if (loginOtt.empty()) {
        return Fail(HttpLoginError::Protocol,
                    "未拿到 Galaxy 登录会话 OTT（/login/init/mstc/OTT:…）");
    }
    Log(log, L"[gamapass-http] sessionOtt=" + RedactOtt(loginOtt));

    // ② 跟随 OpenID → 捕获 oauth2/authorize query（state/nonce/scope…）
    Log(log, L"[gamapass-http] GET Gama Pass OpenID，解析 OAuth 参数…");
    auto openid = http.Get(kGamaPassOpenId, true);
    std::wstring authUrl = openid.finalUrl;
    if (authUrl.find(L"/oauth2/authorize") == std::wstring::npos) {
        for (auto it = openid.redirectChain.rbegin(); it != openid.redirectChain.rend(); ++it) {
            if (it->find(L"/oauth2/authorize") != std::wstring::npos) {
                authUrl = *it;
                break;
            }
        }
    }
    if (authUrl.find(L"/oauth2/authorize") == std::wstring::npos) {
        Log(log, L"[gamapass-http] FAIL 未到达 oauth2/authorize；final=" + openid.finalUrl);
        return Fail(HttpLoginError::Protocol, "未捕获 Gama Pass OAuth authorize 参数");
    }
    auto q = ParseQuery(authUrl);
    auto need = [&](const char* k) -> std::string {
        auto it = q.find(k);
        return it == q.end() ? std::string{} : UrlDecode(it->second);
    };
    const std::string clientId = need("client_id").empty() ? kGtwClientId : need("client_id");
    const std::string redirectUri = need("redirect_uri");
    const std::string state = need("state");
    const std::string nonce = need("nonce");
    const std::string scope = need("scope");
    // 与 CDP 红线一致：不把 prompt=login 改写成 none（冲刷 SSO / 制造须重登态）。
    // URL 未带 prompt 时默认 none（已有 userToken 的 authorize 语义）。
    const std::string promptUse = need("prompt").empty() ? "none" : need("prompt");
    const std::string requiredInfo = need("required_info");
    const std::string provider = need("provider");
    const std::string providerMigrate = need("provider_migrate_redirect_uri");
    if (redirectUri.empty() || state.empty()) {
        return Fail(HttpLoginError::Protocol, "OAuth authorize 缺少 redirect_uri/state");
    }
    Log(log, L"[gamapass-http] OAuth client 已解析");

    // ⑤ Nuxt oauth2/authorize → redirect uri（含 code）
    {
        Log(log, L"[gamapass-http] POST oauth2/authorize…");
        const std::string serverTok = MakeServerSideAuthToken();
        if (serverTok.empty()) {
            return Fail(HttpLoginError::Protocol, "生成 OAuth server-side token 失败");
        }
        std::ostringstream ob;
        ob << "{"
           << "\"response_type\":\"code\","
           << "\"prompt\":\"" << JsonEscape(promptUse) << "\","
           << "\"client_id\":\"" << JsonEscape(clientId) << "\","
           << "\"scope\":\"" << JsonEscape(scope) << "\","
           << "\"redirect_uri\":\"" << JsonEscape(redirectUri) << "\","
           << "\"state\":\"" << JsonEscape(state) << "\","
           << "\"nonce\":\"" << JsonEscape(nonce) << "\",";
        if (!requiredInfo.empty())
            ob << "\"required_info\":\"" << JsonEscape(requiredInfo) << "\",";
        if (!provider.empty()) ob << "\"provider\":\"" << JsonEscape(provider) << "\",";
        if (!providerMigrate.empty())
            ob << "\"provider_migrate_redirect_uri\":\"" << JsonEscape(providerMigrate) << "\",";
        ob << "\"device_id\":\"" << JsonEscape(deviceId) << "\","
           << "\"token\":\"Bearer " << JsonEscape(userToken) << "\""
           << "}";

        std::vector<std::pair<std::wstring, std::wstring>> oauthHeaders = {
            {L"Authorization", WidenUtf8(serverTok)},
            {L"x-client-id", WidenUtf8(clientId)},
            {L"x-device-id", WidenUtf8(deviceId)},
            {L"x-track-id", WidenUtf8(NewUuid())},
            {L"x-client-path-name", L"/login/finished"},
            {L"Origin", L"https://accounts.gamania.com"},
            {L"Referer", L"https://accounts.gamania.com/login/finished"},
            {L"Accept", L"application/json"},
        };
        auto ar = http.PostJson(kOauthAuthorizeApi, ob.str(), true,
                                L"https://accounts.gamania.com/login/finished", oauthHeaders);
        std::string uri = JsonString(ar.body, "uri");
        if (uri.empty()) uri = JsonString(ar.body, "url");
        if (uri.empty()) {
            Log(log, L"[gamapass-http] FAIL authorize status=" + std::to_wstring(ar.status) +
                         L" snip=" + WidenUtf8(ar.body.substr(0, 240)));
            if (ar.status == 401 || ar.status == 403) {
                return Fail(HttpLoginError::Protocol,
                            "Gama Pass OAuth authorize 被拒绝（请在浏览器重新登录 accounts.gamania.com）");
            }
            return Fail(HttpLoginError::Protocol, "Gama Pass OAuth authorize 未返回 uri");
        }
        Log(log, L"[gamapass-http] authorize uri ok，跟随换 WebToken…");

        // ⑦ 跟随 uri → WebToken（中间可能停在 SelectGameAccount 选游戏昵称）
        auto jump = http.Get(WidenUtf8(uri), true);
        std::string webToken = ExtractWebTokenFromJump(jump);
        if (webToken.empty()) {
            msc::http::Response afterSelect;
            if (TrySubmitSelectGameAccount(http, jump, log, afterSelect)) {
                jump = std::move(afterSelect);
                webToken = ExtractWebTokenFromJump(jump);
                // 偶发二次选号页：再试一次
                if (webToken.empty()) {
                    msc::http::Response again;
                    if (TrySubmitSelectGameAccount(http, jump, log, again)) {
                        jump = std::move(again);
                        webToken = ExtractWebTokenFromJump(jump);
                    }
                }
            } else if (UrlHasSelectGameAccount(jump.finalUrl)) {
                Log(log, L"[gamapass-http] FAIL 停在 SelectGameAccount 且无法解析表单；final=" +
                             jump.finalUrl);
                return Fail(HttpLoginError::OttMissing,
                            "Gama Pass 需要选择游戏昵称，但页面无法自动提交（请在浏览器点一次第一项昵称后重试）");
            }
        }
        if (webToken.empty()) {
            Log(log, L"[gamapass-http] FAIL 未拿到 WebToken；final=" + jump.finalUrl);
            return Fail(HttpLoginError::OttMissing, "Gama Pass 登录后未拿到 Galaxy WebToken");
        }
        Log(log, L"[gamapass-http] WebToken ok len=" + std::to_wstring(webToken.size()));

        // ⑧ POST result/mstc/beanfun
        const std::wstring resultUrl =
            std::wstring(kGalaxyResultBeanfun) + L"?WebToken=" + WidenUtf8(webToken);
        std::string csrf;
        {
            auto rgGet = http.Get(resultUrl, true);
            csrf = msc::http::RegexGroup1(
                rgGet.body, "name=[\"']csrf-token[\"']\\s+content=[\"']([^\"']+)[\"']");
            if (csrf.empty()) {
                csrf = msc::http::RegexGroup1(rgGet.body, "\"csrfToken\"\\s*:\\s*\"([^\"]+)\"");
            }
        }
        std::string resultBody =
            std::string("{\"ott\":\"") + JsonEscape(NarrowUtf8(loginOtt)) + "\",\"fromSelf\":true";
        if (!csrf.empty()) resultBody += ",\"_token\":\"" + JsonEscape(csrf) + "\"";
        resultBody += "}";
        std::vector<std::pair<std::wstring, std::wstring>> extra;
        if (!csrf.empty()) {
            const std::wstring csrfW = WidenUtf8(csrf);
            extra.emplace_back(L"X-CSRF-TOKEN", csrfW);
            extra.emplace_back(L"RequestVerificationToken", csrfW);
        }
        Log(log, L"[gamapass-http] POST Galaxy result/mstc/beanfun…");
        auto rg = http.PostJson(resultUrl, resultBody, true, resultUrl, extra);
        std::wstring ott = HttpParseGalaxyResultOtt(rg.body);
        if (ott.empty()) {
            for (const auto& u : rg.redirectChain) {
                std::wstring t = ExtractOttToken(u);
                if (IsTicketOtt(t)) {
                    ott = t;
                    break;
                }
            }
        }
        if (ott.empty()) {
            auto mainGet = http.Get(std::wstring(kClassicMain) + L"?WebToken=" + WidenUtf8(webToken),
                                    true);
            for (const auto& u : mainGet.redirectChain) {
                std::wstring t = ExtractOttToken(u);
                if (IsTicketOtt(t)) {
                    ott = t;
                    break;
                }
            }
            if (ott.empty()) {
                std::wstring t = ExtractOttToken(mainGet.finalUrl);
                if (IsTicketOtt(t)) ott = t;
            }
        }
        if (ott.empty() || !IsTicketOtt(ott)) {
            Log(log, L"[gamapass-http] FAIL ticketOtt 未就绪 body_snip=" +
                         WidenUtf8(rg.body.substr(0, 220)));
            return Fail(HttpLoginError::OttMissing,
                        "Gama Pass 登录成功但未捕获换票 OTT；请在浏览器重新登录后重试");
        }
        Log(log, L"[gamapass-http] ticketOtt=" + RedactOtt(ott) + L" → GetOneTimeWebInfo…");

        TicketFetchOptions fo;
        fo.ott = ott;
        fo.timeoutMs = timeoutMs;
        auto fr = FetchGalaxyTicketFromOtt(fo);
        HttpLoginResult out;
        out.ott = ott;
        if (!fr.ok) {
            out.ok = false;
            out.error = HttpLoginError::Protocol;
            out.message = fr.message.empty() ? "GetOneTimeWebInfo 失败" : fr.message;
            return out;
        }
        out.ok = true;
        out.error = HttpLoginError::Ok;
        out.message = "ok";
        out.ticket = std::move(fr.ticket);
        out.ticketFilled = true;
        return out;
    }
}

}  // namespace msc::launcher
