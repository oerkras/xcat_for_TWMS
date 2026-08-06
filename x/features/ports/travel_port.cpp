// Classic TWMS travel_port ??PortalManager / MapData / ???? / ???? / ??????
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "travel_port.h"

#include "foothold_path.h"
#include "teleport_port.h"
#include "world_port.h"
#include "../fly/fly.h"
#include "../invuln/invuln.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/anchor_lamps.h"
#include "../../ui/player_vitals.h"
#include "input_port.h"

#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace x::features::ports::travel {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr char kPortalManagerClass[] =
    "e67f9ad37cf404d09bae32979e6e59ad535a364232b52dcf9a1d59458a0ed91";  // remounted 2026-08-06
// WM / UserLocal / NM → il2cpp_shape Resolve*Klass（hash + shape）
// SEND OutPacket TypeDef 13775（勿用 13774 InPacket / b980769a…）
constexpr char kOutPacketClass[] =
    "b2cb1e0adcf26c5021bc6b1880a32e838d1eb783e3880f4a70e70990079a04b";
// remounted 2026-08-06 · dump.cs / script.json
constexpr char kHashCheckMovePortal[] =
    "f480d61f81686460994a568f0a78aaf5e4ae9c12fe009658ba7af2517c58eb3";
constexpr char kHashOutCreate[] =
    "d5cef5f625ea2385cd9eaaf8b9a49342353732f8534da040cfa123e58f0ed27";
constexpr char kHashEncode1[] =
    "e8c10cdad1bc8d76acb9eec60662b480fca2022b3f2e7c27220303c60151bde";  // Encode1(byte)
constexpr char kHashEncodeStr[] =
    "c9ea89f993b612dbfdc4fffe6486b28a58cfd581c4c3d2af5262c85dd5a4395";
constexpr char kHashSendPacket[] =
    "ddc1a3d2b1ecceba615002a4805504bc8dc6096ad3706c3d16a06875bd4de28";  // Session bool(OutPacket)

// Unity FindAll / get_gameObject / get_name → x::runtime::il2cpp::kRva*（il2cpp_bind.h SSOT）
constexpr uint32_t kRvaCheckMovePortal = 0xDCC6D0;  // remounted 2026-08-06 WM.CheckMovePortal
constexpr uint32_t kRvaOutPacketCreate = 0x1CC63D0;  // remounted 2026-08-06 OutPacket.Create
constexpr uint32_t kRvaOutPacketEncode1Byte = 0x1CD2AE0;  // remounted 2026-08-06 Encode1(byte)
constexpr uint32_t kRvaOutPacketEncodeStr = 0x1CD31F0;  // remounted 2026-08-06 EncodeStr
constexpr uint32_t kRvaNmSend = 0x1CC7FE0;  // remounted 2026-08-06 Session.SendPacket bool
// CMS ClientPacket.UserPortalTeleportRequest = 114 · wire 0x0072
constexpr int kClientPortalTeleport = 114;
constexpr uint16_t kWirePortalTeleport = 0x0072;

constexpr size_t kOffCachedPtr = 0x10;
#define kOffPmPortalList (x::runtime::il2cpp_mapdata::OffPmPortalList())
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffArrLen (x::runtime::il2cpp_container::OffArrayMaxLength())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())
#define kOffWmMapData (x::runtime::il2cpp_mapdata::OffWmMapData())
#define kOffMapId (x::runtime::il2cpp_mapdata::OffMapId())
#define kOffMapPortals (x::runtime::il2cpp_mapdata::OffMapPortals())
#define kOffWmMyUser (x::ui::player::OffWmMyUser())

constexpr char kMapPortalDataClass[] =
    "e33e43ed48276e30b1ffac1e543e2d7f439e5b5709b9e8b3140fdac25a6bb60";  // remounted 2026-08-06
constexpr char kPortalClass[] =
    "d0a22aa37d18c7ac1fb75be043fa2115169e2c7410e110876a1f5d34004f486";  // remounted 2026-08-06
constexpr char kActorBaseClass[] =
    "edc85ce203606bdb549e5fb94458b1d2d11ce78034d24d41e39a54c0288d38e";  // = teleport_port
constexpr char kVecCtrlClass[] =
    "e0eb55b82f10cb9eeb9424eb3aadf1450a014afa564bc55c3739b2909abfbbc";  // = teleport_port
constexpr char kPacketClass[] =
    "b374f35823e074687fd2a9225e7738d9b8b664c18aed556fc7835da03f2bad1";  // Packet base 13773

constexpr char kHashPortalData[] =
    "bbe42ed7f0f97903601a2a6124e43ee9439f5966c25a7ddc7c9d0bfcb39ec1b";
constexpr char kHashMpdId[] =
    "<ad9a043484d1666fc6beef64680b7208afecfc60044e7bd92f4f2f85a72e174>k__BackingField";
constexpr char kHashMpdType[] =
    "<c70d5285f0b73da84151d639d00281113d897c7a3b95716c045ae40c8a17345>k__BackingField";
constexpr char kHashMpdEnable[] =
    "<a4289b27033bae7778f40426f264efb62d877cc244b3008a7c8ab0cd0cae7c5>k__BackingField";
constexpr char kHashMpdPName[] =
    "<ac7608e244c4a4ad5ab2c8f4ae620ce34c8fa3370f803a04a682dcbb0aa7dad>k__BackingField";
constexpr char kHashMpdX[] =
    "<a6fcd2442fb866867177acd414a548e6a39cfe7b975d1fb4f3a8249ca499aa9>k__BackingField";
constexpr char kHashMpdY[] =
    "<e6e9f7534c7873ef275b8322f860a8fab3fe54e149dfd0e968b8f8071778103>k__BackingField";
constexpr char kHashMpdToMapId[] =
    "<c752ecd5869737e7695b94c3efc683bbc4add8ab1a158fbf0134d1abdcd7ece>k__BackingField";
// MapPortalData 字段偏移未漂；hash remount 2026-08-06 dump.cs TypeDef 2079
constexpr char kHashMpdPortalRect[] =
    "<b7eda6420d187bddc78bfa953700f2e07f13886728996dc99d8fa75b41d0a80>k__BackingField";
constexpr char kHashMpdHRange[] =
    "<ca5ca41653dfe9237dda1d3bae61d17374303c0ffc1fb14935cdfce335dbd6b>k__BackingField";
constexpr char kHashMpdVRange[] =
    "<af31f84bba631be21538593d480729aae05f73bd1d4a159a52971b7d7cb49b5>k__BackingField";
constexpr char kHashMpdVImpact[] =
    "<f7253c0afdbbb613471491479ebf75a254b7b4fd456edf4ee2096c4eb41f950>k__BackingField";
constexpr char kHashMpdHImpact[] =
    "<a8112a6f65f4705a1c24feb059524b57d8932575337e639a021adb4adaeb348>k__BackingField";
constexpr char kHashWmFieldKey[] =
    "b4ba3b6c23175b5e7a2099cdc465f07c421273042694938c0193ea5be2a924c";  // = world_port
// Packet base 13773 buffer/offset；SEND OutPacket 13775 id@0x20（非 InPacket backing）
constexpr char kHashPacketBuffer[] =
    "<f144fe8dbde79dea20d46b23b481b820339104f066fb33eda5c77a04363b872>k__BackingField";
constexpr char kHashPacketOffset[] =
    "<a22ae0bd7de5fc24a4a31fea49b5261e154c755a12e02510fd592b6dc594841>k__BackingField";
constexpr char kHashOutPacketId[] =
    "e124ab3ffe08d49850755d299692770376cce0daf952029aeb0b5a6286398f2";
constexpr char kHashUserVecCtrl[] =
    "<acb8946a384ed398c4ad9268349397cf4f6e65cf136078ebc9aa26a949efd41>k__BackingField";
constexpr char kHashVcAp[] =
    "e558fbd3da65bf13bea9360dfa61506af709ad89f925bc16b67e7e1cdb24107";
constexpr char kHashVcApl[] =
    "b5eb27f6f80eeaea51f811969e3c5bc8a7b73b19741a8cb481b29a0082c958d";

constexpr size_t kFbPortalData = 0x10, kFbMpdId = 0x10, kFbMpdType = 0x14, kFbMpdEnable = 0x18;
constexpr size_t kFbMpdPName = 0x20, kFbMpdX = 0x28, kFbMpdY = 0x2C, kFbMpdToMapId = 0x30;
constexpr size_t kFbMpdPortalRect = 0x54, kFbMpdHRange = 0x64, kFbMpdVRange = 0x68;
constexpr size_t kFbMpdVImpact = 0x78, kFbMpdHImpact = 0x7C;
constexpr size_t kFbWmFieldKey = 0x80, kFbPacketBuffer = 0x10, kFbPacketOffset = 0x18;
constexpr size_t kFbOutPacketId = 0x20, kFbUserVecCtrl = 0x50, kFbVcAp = 0x98, kFbVcApl = 0xB8;

size_t gOffPortalData = kFbPortalData, gOffMpdId = kFbMpdId, gOffMpdType = kFbMpdType;
size_t gOffMpdEnable = kFbMpdEnable, gOffMpdPName = kFbMpdPName, gOffMpdX = kFbMpdX;
size_t gOffMpdY = kFbMpdY, gOffMpdToMapId = kFbMpdToMapId, gOffWmFieldKey = kFbWmFieldKey;
size_t gOffMpdPortalRect = kFbMpdPortalRect, gOffMpdHRange = kFbMpdHRange;
size_t gOffMpdVRange = kFbMpdVRange, gOffMpdVImpact = kFbMpdVImpact;
size_t gOffMpdHImpact = kFbMpdHImpact;
size_t gOffPacketBuffer = kFbPacketBuffer, gOffPacketOffset = kFbPacketOffset;
size_t gOffOutPacketId = kFbOutPacketId, gOffUserVecCtrl = kFbUserVecCtrl, gOffVcAp = kFbVcAp;
size_t gOffVcApl = kFbVcApl;
#define kOffPortalData (gOffPortalData)
#define kOffMpdId (gOffMpdId)
#define kOffMpdType (gOffMpdType)
#define kOffMpdEnable (gOffMpdEnable)
#define kOffMpdPName (gOffMpdPName)
#define kOffMpdX (gOffMpdX)
#define kOffMpdY (gOffMpdY)
#define kOffMpdToMapId (gOffMpdToMapId)
#define kOffMpdPortalRect (gOffMpdPortalRect)
#define kOffMpdHRange (gOffMpdHRange)
#define kOffMpdVRange (gOffMpdVRange)
#define kOffMpdVImpact (gOffMpdVImpact)
#define kOffMpdHImpact (gOffMpdHImpact)
#define kOffWmFieldKey (gOffWmFieldKey)
#define kOffPacketBuffer (gOffPacketBuffer)
#define kOffPacketOffset (gOffPacketOffset)
#define kOffOutPacketId (gOffOutPacketId)
#define kOffUserVecCtrl (gOffUserVecCtrl)
#define kOffVcApX (gOffVcAp)
#define kOffVcApY (gOffVcAp + 8)
#define kOffVcAplX (gOffVcApl)
#define kOffVcAplY (gOffVcApl + 8)
bool gTravelFieldTried = false;

bool TravelFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t lo,
                       size_t hi) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, hash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (off >= lo && off < hi) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}

void EnsureTravelFieldOff() {
    if (gTravelFieldTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gTravelFieldTried = true;
    void* mpd = x::runtime::il2cpp::FindClass("", kMapPortalDataClass);
    void* portal = x::runtime::il2cpp::FindClass("", kPortalClass);
    // WorldManager hash（与 il2cpp_shape / world_port 同源）
    constexpr char kWorldManagerClass[] =
        "acda742ab51e7e2e3003fd2b44fbc00eababde4300ef17ac35b5f4fd01bee68";
    void* wm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!wm) wm = x::runtime::il2cpp::FindClass("", kWorldManagerClass);
    void* actor = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    if (!actor) actor = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* vc = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* pkt = x::runtime::il2cpp::FindClass("", kPacketClass);
    // OutPacket.id@0x20 只在 OutPacket 子类（TDI 13775），不在 Packet 基类
    void* outPkt = x::runtime::il2cpp::FindClass("", kOutPacketClass);
    if (!outPkt) outPkt = x::runtime::il2cpp::FindClass("", "OutPacket");
    int hits = 0;
    char miss[96] = {};
    size_t missN = 0;
    auto hit = [&](bool ok, const char* tag) {
        if (ok) {
            ++hits;
            return;
        }
        if (missN + 1 < sizeof(miss)) {
            if (missN) miss[missN++] = ',';
            for (const char* p = tag; *p && missN + 1 < sizeof(miss); ++p) miss[missN++] = *p;
            miss[missN] = 0;
        }
    };
    hit(TravelFieldOffHit(portal, kHashPortalData, kFbPortalData, &gOffPortalData, 0x08, 0x40),
        "portal");
    hit(TravelFieldOffHit(mpd, kHashMpdId, kFbMpdId, &gOffMpdId, 0x08, 0x40), "mpdId");
    hit(TravelFieldOffHit(mpd, kHashMpdType, kFbMpdType, &gOffMpdType, 0x08, 0x40), "mpdTy");
    hit(TravelFieldOffHit(mpd, kHashMpdEnable, kFbMpdEnable, &gOffMpdEnable, 0x08, 0x40), "mpdEn");
    hit(TravelFieldOffHit(mpd, kHashMpdPName, kFbMpdPName, &gOffMpdPName, 0x10, 0x40), "mpdPn");
    hit(TravelFieldOffHit(mpd, kHashMpdX, kFbMpdX, &gOffMpdX, 0x18, 0x40), "mpdX");
    hit(TravelFieldOffHit(mpd, kHashMpdY, kFbMpdY, &gOffMpdY, 0x18, 0x40), "mpdY");
    hit(TravelFieldOffHit(mpd, kHashMpdToMapId, kFbMpdToMapId, &gOffMpdToMapId, 0x18, 0x40), "mpdTo");
    hit(TravelFieldOffHit(mpd, kHashMpdPortalRect, kFbMpdPortalRect, &gOffMpdPortalRect, 0x40, 0x90),
        "rect");
    hit(TravelFieldOffHit(mpd, kHashMpdHRange, kFbMpdHRange, &gOffMpdHRange, 0x40, 0x90), "hRange");
    hit(TravelFieldOffHit(mpd, kHashMpdVRange, kFbMpdVRange, &gOffMpdVRange, 0x40, 0x90), "vRange");
    hit(TravelFieldOffHit(mpd, kHashMpdVImpact, kFbMpdVImpact, &gOffMpdVImpact, 0x40, 0x90), "vImp");
    hit(TravelFieldOffHit(mpd, kHashMpdHImpact, kFbMpdHImpact, &gOffMpdHImpact, 0x40, 0x90), "hImp");
    // FieldKey@0x80：与 WorldPort 同宽 plausible（避免过窄 range 误杀）
    hit(TravelFieldOffHit(wm, kHashWmFieldKey, kFbWmFieldKey, &gOffWmFieldKey, 0x20, 0x200), "fk");
    hit(TravelFieldOffHit(pkt, kHashPacketBuffer, kFbPacketBuffer, &gOffPacketBuffer, 0x08, 0x40),
        "buf");
    hit(TravelFieldOffHit(pkt, kHashPacketOffset, kFbPacketOffset, &gOffPacketOffset, 0x08, 0x40),
        "off");
    hit(TravelFieldOffHit(outPkt, kHashOutPacketId, kFbOutPacketId, &gOffOutPacketId, 0x10, 0x40),
        "outId");
    hit(TravelFieldOffHit(actor, kHashUserVecCtrl, kFbUserVecCtrl, &gOffUserVecCtrl, 0x40, 0x100),
        "ulVc");
    hit(TravelFieldOffHit(vc, kHashVcAp, kFbVcAp, &gOffVcAp, 0x80, 0x100), "ap");
    hit(TravelFieldOffHit(vc, kHashVcApl, kFbVcApl, &gOffVcApl, 0x80, 0x100), "apl");
    constexpr int kExpect = 20;
    x::runtime::LogI("Travel",
                     "travel slots path=%s hits=%d/%d mpdId=0x%zX pn=0x%zX rect=0x%zX fk=0x%zX "
                     "vc=0x%zX ap=0x%zX apl=0x%zX outId=0x%zX myUser=0x%zX miss=%s",
                     hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
                     gOffMpdId, gOffMpdPName, gOffMpdPortalRect, gOffWmFieldKey, gOffUserVecCtrl,
                     gOffVcAp, gOffVcApl, gOffOutPacketId, x::ui::player::OffWmMyUser(),
                     missN ? miss : "-");
}

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnCompGo = void* (*)(void* comp, void* method);
using FnObjName = void* (*)(void* go, void* method);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnStrNew = void* (*)(const char* str);
using FnCheckMovePortal = void (*)(void* self, const void* method);
using FnOutCreate = void* (*)(int packetEnum, const void* method);
using FnEncode1 = void (*)(void* self, uint8_t v, const void* method);
using FnEncodeStr = void (*)(void* self, void* str, const void* method);
using FnNmSend = bool (*)(void* self, void* outPacket, const void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;
FnStrNew gStrNew = nullptr;

void* gPmKlass = nullptr;
void* gPmType = nullptr;
void* gPm = nullptr;
void* gWmType = nullptr;
void* gWm = nullptr;
void* gWmKlass = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gNmKlass = nullptr;      // Session?Send MethodInfo??
void* gNmType = nullptr;       // legacy
void* gNm = nullptr;           // Session*
void* gFacadeKlass = nullptr;
void* gFacadeType = nullptr;
void* gOutPacketKlass = nullptr;
#define kOffNmSession (x::runtime::il2cpp_network::OffNmSession())
DWORD gLastRebindMs = 0;

// ??????????+ ?? CheckMovePortal????
std::atomic<int> gFireMode{static_cast<int>(FireMode::DirectEnter)};
// 已站门判定：无 rect 时用门坐标近距；有 rect 用 Strict 触发框。
constexpr float kStandYTol = 12.f;
constexpr float kStickNearR = 72.f;
// Impact 远处贴门：分段冲量直到进触发区。
constexpr DWORD kImpactStickMaxMs = 10000;
constexpr DWORD kImpactStickPollMs = 50;
// FindMovePortal 触发框松弛（pre-fire / PointInPortalRect）
constexpr float kPortalRectSlop = 12.f;
std::atomic<bool> gCaptureOn{false};
std::atomic<bool> gCaptureInstalled{false};
MethodInfoHead* gMiSend = nullptr;
void* gOrigSend = nullptr;
MethodInfoHead* gMiCheckMove = nullptr;
MethodInfoHead* gMiOutCreate = nullptr;
MethodInfoHead* gMiEncode1 = nullptr;
MethodInfoHead* gMiEncodeStr = nullptr;
std::mutex gLastCapMu;
std::string gLastCapHex;
uint16_t gLastCapId = 0;

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

int32_t ReadI32(void* obj, size_t off);

uint16_t ReadU16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// hash ??plain ??RVA/kind?FindMethodResolved SSOT???
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plainName = nullptr, const char* hashName = nullptr,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plainName, hashName);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    *outOrig = mi->methodPointer;
    mi->methodPointer = hook;
    // ?? methodPointer????virtualMethodPointer=?? RVA?? shop ????FindMethodByRva ??????
    // NetworkManager.Send ??????methodPointer?Hook?sibling ??virtual ??RVA ?? MI??
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return true;
}

int CopyPacketHex(void* buf, int off, char* hex, int hexSz) {
    if (!hex || hexSz <= 0) return 0;
    hex[0] = 0;
    if (!LooksLikeHeapPtr(buf) || off <= 0) return 0;
    const int n = off > 96 ? 96 : off;
    int nhex = 0;
    __try {
        const uint8_t* data = reinterpret_cast<uint8_t*>(buf) + kOffArrData;
        for (int i = 0; i < n && nhex + 3 < hexSz; ++i)
            nhex += snprintf(hex + nhex, static_cast<size_t>(hexSz - nhex), "%02X", data[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(hex, static_cast<size_t>(hexSz), "READ_FAIL");
        return 0;
    }
    return nhex;
}

void DumpOutPacket(void* pkt, const char* tag) {
    if (!pkt) return;
    const uint16_t id = ReadU16(pkt, kOffOutPacketId);
    const int off = ReadI32(pkt, kOffPacketOffset);
    void* buf = ReadPtr(pkt, kOffPacketBuffer);
    char hex[512]{};
    CopyPacketHex(buf, off, hex, sizeof(hex));
    x::runtime::LogI("Travel", "C2S %s id=0x%04X(%u) off=%d hex=%s%s", tag ? tag : "pkt", id,
                     (unsigned)id, off, hex, off > 96 ? "..." : "");
    if (id == kWirePortalTeleport || id == (uint16_t)kClientPortalTeleport) {
        std::lock_guard<std::mutex> lock(gLastCapMu);
        gLastCapId = id;
        gLastCapHex = hex;
    }
}

bool __fastcall HookNmSend(void* self, void* outPacket, const void* method) {
    if (gCaptureOn.load() && outPacket) {
        const uint16_t id = ReadU16(outPacket, kOffOutPacketId);
        if (id == kWirePortalTeleport || id == (uint16_t)kClientPortalTeleport ||
            id == 0x0071 /* script portal candidate */)
            DumpOutPacket(outPacket, "portal?");
    }
    auto* orig = reinterpret_cast<FnNmSend>(gOrigSend);
    return orig ? orig(self, outPacket, method) : false;
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t ReadU8(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

float ReadF32(void* obj, size_t off) {
    if (!obj) return 0.f;
    __try {
        return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

bool ReadIl2CppString(void* strObj, char* out, int outSz) {
    if (!strObj || !out || outSz <= 0) return false;
    out[0] = 0;
    __try {
        const int len = ReadI32(strObj, 0x10);
        if (len <= 0 || len > 200) return false;
        const char16_t* chars =
            reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(strObj) + 0x14);
        int n = 0;
        for (int i = 0; i < len && n + 1 < outSz; ++i) {
            const char16_t c = chars[i];
            if (c < 128)
                out[n++] = static_cast<char>(c);
            else if (c < 0x800 && n + 2 < outSz) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            } else if (n + 3 < outSz) {
                out[n++] = static_cast<char>(0xE0 | (c >> 12));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* FindClass(const char* name) {
    void* k = x::runtime::il2cpp::FindClass("", name);
    if (!k) k = x::runtime::il2cpp::FindClass("Msc.Game.Object", name);
    return k;
}

void* ClassTypeObject(void* klass) {
    return x::runtime::il2cpp::ClassTypeObject(klass);
}

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t offs[] = {sizeof(void*), sizeof(void*) * 2, sizeof(void*) * 3};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); ++i) {
        void* v = ReadPtr(lazy, offs[i]);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* ResolveSingleton(void* klass) {
    if (!klass || !gClassStaticData) return nullptr;
    __try {
        if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(klass);
        void* statics = gClassStaticData(klass);
        if (!statics && gClassParent) {
            void* parent = gClassParent(klass);
            if (parent) {
                if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
                statics = gClassStaticData(parent);
            }
        }
        if (!statics) return nullptr;
        // Singleton Lazy often at +0x0 of statics
        for (size_t off = 0; off <= 0x40; off += sizeof(void*)) {
            void* lazy = ReadPtr(statics, off);
            void* inst = TryLazyValue(lazy);
            if (LooksLikeHeapPtr(inst) && ReadPtr(inst, 0) == klass) return inst;
            if (LooksLikeHeapPtr(lazy) && ReadPtr(lazy, 0) == klass) return lazy;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool ResolveApi() {
    if (!gGA || !gFindAll) {
        if (!x::runtime::il2cpp::Ensure()) return false;
        const auto& e = x::runtime::il2cpp::Get();
        gGA = e.ga;
        gFindAll = e.findAll;
        gCompGo = e.compGo;
        gObjName = e.objName;
        gClassStaticData = e.classStaticData;
        gClassParent = e.classParent;
        gRuntimeClassInit = e.runtimeClassInit;
        gClassGetMethods = e.classGetMethods;
        gStrNew = reinterpret_cast<FnStrNew>(e.stringNew);
    }
    return gFindAll && gClassGetMethods;
}

bool RebindManagers(DWORD now) {
    // ??SSOT ?????????gWm????????2s
    gWm = world::PeekWorldManager();
    if (now - gLastRebindMs < 2000 && gPm && gWm && world::IsAlive()) return true;
    gLastRebindMs = now;
    if (!ResolveApi()) return false;

    if (!gPmKlass) gPmKlass = FindClass(kPortalManagerClass);
    if (!gPmType && gPmKlass) gPmType = ClassTypeObject(gPmKlass);
    if (!gWmType) {
        void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
        gWmType = ClassTypeObject(wmKlass);
    }
    if (!gLuType) {
        void* luKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
        gLuType = ClassTypeObject(luKlass);
    }
    if (!gWmKlass) gWmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    if (!gFacadeKlass) gFacadeKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    if (!gNmKlass) gNmKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();  // Session
    if (!gFacadeType && gFacadeKlass) gFacadeType = ClassTypeObject(gFacadeKlass);
    if (!gOutPacketKlass) {
        gOutPacketKlass = FindClass("OutPacket");
        if (!gOutPacketKlass) gOutPacketKlass = FindClass(kOutPacketClass);
    }

    gPm = ResolveSingleton(gPmKlass);
    // InterStage：禁 FindAll；Singleton / WM.MyUser 仍可轻量绑。
    const bool allowFindAll = world::IsPlayReady();
    if (!gPm && allowFindAll && gPmType && gFindAll) {
        void* arr = x::runtime::managed_main::FindAll(gFindAll, gPmType, 1500);
        const int n = arr ? static_cast<int>(
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) +
                                                              kOffArrLen))
                          : 0;
        for (int i = 0; i < n && i < 8; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
                                                static_cast<size_t>(i) * sizeof(void*));
            if (LooksLikeHeapPtr(o) && LooksLikeHeapPtr(ReadPtr(o, kOffPmPortalList))) {
                gPm = o;
                break;
            }
        }
    }

    gWm = world::GetWorldManager();

    gLocalUser = nullptr;
    if (gWm) {
        void* mu = ReadPtr(gWm, kOffWmMyUser);
        if (LooksLikeHeapPtr(mu)) gLocalUser = mu;
    }
    if (!gLocalUser && allowFindAll && gLuType && gFindAll) {
        void* arr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 1500);
        const int n = arr ? static_cast<int>(
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + kOffArrLen))
                          : 0;
        for (int i = 0; i < n && i < 32; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
                                                static_cast<size_t>(i) * sizeof(void*));
            // FindAll 在泵上跑完后仍回到 worker：禁 GetGoName（GC unknown thread）。
            if (!LooksLikeHeapPtr(o) || !ReadPtr(o, 0) || !ReadPtr(o, 0x10)) continue;
            gLocalUser = o;
            break;
        }
    }

    void* facade = ResolveSingleton(gFacadeKlass);
    if (!facade && allowFindAll && gFacadeType && gFindAll) {
        void* arr = x::runtime::managed_main::FindAll(gFindAll, gFacadeType, 1500);
        const int n = arr ? static_cast<int>(
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + kOffArrLen))
                          : 0;
        for (int i = 0; i < n && i < 4; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
                                                static_cast<size_t>(i) * sizeof(void*));
            if (LooksLikeHeapPtr(o) && (!gFacadeKlass || ReadPtr(o, 0) == gFacadeKlass)) {
                facade = o;
                break;
            }
        }
    }
    gNm = nullptr;
    if (facade) {
        void* sess = ReadPtr(facade, kOffNmSession);
        if (LooksLikeHeapPtr(sess) && (!gNmKlass || ReadPtr(sess, 0) == gNmKlass)) gNm = sess;
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;

    EnsureTravelFieldOff();
    int methodHashHits = 0;
    auto note = [&](ResolvePath path) {
        if (path == ResolvePath::Hash) ++methodHashHits;
    };
    ResolvePath pChk = ResolvePath::Miss, pCreate = ResolvePath::Miss, pEnc = ResolvePath::Miss,
                pEncStr = ResolvePath::Miss, pSend = ResolvePath::Miss;

    if (gWmKlass && !gMiCheckMove) {
        // void() ????? ??????kind ?????
        constexpr MethodShape kChk{0, TypeKind::Void, false, true, {}};
        gMiCheckMove = ResolveMi(gWmKlass, kRvaCheckMovePortal, kChk, "CheckMovePortal",
                                 kHashCheckMovePortal, &pChk);
        note(pChk);
    }
    if (gOutPacketKlass) {
        // static OutPacket Create(enum) ??dump ??Create ?????
        constexpr MethodShape kCreate{1, TypeKind::Ptr, true, false, {TypeKind::Any}};
        if (!gMiOutCreate) {
            gMiOutCreate = ResolveMi(gOutPacketKlass, kRvaOutPacketCreate, kCreate, "Create",
                                     kHashOutCreate, &pCreate);
            note(pCreate);
        }
        // Encode1(byte) / EncodeStr(string) — SEND OutPacket 13775
        constexpr MethodShape kEnc{1, TypeKind::Void, true, false, {TypeKind::Any}};
        if (!gMiEncode1) {
            gMiEncode1 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode1Byte, kEnc, "Encode1",
                                   kHashEncode1, &pEnc);
            note(pEnc);
        }
        constexpr MethodShape kEncStr{1, TypeKind::Void, false, false, {TypeKind::Ptr}};
        if (!gMiEncodeStr) {
            gMiEncodeStr = ResolveMi(gOutPacketKlass, kRvaOutPacketEncodeStr, kEncStr, "EncodeStr",
                                     kHashEncodeStr, &pEncStr);
            note(pEncStr);
        }
    }
    if (gNmKlass && !gCaptureInstalled.load()) {
        MethodShape kSend{};
        kSend.arity = 1;
        kSend.ret = TypeKind::Bool;
        kSend.unique = true;
        kSend.walkParents = true;
        kSend.param[0] = TypeKind::Ptr;
        if (gOutPacketKlass) kSend.paramKlass[0] = gOutPacketKlass;
        gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "SendPacket", kHashSendPacket, &pSend);
        if (!gMiSend)
            gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "Send", kHashSendPacket, &pSend);
        note(pSend);
        if (gMiSend && PatchMethodInfo(gMiSend, reinterpret_cast<void*>(&HookNmSend), &gOrigSend)) {
            gCaptureInstalled.store(true);
            x::runtime::LogI("Travel", "Send capture MethodInfo ok mi=%p", (void*)gMiSend);
            x::runtime::anchor_lamps::Set("TravelSend",
                                         x::runtime::anchor_lamps::AnchorLampCode::Ok, "MI ok");
        } else if (gMiSend) {
            x::runtime::anchor_lamps::Set("TravelSend",
                                         x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                         "MI no patch");
        } else {
            x::runtime::anchor_lamps::Set("TravelSend",
                                         x::runtime::anchor_lamps::AnchorLampCode::Miss, "MISS");
        }
    }
    static bool sMethodHitsLogged = false;
    if (!sMethodHitsLogged && (gMiCheckMove || gMiOutCreate || gMiEncode1 || gMiEncodeStr || gMiSend)) {
        sMethodHitsLogged = true;
        x::runtime::LogI("Travel", "methods path=%s hits=%d/5",
                         methodHashHits == 5 ? "meta"
                                             : (methodHashHits ? "meta-partial" : "fallback"),
                         methodHashHits);
    }

    return LooksLikeHeapPtr(gPm) && LooksLikeHeapPtr(gWm);
}

std::string PadMapKey(int mapId) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%09d", mapId);
    return buf;
}

bool FinalizeAabb(float L, float T, float R, float B, float& outL, float& outT, float& outR,
                  float& outB) {
    constexpr float kMaxPortalSpan = 600.f;
    constexpr float kMaxAbs = 30000.f;
    if (!(L < R) || !(T < B)) return false;
    if ((R - L) > kMaxPortalSpan || (B - T) > kMaxPortalSpan) return false;
    if (std::fabs(L) > kMaxAbs || std::fabs(R) > kMaxAbs || std::fabs(T) > kMaxAbs ||
        std::fabs(B) > kMaxAbs)
        return false;
    outL = L;
    outT = T;
    outR = R;
    outB = B;
    return true;
}

// Reject rects that do not cover the portal anchor (misparse / stale field).
bool RectCoversPortal(float L, float T, float R, float B, float px, float py) {
    constexpr float kSlop = 2.f;
    return (px + kSlop) >= L && (px - kSlop) <= R && (py + kSlop) >= T && (py - kSlop) <= B;
}

// Disambiguate Unity Rect (x,y,w,h) vs LTRB corners. Prefer the interpretation
// that covers the portal with the smaller area (true triggers are tight).
// Blind sizeLike=>xywh misreads int LTRB like (100,200,140,250) (Bugbot);
// blind c>a=>LTRB misreads Unity (-1589,-2133,40,100) (BIN f61e43).
bool ResolvePortalAabb(float a, float b, float c, float d, float px, float py, float& outL,
                       float& outT, float& outR, float& outB) {
    constexpr float kMaxSize = 800.f;
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d))
        return false;

    float bestL = 0.f, bestT = 0.f, bestR = 0.f, bestB = 0.f;
    float bestArea = 0.f;
    bool have = false;

    auto consider = [&](float L, float T, float R, float B) {
        float l = 0.f, t = 0.f, r = 0.f, bb = 0.f;
        if (!FinalizeAabb(L, T, R, B, l, t, r, bb)) return;
        if (!RectCoversPortal(l, t, r, bb, px, py)) return;
        const float area = (r - l) * (bb - t);
        if (!have || area < bestArea) {
            have = true;
            bestArea = area;
            bestL = l;
            bestT = t;
            bestR = r;
            bestB = bb;
        }
    };

    // Candidate A: Unity Rect x,y,w,h
    if (c > 0.f && d > 0.f && c <= kMaxSize && d <= kMaxSize) {
        consider(a, b, a + c, b + d);
    }
    // Candidate B: absolute LTRB corners
    if (c > a && d > b) {
        consider(a, b, c, d);
    } else {
        consider((std::min)(a, c), (std::min)(b, d), (std::max)(a, c), (std::max)(b, d));
    }

    if (!have) return false;
    outL = bestL;
    outT = bestT;
    outR = bestR;
    outB = bestB;
    return true;
}

bool PointInPortalRect(const PortalInfo& p, float x, float y) {
    if (!p.rectValid) return true;  // no rect => do not gate
    return x >= (p.rectL - kPortalRectSlop) && x <= (p.rectR + kPortalRectSlop) &&
           y >= (p.rectT - kPortalRectSlop) && y <= (p.rectB + kPortalRectSlop);
}

bool PointInPortalRectStrict(const PortalInfo& p, float x, float y) {
    if (!p.rectValid) return true;
    return x >= p.rectL && x <= p.rectR && y >= p.rectT && y <= p.rectB;
}

void ClampIntoPortalRect(const PortalInfo& p, float& x, float& y) {
    if (!p.rectValid) return;
    if (x < p.rectL) x = p.rectL;
    if (x > p.rectR) x = p.rectR;
    if (y < p.rectT) y = p.rectT;
    if (y > p.rectB) y = p.rectB;
}

void FillPortalTriggerRect(void* data, PortalInfo& out) {
    out.rectValid = false;
    // 1) PortalRect @0x54 = Unity Rect float x,y,w,h (fallback int L,T,R,B)
    float f0 = ReadF32(data, kOffMpdPortalRect);
    float f1 = ReadF32(data, kOffMpdPortalRect + 4);
    float f2 = ReadF32(data, kOffMpdPortalRect + 8);
    float f3 = ReadF32(data, kOffMpdPortalRect + 12);
    float L = 0.f, T = 0.f, R = 0.f, B = 0.f;
    if (ResolvePortalAabb(f0, f1, f2, f3, out.x, out.y, L, T, R, B)) {
        out.rectL = L;
        out.rectT = T;
        out.rectR = R;
        out.rectB = B;
        out.rectValid = true;
        return;
    }
    const int i0 = ReadI32(data, kOffMpdPortalRect);
    const int i1 = ReadI32(data, kOffMpdPortalRect + 4);
    const int i2 = ReadI32(data, kOffMpdPortalRect + 8);
    const int i3 = ReadI32(data, kOffMpdPortalRect + 12);
    if (ResolvePortalAabb(static_cast<float>(i0), static_cast<float>(i1), static_cast<float>(i2),
                          static_cast<float>(i3), out.x, out.y, L, T, R, B)) {
        out.rectL = L;
        out.rectT = T;
        out.rectR = R;
        out.rectB = B;
        out.rectValid = true;
        return;
    }
    // 2) Impact / Range half-extents around portal (x,y). Do not fabricate a
    // rect from zero data - false rectValid would gate CheckMove incorrectly.
    float hx = static_cast<float>(ReadI32(data, kOffMpdHImpact));
    float hy = static_cast<float>(ReadI32(data, kOffMpdVImpact));
    if (!(hx > 0.f) && !(hy > 0.f)) {
        hx = static_cast<float>(ReadI32(data, kOffMpdHRange));
        hy = static_cast<float>(ReadI32(data, kOffMpdVRange));
    }
    if (hx < 0.f) hx = -hx;
    if (hy < 0.f) hy = -hy;
    if (!(hx > 0.f) && !(hy > 0.f)) return;
    if (hx < 8.f) hx = 25.f;
    if (hy < 8.f) hy = 50.f;
    if (ResolvePortalAabb(out.x - hx, out.y - hy, out.x + hx, out.y + hy, out.x, out.y, L, T, R,
                          B)) {
        out.rectL = L;
        out.rectT = T;
        out.rectR = R;
        out.rectB = B;
        out.rectValid = true;
    }
}

bool FillFromData(void* data, const std::string& mapKey, PortalInfo& out) {
    if (!LooksLikeHeapPtr(data)) return false;
    char pn[96]{};
    if (!ReadIl2CppString(ReadPtr(data, kOffMpdPName), pn, sizeof(pn))) return false;
    if (!pn[0] || strcmp(pn, "sp") == 0) return false;
    out.name = pn;
    out.pt = ReadI32(data, kOffMpdType);
    out.activate = ReadU8(data, kOffMpdEnable) != 0;
    out.x = static_cast<float>(ReadI32(data, kOffMpdX));
    out.y = static_cast<float>(ReadI32(data, kOffMpdY));
    out.toMapId = ReadI32(data, kOffMpdToMapId);
    out.vis = (out.pt == 2 || out.pt == 1 || out.pt == 3);  // Visible/Invisible/Collision
    out.fm = false;
    if (out.toMapId > 0 && out.toMapId != 999999999)
        out.destMap = PadMapKey(out.toMapId);
    out.id = "seed:" + mapKey + "/" + out.name;
    FillPortalTriggerRect(data, out);
    return true;
}

}  // namespace

bool EnsureBound() { return RebindManagers(GetTickCount()); }

void Init() {
    if (ResolveApi()) {
        x::runtime::LogI("Travel", "port init api ready");
    } else {
        x::runtime::LogW("Travel", "port init ResolveApi incomplete");
    }
}

int CurrentMapId() {
    // ?? world_port SSOT?? foothold/mobscan ????travel ?? WM ??????MapData??
    const int viaWorld = ports::world::GetMapId();
    if (viaWorld > 0) return viaWorld;
    if (!EnsureBound() || !gWm) return 0;
    void* md = ReadPtr(gWm, kOffWmMapData);
    if (!LooksLikeHeapPtr(md)) return 0;
    return ReadI32(md, kOffMapId);
}

std::string CurrentMapKey() {
    const int id = CurrentMapId();
    return id > 0 ? PadMapKey(id) : std::string();
}

// list ????Portal*????Data@0x10???? MapPortalData*??
bool EnumPortalList(void* list, const std::string& mapKey, bool elementsArePortalObj,
                    std::vector<PortalInfo>& out) {
    if (!LooksLikeHeapPtr(list)) return false;
    const int n = ReadI32(list, kOffListSize);
    void* items = ReadPtr(list, kOffListItems);
    if (!LooksLikeHeapPtr(items) || n <= 0 || n > 512) return false;

    for (int i = 0; i < n; ++i) {
        void* elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + kOffArrData +
                                               static_cast<size_t>(i) * sizeof(void*));
        if (!LooksLikeHeapPtr(elem)) continue;
        void* data = elementsArePortalObj ? ReadPtr(elem, kOffPortalData) : elem;
        PortalInfo pi{};
        if (!FillFromData(data, mapKey, pi)) continue;
        out.push_back(std::move(pi));
    }
    return !out.empty();
}

bool EnumPortals(std::string& outMapKey, std::vector<PortalInfo>& out) {
    out.clear();
    outMapKey.clear();
    outMapKey = CurrentMapKey();
    if (outMapKey.empty()) {
        if (!EnsureBound()) return false;
        outMapKey = CurrentMapKey();
    }
    if (outMapKey.empty()) return false;

    // 1) PortalManager._portalList???? Portal ????
    if (EnsureBound() && gPm) {
        void* list = ReadPtr(gPm, kOffPmPortalList);
        if (EnumPortalList(list, outMapKey, /*elementsArePortalObj=*/true, out)) return true;
    }

    // 2) ???MapData.Portals?? foothold ??SSOT?PM ????????????
    void* wm = world::GetWorldManager();
    if (!wm) wm = gWm;
    if (!LooksLikeHeapPtr(wm)) return false;
    void* md = ReadPtr(wm, kOffWmMapData);
    if (!LooksLikeHeapPtr(md)) return false;
    void* mdList = ReadPtr(md, kOffMapPortals);
    if (!EnumPortalList(mdList, outMapKey, /*elementsArePortalObj=*/false, out)) return false;
    static std::string sLastEnumLogMap;
    if (sLastEnumLogMap != outMapKey) {
        sLastEnumLogMap = outMapKey;
        x::runtime::LogI("Travel", "enum portals via MapData n=%d map=%s", (int)out.size(),
                         outMapKey.c_str());
    }
    return true;
}

bool FindPortalByName(const std::string& portalName, PortalInfo& out) {
    std::string mapKey;
    std::vector<PortalInfo> all;
    if (!EnumPortals(mapKey, all)) return false;
    for (auto& p : all) {
        if (p.name == portalName) {
            out = std::move(p);
            return true;
        }
    }
    return false;
}

bool FirePortalByName(const std::string& portalName, std::string& outResult) {
    return FirePortalByName(portalName, true, outResult);
}

struct FireJob {
    FireMode mode = FireMode::Up;
    char name[96]{};
    uint8_t fieldKey = 0;
    bool ok = false;
    char result[80]{};
};

bool CallCheckMovePortal(char* result, size_t resultCap) {
    if (!world::IsPlayReady()) {
        snprintf(result, resultCap, "NOT_PLAY_READY");
        return false;
    }
    if (!gWm || !gMiCheckMove || !gMiCheckMove->methodPointer) {
        snprintf(result, resultCap, "NO_CHECKMOVE");
        return false;
    }
    auto* fn = reinterpret_cast<FnCheckMovePortal>(gMiCheckMove->methodPointer);
    fn(gWm, gMiCheckMove);
    snprintf(result, resultCap, "FIRED_CHECKMOVE");
    return true;
}

bool CallUpKey(char* result, size_t resultCap) {
    ports::input::EnsureBound();
    if (!ports::input::InjectKeyHold(VK_UP, 120)) {
        snprintf(result, resultCap, "KEY_FAIL");
        return false;
    }
    snprintf(result, resultCap, "FIRED_UP");
    return true;
}

void FireJobOnMain(void* user) {
    auto* job = reinterpret_cast<FireJob*>(user);
    if (!job) return;
    __try {
        // Impact 贴门已在 worker 侧完成；此处只触发 CheckMove/↑ / Rpc。
        // 禁止 AbsPos/Transform 硬写坐标；禁止 fill+Doing。
        if (job->mode == FireMode::Up || job->mode == FireMode::StickUp) {
            job->ok = CallUpKey(job->result, sizeof(job->result));
            if (job->ok && job->mode == FireMode::StickUp)
                snprintf(job->result, sizeof(job->result), "FIRED_STICK_UP");
            return;
        }

        if (job->mode == FireMode::CheckMove || job->mode == FireMode::DirectEnter) {
            job->ok = CallCheckMovePortal(job->result, sizeof(job->result));
            if (job->ok && job->mode == FireMode::DirectEnter)
                snprintf(job->result, sizeof(job->result), "FIRED_DIRECT");
            return;
        }

        // Rpc：Create(114) + Encode1(fieldKey) + EncodeStr(pn) + Send
        if (!gNm || !gMiOutCreate || !gMiEncodeStr || !gMiSend || !gStrNew) {
            snprintf(job->result, sizeof(job->result), "NO_RPC_API");
            return;
        }
        auto* create = reinterpret_cast<FnOutCreate>(
            gMiOutCreate->methodPointer ? gMiOutCreate->methodPointer
                                        : AtRva<void*>(kRvaOutPacketCreate));
        void* pkt = create(kClientPortalTeleport, gMiOutCreate);
        if (!LooksLikeHeapPtr(pkt)) {
            snprintf(job->result, sizeof(job->result), "CREATE_FAIL");
            return;
        }
        if (gMiEncode1 && gMiEncode1->methodPointer) {
            auto* enc1 = reinterpret_cast<FnEncode1>(gMiEncode1->methodPointer);
            enc1(pkt, job->fieldKey, gMiEncode1);
        }
        void* s = x::runtime::il2cpp::NewString(job->name);
        if (!LooksLikeHeapPtr(s)) {
            snprintf(job->result, sizeof(job->result), "STR_FAIL");
            return;
        }
        auto* encStr = reinterpret_cast<FnEncodeStr>(gMiEncodeStr->methodPointer);
        encStr(pkt, s, gMiEncodeStr);
        DumpOutPacket(pkt, "rpc-pre");
        auto* send = reinterpret_cast<FnNmSend>(gOrigSend ? gOrigSend : gMiSend->methodPointer);
        const bool sent = send(gNm, pkt, gMiSend);
        snprintf(job->result, sizeof(job->result), sent ? "FIRED_RPC" : "SEND_FAIL");
        job->ok = sent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(job->result, sizeof(job->result), "EXCEPTION");
    }
}

// 读 LocalUser.VecCtrl.Ap（worker 可调；必要时 RebindManagers）。
bool ReadLocalAp(float& outX, float& outY) {
    if (!LooksLikeHeapPtr(gLocalUser)) {
        // ?? worker ??????MyUser??????
        (void)RebindManagers(GetTickCount());
    }
    if (!LooksLikeHeapPtr(gLocalUser)) return false;
    void* vc = ReadPtr(gLocalUser, kOffUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    outX = static_cast<float>(ReadF64(vc, kOffVcApX));
    outY = static_cast<float>(ReadF64(vc, kOffVcApY));
    return true;
}

bool AlreadyStoodAtPortal(const PortalInfo& portal) {
    float ax = 0.f, ay = 0.f;
    if (!ReadLocalAp(ax, ay)) return false;
    if (portal.rectValid) return PointInPortalRectStrict(portal, ax, ay);
    const float dx = ax - portal.x;
    const float dy = ay - portal.y;
    return std::fabs(dy) <= kStandYTol &&
           (dx * dx + dy * dy) <= (kStickNearR * kStickNearR);
}

// Impact 贴门（仅 travel）：Snap 后分段 toward；掠过触发区当拍 CheckMove/↑，不要求刹停。
// 不改 teleport_port / fly / combat 的默认 opts 或公共 API。
bool TryFireEnterOnMain(FireMode mode, std::string& outResult) {
    FireJob job{};
    job.mode = mode;
    job.fieldKey = gWm ? ReadU8(gWm, kOffWmFieldKey) : 0;
    if (mode == FireMode::Up) {
        FireJobOnMain(&job);
    } else if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, 2500)) {
        outResult = "MAIN_TIMEOUT";
        return false;
    }
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        outResult = "MAP_TRANSITION";
        return true;
    }
    outResult = job.result;
    return job.ok;
}

bool InPortalTrigger(const PortalInfo& portal) {
    // 与引擎进门一致：有框用触发框（含 slop）；无框退回近距站立判定。
    float ax = 0.f, ay = 0.f;
    if (!ReadLocalAp(ax, ay)) return false;
    if (portal.rectValid) return PointInPortalRect(portal, ax, ay);
    return AlreadyStoodAtPortal(portal);
}

bool ImpactStickToPortal(const PortalInfo& portal, FireMode enterMode, std::string& outResult,
                         float* outSx, float* outSy) {
    float aimX = portal.x;
    float aimY = portal.y;
    if (portal.rectValid) {
        if (!PointInPortalRect(portal, aimX, aimY)) ClampIntoPortalRect(portal, aimX, aimY);
    }
    if (outSx) *outSx = aimX;
    if (outSy) *outSy = aimY;

    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        outResult = "NOT_PLAY_READY";
        return false;
    }
    float luX = 0.f, luY = 0.f;
    if (!ReadLocalAp(luX, luY)) {
        outResult = "NO_LOCALUSER";
        return false;
    }
    (void)luX;
    (void)luY;

    float standX = aimX, standY = aimY;
    uint32_t fh = 0;
    if (!ports::foothold_path::SnapStandForPortal(aimX, aimY, portal.rectL, portal.rectT,
                                                 portal.rectR, portal.rectB, portal.rectValid,
                                                 &standX, &standY, &fh) ||
        fh == 0) {
        outResult = "FH0_FORBID";
        x::runtime::LogW("Travel",
                         "impact stick forbid fh=0 portal=(%.0f,%.0f) aim=(%.0f,%.0f) "
                         "snap=(%.0f,%.0f)",
                         portal.x, portal.y, aimX, aimY, standX, standY);
        return false;
    }
    if (portal.rectValid && !PointInPortalRectStrict(portal, standX, standY)) {
        const float beforeX = standX, beforeY = standY;
        float targetX = portal.x;
        if (targetX < portal.rectL) targetX = portal.rectL;
        if (targetX > portal.rectR) targetX = portal.rectR;
        float sx2 = targetX, sy2 = portal.y;
        uint32_t fh2 = 0;
        if (ports::foothold_path::SnapStandForPortal(targetX, portal.y, portal.rectL, portal.rectT,
                                                     portal.rectR, portal.rectB, true, &sx2, &sy2,
                                                     &fh2) &&
            fh2 != 0) {
            standX = sx2;
            standY = sy2;
            fh = fh2;
        } else {
            standX = targetX;
            standY = portal.y;
        }
        if (!PointInPortalRectStrict(portal, standX, standY)) {
            standX = targetX;
            ClampIntoPortalRect(portal, standX, standY);
            float sx3 = standX, sy3 = standY;
            if (fh != 0 &&
                ports::foothold_path::SnapOnFh(fh, standX, &sx3, &sy3,
                                              /*avoidWalkJunction=*/false)) {
                standX = sx3;
                standY = sy3;
                if (!PointInPortalRectStrict(portal, standX, standY)) {
                    standX = targetX;
                    ClampIntoPortalRect(portal, standX, standY);
                }
            }
        }
        x::runtime::LogW("Travel",
                         "impact snap clamp name=%s before=(%.0f,%.0f) after=(%.0f,%.0f) "
                         "rect=(%.0f,%.0f)-(%.0f,%.0f) in=%d",
                         portal.name.c_str(), beforeX, beforeY, standX, standY, portal.rectL,
                         portal.rectT, portal.rectR, portal.rectB,
                         PointInPortalRectStrict(portal, standX, standY) ? 1 : 0);
    }
    if (outSx) *outSx = standX;
    if (outSy) *outSy = standY;

    x::runtime::LogI("Travel",
                     "impact stick aim name=%s portal=(%.0f,%.0f) rect=%d "
                     "(%.0f,%.0f)-(%.0f,%.0f) stand=(%.0f,%.0f) fh=%u",
                     portal.name.c_str(), portal.x, portal.y, portal.rectValid ? 1 : 0,
                     portal.rectL, portal.rectT, portal.rectR, portal.rectB, standX, standY,
                     (unsigned)fh);

    if (!x::features::invuln::IsEnabled()) {
        outResult = "INVULN_OFF";
        x::runtime::LogW("Travel", "impact stick refuse invuln_off name=%s", portal.name.c_str());
        return false;
    }

    const auto route = x::features::fly::GetMode() == 1u
                           ? ports::teleport::ImpactRoute::SetImpactNext
                           : ports::teleport::ImpactRoute::NockBack;
    // 仅本循环本地 opts：近距 soft-brake，不改 fly/combat 默认。
    ports::teleport::ImpactTowardOpts opts{};
    opts.quietLog = true;
    opts.adaptive = true;
    opts.leadSec = 0.f;
    opts.maxSegPx = 320.f;
    opts.minSegPx = 8.f;
    opts.maxSpeed = 1600.f;
    opts.speedScale = 4.f;

    const DWORD t0 = GetTickCount();
    int failStreak = 0;
    int hopN = 0;
    for (;;) {
        if (!world::IsInMapScene() || !world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            x::runtime::LogI("Travel", "impact stick map transition name=%s hops=%d",
                             portal.name.c_str(), hopN);
            return true;
        }
        // 掠过即火：进触发区当拍开火，本拍不再 toward（避免残速把人推出框后再二次判定）。
        if (InPortalTrigger(portal)) {
            float ax = 0.f, ay = 0.f;
            (void)ReadLocalAp(ax, ay);
            x::runtime::LogI("Travel",
                             "impact stick glide-fire name=%s ap=(%.0f,%.0f) stand=(%.0f,%.0f) "
                             "hops=%d mode=%s",
                             portal.name.c_str(), ax, ay, standX, standY, hopN,
                             FireModeName(enterMode));
            return TryFireEnterOnMain(enterMode, outResult);
        }
        if (GetTickCount() - t0 >= kImpactStickMaxMs) {
            outResult = "NOT_STOOD";
            float ax = 0.f, ay = 0.f;
            const bool got = ReadLocalAp(ax, ay);
            x::runtime::LogW("Travel",
                             "impact stick timeout name=%s ap=(%.0f,%.0f) stand=(%.0f,%.0f) "
                             "hops=%d failStreak=%d",
                             portal.name.c_str(), got ? ax : 0.f, got ? ay : 0.f, standX, standY,
                             hopN, failStreak);
            return false;
        }

        if (!ports::teleport::ImpactImpulseToward(standX, standY, route, opts)) {
            ++failStreak;
            if (failStreak >= 8) {
                outResult = "IMPACT_STICK_FAIL";
                x::runtime::LogW("Travel",
                                 "impact stick fail name=%s streak=%d hops=%d route=%u",
                                 portal.name.c_str(), failStreak, hopN,
                                 static_cast<unsigned>(route));
                return false;
            }
        } else {
            failStreak = 0;
            ++hopN;
        }
        Sleep(kImpactStickPollMs);
    }
}

// 已在门内：返回 OK，由 FirePortal 补一枪（无惯性问题）。
// 远处：冲量掠过触发区当拍开火，outResult 已是 FIRED_* / MAP_*。
bool StickThenEnterReady(const PortalInfo& portal, FireMode enterMode, std::string& outResult) {
    if (AlreadyStoodAtPortal(portal) || InPortalTrigger(portal)) {
        outResult = "OK";
        float ax = 0.f, ay = 0.f;
        if (ReadLocalAp(ax, ay)) {
            x::runtime::LogI("Travel",
                             "stick skip-impact already_in name=%s ap=(%.0f,%.0f)",
                             portal.name.c_str(), ax, ay);
        }
        return true;
    }
    return ImpactStickToPortal(portal, enterMode, outResult, nullptr, nullptr);
}

bool FirePortalByName(const std::string& portalName, bool warpFirst, std::string& outResult) {
    outResult.clear();
    const FireMode mode = GetFireMode();
    PortalInfo p{};
    if (!FindPortalByName(portalName, p)) {
        // Rpc-far ????????????????
        if (!(mode == FireMode::Rpc && !warpFirst)) {
            outResult = "NO_PORTAL";
            return false;
        }
        p.name = portalName;
    }
    // ?????Enable ??????????????????????????DISABLED??
    if (warpFirst && !p.activate && mode != FireMode::Rpc &&
        mode != FireMode::StickUp && mode != FireMode::DirectEnter) {
        outResult = "PORTAL_DISABLED";
        return false;
    }

    // 产品贴门：已在门内 → 下面补火；远处冲量掠过 → Stick 内已开火。
    if ((mode == FireMode::StickUp || mode == FireMode::DirectEnter) && warpFirst) {
        if (!StickThenEnterReady(p, mode, outResult)) return false;
        if (outResult == "MAP_TRANSITION" || outResult == "MAP_CHANGED") {
            x::runtime::LogI("Travel", "skip CheckMove (%s) name=%s", outResult.c_str(),
                             portalName.c_str());
            return true;
        }
        // 掠过当拍已开火：勿再二次 inRect（残速易 OUT_OF_RECT）也勿再排队一枪。
        if (outResult.rfind("FIRED_", 0) == 0) {
            x::runtime::LogI("Travel", "glide-fire done name=%s res=%s", portalName.c_str(),
                             outResult.c_str());
            return true;
        }
        if (!world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            x::runtime::LogI("Travel", "skip CheckMove (not play ready) name=%s",
                             portalName.c_str());
            return true;
        }
        float apX = 0.f, apY = 0.f;
        if (ReadLocalAp(apX, apY)) {
            const bool inRect = PointInPortalRect(p, apX, apY);
            x::runtime::LogI("Travel",
                             "pre-fire name=%s ap=(%.0f,%.0f) rect=%d inRect=%d mode=%s",
                             portalName.c_str(), apX, apY, p.rectValid ? 1 : 0, inRect ? 1 : 0,
                             FireModeName(mode));
            if (p.rectValid && !inRect) {
                outResult = "OUT_OF_RECT";
                x::runtime::LogW("Travel",
                                 "skip fire OUT_OF_RECT name=%s ap=(%.0f,%.0f) "
                                 "rect=(%.0f,%.0f)-(%.0f,%.0f)",
                                 portalName.c_str(), apX, apY, p.rectL, p.rectT, p.rectR,
                                 p.rectB);
                return false;
            }
        }
    }

    FireJob job{};
    job.mode = mode;
    job.fieldKey = gWm ? ReadU8(gWm, kOffWmFieldKey) : 0;
    strncpy_s(job.name, portalName.c_str(), _TRUNCATE);

    // Up ????????????managed_main?GC / MethodInfo??
    if (job.mode == FireMode::Up) {
        FireJobOnMain(&job);
    } else {
        if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, 2500)) {
            outResult = "MAIN_TIMEOUT";
            return false;
        }
    }
    outResult = job.result;
    return job.ok;
}

void SetFireMode(FireMode mode) {
    gFireMode.store(static_cast<int>(mode));
    x::runtime::LogI("Travel", "fire_mode=%s", FireModeName(mode));
}

FireMode GetFireMode() { return static_cast<FireMode>(gFireMode.load()); }

const char* FireModeName(FireMode mode) {
    switch (mode) {
    case FireMode::Up:
        return "up";
    case FireMode::CheckMove:
        return "check";
    case FireMode::Rpc:
        return "rpc";
    case FireMode::StickUp:
        return "stick";
    case FireMode::DirectEnter:
        return "direct";
    default:
        return "?";
    }
}

void SetCaptureEnabled(bool on) {
    gCaptureOn.store(on);
    EnsureBound();
    x::runtime::LogI("Travel", "capture=%d installed=%d", on ? 1 : 0,
                     gCaptureInstalled.load() ? 1 : 0);
}

bool IsCaptureEnabled() { return gCaptureOn.load(); }

}  // namespace x::features::ports::travel
