#pragma once

// payload → launcher 通知：Local\\XCatNotify_<hash> SHM；旧 notify.bin 只读 migrate。

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace xcat {

constexpr uint32_t kPayloadNotifyMagic = 0x5446594Eu;  // 'XFTN'
constexpr uint32_t kPayloadNotifyVersion = 2u;
constexpr size_t kPayloadNotifyCapacity = 48u;
constexpr size_t kPayloadNotifyKeyLen = 48u;
constexpr size_t kPayloadNotifyTitleLen = 64u;
constexpr size_t kPayloadNotifyBodyLen = 256u;

enum PayloadNotifyKind : uint32_t {
    kPayloadNotifyInfo = 0,
    kPayloadNotifySuccess = 1,
    kPayloadNotifyWarning = 2,
    kPayloadNotifyDanger = 3,
};

#pragma pack(push, 1)
struct PayloadNotifyEvent {
    uint32_t seq = 0;
    uint32_t kind = kPayloadNotifyInfo;
    uint32_t ttlMs = 4200;
    char key[kPayloadNotifyKeyLen]{};
    char title[kPayloadNotifyTitleLen]{};
    char body[kPayloadNotifyBodyLen]{};
};

struct PayloadNotifyQueue {
    uint32_t magic = kPayloadNotifyMagic;
    uint32_t version = kPayloadNotifyVersion;
    uint32_t nextSeq = 1;
    uint32_t head = 0;
    uint32_t epoch = 1;
    PayloadNotifyEvent slots[kPayloadNotifyCapacity]{};
};
#pragma pack(pop)

std::string PayloadNotifyRelPath();
std::string PayloadNotifyPath(const char* binDir);

bool ResetPayloadNotifyQueue(const char* binDir);
bool PeekPayloadNotifyEpoch(const char* binDir, uint32_t* outEpoch);
void SkipPayloadNotifyBacklog(const char* binDir, uint32_t* inOutLastSeq);
// ttlMs = kPayloadNotifyDismissTtlMs 表示撤掉同 key 的现有气泡（不播提示音、不记事件）。
constexpr uint32_t kPayloadNotifyDismissTtlMs = 0xFFFFFFFFu;

bool EnqueuePayloadNotify(const char* binDir, uint32_t kind, const char* key, const char* title,
                            const char* body, uint32_t ttlMs);
size_t DrainPayloadNotify(const char* binDir, uint32_t* inOutLastSeq, PayloadNotifyEvent* out,
                          size_t maxEvents);

}  // namespace xcat
