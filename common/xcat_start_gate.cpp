#include "xcat_start_gate.h"

#include "xcat_start_gate_secret.h"

#include "process_util.h"
#include "xcat_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shlobj.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace xcat {
namespace gate {
namespace {

// ---- base64url 解码 ----------------------------------------------------------

int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;  // base64url
    if (c == '_') return 63;  // base64url
    return -1;
}

bool B64UrlDecode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        const int v = B64Val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string B64UrlEncode(const uint8_t* data, size_t len) {
    static const char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kAlpha[(v >> 18) & 63]);
        out.push_back(kAlpha[(v >> 12) & 63]);
        out.push_back(kAlpha[(v >> 6) & 63]);
        out.push_back(kAlpha[v & 63]);
        i += 3;
    }
    // base64url 不补 '='（与服务端 Buffer.toString("base64url") 一致）。
    if (i + 1 == len) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kAlpha[(v >> 18) & 63]);
        out.push_back(kAlpha[(v >> 12) & 63]);
    } else if (i + 2 == len) {
        const uint32_t v =
            (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlpha[(v >> 18) & 63]);
        out.push_back(kAlpha[(v >> 12) & 63]);
        out.push_back(kAlpha[(v >> 6) & 63]);
    }
    return out;
}

// ---- SHA-256（内存缓冲）------------------------------------------------------

bool Sha256Mem(const uint8_t* data, size_t len, uint8_t digest[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = 0;
    std::vector<uint8_t> obj;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                          sizeof(objLen), &cb, 0) == 0) {
        obj.resize(objLen);
        if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0 &&
                BCryptFinishHash(hash, digest, 32, 0) == 0) {
                ok = true;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// HMAC-SHA256：密钥用卡的签名段原字节，是只有持卡人与签发方知道的共享秘密。
bool HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                uint8_t mac[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = 0;
    std::vector<uint8_t> obj;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0 &&
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                          sizeof(objLen), &cb, 0) == 0) {
        obj.resize(objLen);
        if (BCryptCreateHash(alg, &hash, obj.data(), objLen, const_cast<PUCHAR>(key),
                             static_cast<ULONG>(keyLen), 0) == 0) {
            if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0 &&
                BCryptFinishHash(hash, mac, 32, 0) == 0) {
                ok = true;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- ECDSA P-256 验签（内嵌公钥）--------------------------------------------

bool EcdsaP256Verify(const uint8_t hash[32], const uint8_t* sig, size_t sigLen) {
    if (sigLen != 64) return false;  // IEEE P1363 r||s
    // 组 BCRYPT_ECCPUBLIC_BLOB：头 { dwMagic, cbKey=32 } + X(32) + Y(32)。
    struct Blob {
        BCRYPT_ECCKEY_BLOB header;
        uint8_t xy[64];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    memcpy(blob.xy, kGatePubKeyXY, 64);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0) == 0) {
            const NTSTATUS st = BCryptVerifySignature(
                key, nullptr, const_cast<PUCHAR>(hash), 32,
                const_cast<PUCHAR>(sig), static_cast<ULONG>(sigLen), 0);
            ok = (st == 0);
        }
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- 极简 JSON 字段提取（仅用于我方自签的 payload）--------------------------

std::string JsonStr(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) ++p;  // 跳过转义
        out.push_back(json[p++]);
    }
    return out;
}

long long JsonNum(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return 0;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    bool neg = false;
    if (p < json.size() && json[p] == '-') {
        neg = true;
        ++p;
    }
    long long v = 0;
    bool any = false;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
        v = v * 10 + (json[p] - '0');
        ++p;
        any = true;
    }
    if (!any) return 0;
    return neg ? -v : v;
}

// ---- 激活缓存（混淆双写，魔数 SG1\0）----------------------------------------
//
// kCacheGeneration 写入缓存 JSON 的 "v"。读路径认 1 和当前值：全员锤过一次之后改成
// 「服务端认不出才重贴」，旧 v:1 必须继续免输。

constexpr int kCacheGeneration = 2;

constexpr uint8_t kMagic[4] = {'S', 'G', '1', 0};
constexpr uint8_t kXorKey[16] = {0x3B, 0xE9, 0x14, 0x7C, 0xA2, 0x5D, 0x88, 0xF1,
                                 0x46, 0x0B, 0xD3, 0x9E, 0x27, 0x60, 0xBC, 0x51};

std::vector<uint8_t> Obfuscate(const std::string& plain) {
    std::vector<uint8_t> out;
    out.reserve(plain.size() + 4);
    out.insert(out.end(), kMagic, kMagic + 4);
    for (size_t i = 0; i < plain.size(); ++i) {
        const uint8_t b = static_cast<uint8_t>(plain[i]);
        out.push_back(b ^ kXorKey[i % 16] ^ static_cast<uint8_t>(i * 7 + 13));
    }
    return out;
}

bool Deobfuscate(const std::vector<uint8_t>& buf, std::string& plain) {
    if (buf.size() < 4 || memcmp(buf.data(), kMagic, 4) != 0) return false;
    plain.clear();
    for (size_t i = 4; i < buf.size(); ++i) {
        const size_t j = i - 4;
        const uint8_t b = buf[i] ^ kXorKey[j % 16] ^ static_cast<uint8_t>(j * 7 + 13);
        plain.push_back(static_cast<char>(b));
    }
    return true;
}

std::string MachineCachePath() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &base)) ||
        !base) {
        return {};
    }
    std::wstring w = base;
    CoTaskMemFree(base);
    if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w.push_back(L'\\');
    w += L"{E4B7C2A9-1F8D-4E3A-9C6B-7A2D5F1E0C8B}\\sg.dat";
    return WideToUtf8(w);
}

std::string InstallCachePath(const std::string& payloadBinDir) {
    if (payloadBinDir.empty()) return {};
    std::string path = payloadBinDir;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "state\\sg.cache";
    return path;
}

void EnsureParentDir(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return;
    const std::wstring dir = Utf8ToWide(path.substr(0, slash));
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& buf) {
    if (path.empty()) return false;
    EnsureParentDir(path);
    HANDLE h = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == buf.size();
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& buf) {
    if (path.empty()) return false;
    HANDLE h = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    buf.clear();
    uint8_t chunk[4096];
    DWORD read = 0;
    while (ReadFile(h, chunk, sizeof(chunk), &read, nullptr) && read) {
        buf.insert(buf.end(), chunk, chunk + read);
        if (buf.size() > 64 * 1024) break;  // TOKEN 缓存很小，防异常大文件
    }
    CloseHandle(h);
    return !buf.empty();
}

std::string BuildCachePlain(const std::string& token, const std::string& deviceId,
                            const TokenClaims& claims) {
    char buf[64];
    char genBuf[32];
    std::snprintf(genBuf, sizeof(genBuf), "{\"v\":%d,\"uid\":\"", kCacheGeneration);
    std::string out = genBuf;
    out += claims.uid;
    out += "\",\"deviceId\":\"";
    out += deviceId;
    out += "\",\"token\":\"";
    out += token;
    std::snprintf(buf, sizeof(buf), "\",\"at\":%lld,\"exp\":%lld}",
                  static_cast<long long>(time(nullptr)), claims.exp);
    out += buf;
    return out;
}

void WriteActivationCache(const std::string& payloadBinDir, const std::string& token,
                          const std::string& deviceId, const TokenClaims& claims) {
    const std::vector<uint8_t> blob = Obfuscate(BuildCachePlain(token, deviceId, claims));
    const bool m = WriteFileBytes(MachineCachePath(), blob);
    const bool l = WriteFileBytes(InstallCachePath(payloadBinDir), blob);
    if (!m && !l) {
        xcat::log::Warn("Auth", "gate/1 cache write failed (both paths)");
    }
}

// 从缓存读出有效的 TOKEN：deviceId 必须匹配、TOKEN 必须仍验签通过。返回空=无有效缓存。
std::string ReadValidCachedToken(const std::string& payloadBinDir,
                                 const std::string& currentDeviceId) {
    const std::string paths[] = {MachineCachePath(), InstallCachePath(payloadBinDir)};
    for (const auto& p : paths) {
        std::vector<uint8_t> buf;
        std::string plain;
        if (!ReadFileBytes(p, buf) || !Deobfuscate(buf, plain)) continue;
        const long long gen = JsonNum(plain, "v");
        if (gen != 1 && gen != kCacheGeneration) {
            static bool loggedStale = false;
            if (!loggedStale) {
                loggedStale = true;
                xcat::log::Info("Auth",
                                "gate/1 cache generation unknown (have=%lld); ignore", gen);
            }
            continue;
        }
        const std::string cachedDevice = JsonStr(plain, "deviceId");
        const std::string token = JsonStr(plain, "token");
        if (token.empty()) continue;
        if (!currentDeviceId.empty() && cachedDevice != currentDeviceId) continue;
        TokenClaims claims;
        if (VerifyToken(token, claims)) return token;
    }
    return {};
}

}  // namespace

// ---- 对外接口 ----------------------------------------------------------------

bool VerifyToken(const std::string& token, TokenClaims& out) {
    const size_t dot = token.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= token.size()) return false;
    const std::string payloadB64 = token.substr(0, dot);
    const std::string sigB64 = token.substr(dot + 1);

    std::vector<uint8_t> sig;
    if (!B64UrlDecode(sigB64, sig) || sig.size() != 64) return false;

    uint8_t hash[32];
    if (!Sha256Mem(reinterpret_cast<const uint8_t*>(payloadB64.data()), payloadB64.size(), hash)) {
        return false;
    }
    if (!EcdsaP256Verify(hash, sig.data(), sig.size())) return false;

    std::vector<uint8_t> payloadBytes;
    if (!B64UrlDecode(payloadB64, payloadBytes)) return false;
    const std::string payload(payloadBytes.begin(), payloadBytes.end());

    TokenClaims claims;
    claims.uid = JsonStr(payload, "uid");
    claims.iss = JsonNum(payload, "iss");
    claims.exp = JsonNum(payload, "exp");
    if (claims.uid.empty()) return false;
    if (claims.exp > 0 && static_cast<long long>(time(nullptr)) >= claims.exp) return false;

    out = claims;
    return true;
}

std::string LoadActivatedGateToken(const std::string& payloadBinDir,
                                   const std::string& currentDeviceId) {
    return ReadValidCachedToken(payloadBinDir, currentDeviceId);
}

// 派生凭证：整张卡永不上网，只发 payload + 时间戳 + HMAC(卡签名段, payload|ts|deviceId)。
// 抓包者拿到的凭证绑死了这个 ts 与这台 deviceId，出窗即废，也推不出卡签名 → 造不出新凭证。
std::string BuildGateProof(const std::string& payloadBinDir, const std::string& deviceId,
                           long long unixSec) {
    const std::string token = ReadValidCachedToken(payloadBinDir, deviceId);
    if (token.empty()) return {};
    const size_t dot = token.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= token.size()) return {};
    const std::string payloadB64 = token.substr(0, dot);
    const std::string sigB64 = token.substr(dot + 1);

    std::vector<uint8_t> sig;
    if (!B64UrlDecode(sigB64, sig) || sig.size() != 64) return {};

    char tsBuf[24]{};
    std::snprintf(tsBuf, sizeof(tsBuf), "%lld", unixSec);
    const std::string msg = payloadB64 + "|" + tsBuf + "|" + deviceId;

    uint8_t mac[32]{};
    if (!HmacSha256(sig.data(), sig.size(), reinterpret_cast<const uint8_t*>(msg.data()),
                    msg.size(), mac)) {
        return {};
    }
    return payloadB64 + "." + tsBuf + "." + B64UrlEncode(mac, sizeof(mac));
}

bool HasValidActivation(const std::string& payloadBinDir, const std::string& deviceId) {
    TokenClaims claims;
    const std::string cached = ReadValidCachedToken(payloadBinDir, deviceId);
    if (cached.empty() || !VerifyToken(cached, claims)) return false;
    xcat::log::Info("Auth", "gate/1 pass (cached uid=%s)", claims.uid.c_str());
    return true;
}

void InvalidateActivationCache(const std::string& payloadBinDir) {
    const std::string paths[] = {MachineCachePath(), InstallCachePath(payloadBinDir)};
    for (const auto& p : paths) {
        if (p.empty()) continue;
        const std::wstring w = Utf8ToWide(p);
        const DWORD attr = GetFileAttributesW(w.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (DeleteFileW(w.c_str())) {
            xcat::log::Info("Auth", "gate/1 cache invalidated");
        } else {
            xcat::log::Warn("Auth", "gate/1 cache invalidate failed err=%lu",
                            static_cast<unsigned long>(GetLastError()));
        }
    }
}

bool CommitActivation(const std::string& payloadBinDir, const std::string& deviceId,
                      const std::string& token, TokenClaims& out) {
    TokenClaims claims;
    if (!VerifyToken(token, claims)) {
        xcat::log::Warn("Auth", "gate/1 reject (invalid/expired token)");
        return false;
    }
    WriteActivationCache(payloadBinDir, token, deviceId, claims);
    xcat::log::Info("Auth", "gate/1 activated (uid=%s exp=%lld)", claims.uid.c_str(), claims.exp);
    out = claims;
    return true;
}

}  // namespace gate
}  // namespace xcat
