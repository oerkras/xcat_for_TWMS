// Classic TWMS travel_port ??PortalManager / MapData / ???? / ???? / ??????
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "travel_port.h"

#include "teleport_port.h"
#include "world_port.h"
#include "foothold_path.h"
#include "foothold_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"
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
    "cce70e130cc0ea3b4c77574230246f5d88b70c68de229bbc1e256c09320efb4";  // remounted 2026-08-04
// WM / UserLocal / NM ??il2cpp_shape Resolve*Klass?hash + shape ????
constexpr char kOutPacketClass[] =
    "f07686cc7a01760c9166b2cf7a72f4ac7c084f1ee39bd1c3bdc42c351e884bb";
// ?????dump.cs ? remount 2026-08-04???void()/EncodeStr ??????????
constexpr char kHashCheckMovePortal[] =
    "f919f4ffcc978acefa58dc5c9ed1336a1f381b097ac87e4cbc217a2890a9de9";
constexpr char kHashOutCreate[] =
    "ef87de519630cf113299d43987ff14b1f1676915e89829fad3840a2ba3d6363";
constexpr char kHashEncode1[] =
    "ad9431895506411dc6fe026818f2ee62a880edd40acff717a91c0698fe4e18a";
constexpr char kHashEncodeStr[] =
    "a4d208e8366660c2ac3285830093126c5111acb7e229fa93122f98d04ac2baa";
constexpr char kHashSendPacket[] =
    "a3e15e8fb1d9cacfe30bdb5b652ad6f7df5037a51e3a48cfede943d8fc2d59b";

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E4A610;  // remounted 2026-08-04
constexpr uint32_t kRvaCompGetGo = 0x4E53330;              // remounted 2026-08-04
constexpr uint32_t kRvaObjGetName = 0x4E60290;             // remounted 2026-08-04
constexpr uint32_t kRvaTfSetPos = 0x4E6DDE0;  // remounted 2026-08-04 Transform.set_position
// WorldManager.CheckMovePortal????SetMyUser / InitCamera ????
constexpr uint32_t kRvaCheckMovePortal = 0xDD08D0;  // remounted 2026-08-04
// OutPacket / NetworkManager.Send?C?S??
constexpr uint32_t kRvaOutPacketCreate = 0x1CC22D0;  // remounted 2026-08-04
constexpr uint32_t kRvaOutPacketEncode1Byte = 0x1CCE8B0;  // remounted 2026-08-04
constexpr uint32_t kRvaOutPacketEncodeStr = 0x1CCF0F0;  // remounted 2026-08-04
constexpr uint32_t kRvaNmSend = 0x1CC3EE0;  // remounted 2026-08-04 Session.SendPacket
// CMS ClientPacket.UserPortalTeleportRequest = 114 ??e59 wire 0x0072
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
    "a91d2e09edd6e7ecbf1c9e5b24c97b7a1abbe4f0e261edfefb85479b9579987";
constexpr char kPortalClass[] =
    "e14a58ebd818c4c5d72a24f3f16f357f9dfc6a9ef1ed5c81a1dc983cbdc7f5e";
constexpr char kActorBaseClass[] =
    "ddef6db860cfa2bea6dca39e201bf3065a897797f86009fb4d6104830143d94";
constexpr char kVecCtrlClass[] =
    "ef24024acbe225bcc90ca332f3e00aff5800daa32a769057d2e830eeac776bb";
constexpr char kPacketClass[] =
    "fc6ae331019bd3c1e987ba71c4f75e3591b683aabc4278715ac9c79480cbdac";

constexpr char kHashPortalData[] =
    "f8d092284cbb4575d53243e4243b8a686a6387972a425331abf7f3c559c73ae";
constexpr char kHashMpdId[] =
    "<e42bf60d5d1464c5af4fea6350354857cca52dcc5d91c345dbb640006eaa320>k__BackingField";
constexpr char kHashMpdType[] =
    "<eeafe8e6a3f6e9cd3659f7643cb9a78fd4a8d9dfb21b783a6f56e897f087e75>k__BackingField";
constexpr char kHashMpdEnable[] =
    "<bb5e086289a63bbe366789d211fa709cca3ff895538a61b4a7edc0e88b14b64>k__BackingField";
constexpr char kHashMpdPName[] =
    "<addfadeffbc3c71c1b357406f2cd702450dfb3695d02b5a68ea9738296debd5>k__BackingField";
constexpr char kHashMpdX[] =
    "<e49aaaa93d5514fa05fa51744bb57631089967749b0e83dc2a07f5423a1d339>k__BackingField";
constexpr char kHashMpdY[] =
    "<a89faf45ce0904cb1d7ea80c0ab36665fa6af2cbf756d3a96df0d80f9c84755>k__BackingField";
constexpr char kHashMpdToMapId[] =
    "<d547da7ecfe12bfed575b3255bbf24574763ce3b1eed394654fa14736e196f9>k__BackingField";
// MapPortalData ????dump restore_field_map ? ????hash?offset??
constexpr char kHashMpdPortalRect[] =
    "<ad819f1db6df4cccb1842f1abad2969a7613d6965076de507173f8a75d1c9ab>k__BackingField";
constexpr char kHashMpdHRange[] =
    "<d2be40bd4b4f5a643ae20d8c582ea4c7dc5349e9edb87c09c19bfb37c317189>k__BackingField";
constexpr char kHashMpdVRange[] =
    "<a60aec2d99475a8c3fa14c908789d71bef49c3d131ddf358d78b979c4117035>k__BackingField";
constexpr char kHashMpdVImpact[] =
    "<e9929d2059a510fa6d418f899db9f9cc9d7af56fe48c2fe1697add861f97952>k__BackingField";
constexpr char kHashMpdHImpact[] =
    "<b9fa99b3ffd3fdee2b4482cb905ced180565d34f42d80864bbb64e3df787586>k__BackingField";
constexpr char kHashWmFieldKey[] =
    "c1d14be8e70914fef7c7c4e723b2ec84991b1780e9264fe5b1dcc785203af5f";
constexpr char kHashPacketBuffer[] =
    "<c096dd8ffc6b9c4dc1a6458417487a3a0d8fb33676030af7deb507639a12e2b>k__BackingField";
constexpr char kHashPacketOffset[] =
    "<f9bbb972b920d8265c641b982330d9a76a2a52c97dee9c9c8ed0a9030c1778c>k__BackingField";
constexpr char kHashOutPacketId[] =
    "a40d505bf94e3c9d0dbbc1dad4cfa27e37c562ef01c4fe5364e92e03c6f04af";
constexpr char kHashUserVecCtrl[] =
    "<dc76f5c9e250bc9a327a219b39e16c345cdabf7b01ad5c60b568045069c9120>k__BackingField";
constexpr char kHashVcAp[] =
    "a860e652f11e3e8846eaf4dfb600e319058d3e0e9e79b3fd7a3447344d98bb9";
constexpr char kHashVcApl[] =
    "ddcaef33563d49269da8f9db8391866dfc59ec057b8cca4ffa15a5b38f271b3";

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
    void* wm = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
    void* actor = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    if (!actor) actor = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* vc = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* pkt = x::runtime::il2cpp::FindClass("", kPacketClass);
    if (!pkt) pkt = x::runtime::il2cpp::FindClass("", kOutPacketClass);
    int hits = 0;
    auto hit = [&](bool ok) {
        if (ok) ++hits;
    };
    hit(TravelFieldOffHit(portal, kHashPortalData, kFbPortalData, &gOffPortalData, 0x08, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdId, kFbMpdId, &gOffMpdId, 0x08, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdType, kFbMpdType, &gOffMpdType, 0x08, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdEnable, kFbMpdEnable, &gOffMpdEnable, 0x08, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdPName, kFbMpdPName, &gOffMpdPName, 0x10, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdX, kFbMpdX, &gOffMpdX, 0x18, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdY, kFbMpdY, &gOffMpdY, 0x18, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdToMapId, kFbMpdToMapId, &gOffMpdToMapId, 0x18, 0x40));
    hit(TravelFieldOffHit(mpd, kHashMpdPortalRect, kFbMpdPortalRect, &gOffMpdPortalRect, 0x40, 0x90));
    hit(TravelFieldOffHit(mpd, kHashMpdHRange, kFbMpdHRange, &gOffMpdHRange, 0x40, 0x90));
    hit(TravelFieldOffHit(mpd, kHashMpdVRange, kFbMpdVRange, &gOffMpdVRange, 0x40, 0x90));
    hit(TravelFieldOffHit(mpd, kHashMpdVImpact, kFbMpdVImpact, &gOffMpdVImpact, 0x40, 0x90));
    hit(TravelFieldOffHit(mpd, kHashMpdHImpact, kFbMpdHImpact, &gOffMpdHImpact, 0x40, 0x90));
    hit(TravelFieldOffHit(wm, kHashWmFieldKey, kFbWmFieldKey, &gOffWmFieldKey, 0x60, 0xA0));
    hit(TravelFieldOffHit(pkt, kHashPacketBuffer, kFbPacketBuffer, &gOffPacketBuffer, 0x08, 0x40));
    hit(TravelFieldOffHit(pkt, kHashPacketOffset, kFbPacketOffset, &gOffPacketOffset, 0x08, 0x40));
    hit(TravelFieldOffHit(pkt, kHashOutPacketId, kFbOutPacketId, &gOffOutPacketId, 0x10, 0x40));
    hit(TravelFieldOffHit(actor, kHashUserVecCtrl, kFbUserVecCtrl, &gOffUserVecCtrl, 0x40, 0x100));
    hit(TravelFieldOffHit(vc, kHashVcAp, kFbVcAp, &gOffVcAp, 0x80, 0x100));
    hit(TravelFieldOffHit(vc, kHashVcApl, kFbVcApl, &gOffVcApl, 0x80, 0x100));
    constexpr int kExpect = 20;
    x::runtime::LogI("Travel",
                     "travel slots path=%s hits=%d/%d mpdId=0x%zX pn=0x%zX rect=0x%zX fk=0x%zX "
                     "vc=0x%zX ap=0x%zX myUser=0x%zX",
                     hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect,
                     gOffMpdId, gOffMpdPName, gOffMpdPortalRect, gOffWmFieldKey, gOffUserVecCtrl,
                     gOffVcAp, x::ui::player::OffWmMyUser());
}

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnCompGo = void* (*)(void* comp, void* method);
using FnObjName = void* (*)(void* go, void* method);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnStrNew = void* (*)(const char* str);
struct Vector3 {
    float x, y, z;
};
using FnSetPos = void (*)(void* self, Vector3* value, const void* method);
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
FnSetPos gSetPos = nullptr;
MethodInfoHead* gMiSetPos = nullptr;
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
// ???? Ap???? WaitStood????????????fh=0 ????BIN ???????
constexpr DWORD kTeleportSettleMs = 80;
constexpr DWORD kStandPollMs = 40;
// BIN b71cfd: 400ms ??? native ?????? NOT_STOOD ?????
constexpr DWORD kStandWaitMaxMs = 1200;
constexpr float kStandYTol = 12.f;
constexpr float kStickNearR = 72.f;
// WaitStood / ???????????fh snap ?? X ???? 8px??
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

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !rva) return nullptr;
    const auto& ex = x::runtime::il2cpp::Get();
    if (!ex.classGetMethods) return nullptr;
    HMODULE ga = gGA ? gGA : ex.ga;
    if (!ga) return nullptr;
    void* target = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(ga) + rva);
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* miRaw = ex.classGetMethods(cur, &iter);
                if (!miRaw) break;
                auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
                void* mp = nullptr;
                void* vp = nullptr;
                __try {
                    mp = mi->methodPointer;
                    vp = mi->virtualMethodPointer;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
                if (mp == target || vp == target) return mi;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!ex.classParent) break;
        void* parent = nullptr;
        __try {
            parent = ex.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        const int tryArgc[] = {argc, -1};
        for (int ac : tryArgc) {
            __try {
                mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, ac));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mi = nullptr;
            }
            if (mi && mi->methodPointer) return mi;
        }
    }
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* raw = e.classGetMethods(cur, &iter);
                if (!raw) break;
                const char* nm = e.methodGetName(raw);
                if (nm && strcmp(nm, name) == 0) {
                    mi = reinterpret_cast<MethodInfoHead*>(raw);
                    if (mi && mi->methodPointer) return mi;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
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

// Unity ??????FindMethodResolved = ?? ? RVA/kind?set_position ??????
MethodInfoHead* ResolveUnityMi(void* klass, uint32_t rva, const char* plain,
                               const x::runtime::il2cpp_method::MethodShape& shape,
                               x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, nullptr);
    if (outPath) *outPath = mr.path;
    if (mr.method && mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
        x::runtime::LogI("Travel", "ResolveUnityMi kind hit rva=0x%X plain=%s", rva,
                         plain ? plain : "-");
    }
    if (mr.method) return reinterpret_cast<MethodInfoHead*>(mr.method);
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
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

void WriteF64(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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

bool GetGoName(void* comp, char* out, int outSz) {
    if (!comp || !out || outSz <= 0 || !gCompGo || !gObjName) return false;
    out[0] = 0;
    __try {
        void* go = gCompGo(comp, nullptr);
        if (!go) return false;
        void* nameObj = gObjName(go, nullptr);
        return ReadIl2CppString(nameObj, out, outSz);
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

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    // ??????????set_position????exports ?????????TravelPos ????
    if (!gMiSetPos) {
        void* tfKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Transform");
        if (tfKlass) {
            constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Any}};
            gMiSetPos = ResolveUnityMi(tfKlass, kRvaTfSetPos, "set_position", kSet);
        }
    }
    gSetPos = FnFromMi<FnSetPos>(gMiSetPos, kRvaTfSetPos);
    // TravelPos ??????????????teleport???????????????
    if (gMiSetPos) {
        static bool sLoggedOk = false;
        if (!sLoggedOk) {
            x::runtime::LogI("Travel", "unity methods path=plain hits=1/1 set_position mi=%p rva=0x%X",
                             gMiSetPos, kRvaTfSetPos);
            sLoggedOk = true;
        }
        x::runtime::anchor_lamps::Set("TravelPos", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                     "set_position MI");
    } else if (gSetPos) {
        static bool sLoggedDeg = false;
        if (!sLoggedDeg) {
            x::runtime::LogW("Travel", "set_position MI miss ??RVA 0x%X fallback", kRvaTfSetPos);
            sLoggedDeg = true;
        }
        x::runtime::anchor_lamps::Set("TravelPos",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded, "RVA");
    } else {
        static bool sLoggedMiss = false;
        if (!sLoggedMiss) {
            x::runtime::LogW("Travel", "set_position resolve fail");
            sLoggedMiss = true;
        }
        x::runtime::anchor_lamps::Set("TravelPos", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "MISS");
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
    if (!gPm && gPmType && gFindAll) {
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
    if (!gLocalUser && gLuType && gFindAll) {
        void* arr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 1500);
        const int n = arr ? static_cast<int>(
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + kOffArrLen))
                          : 0;
        for (int i = 0; i < n && i < 32; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
                                                static_cast<size_t>(i) * sizeof(void*));
            if (!LooksLikeHeapPtr(o)) continue;
            char name[64]{};
            if (GetGoName(o, name, sizeof(name)) && strcmp(name, "MyUser") == 0) {
                gLocalUser = o;
                break;
            }
        }
    }

    void* facade = ResolveSingleton(gFacadeKlass);
    if (!facade && gFacadeType && gFindAll) {
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
        // Encode1(sbyte) dump ???EncodeStr(string) ???????????
        constexpr MethodShape kEnc{1, TypeKind::Void, true, false, {TypeKind::Any}};
        if (!gMiEncode1) {
            gMiEncode1 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode1Byte, kEnc, nullptr,
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

bool WarpLocalUser(float x, float y) {
    if (!LooksLikeHeapPtr(gLocalUser)) return false;
    void* vc = ReadPtr(gLocalUser, kOffUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    WriteF64(vc, kOffVcApX, x);
    WriteF64(vc, kOffVcApY, y);
    WriteF64(vc, kOffVcAplX, x);
    WriteF64(vc, kOffVcAplY, y);
    // Transform SetPos if available
    if (gSetPos && gCompGo) {
        __try {
            void* go = gCompGo(gLocalUser, nullptr);
            if (LooksLikeHeapPtr(go)) {
                // Transform often @ Component+0x10 ??wait, Unity Component has m_CachedPtr;
                // LocalUser may hold Transform separately. Skip if unclear ??AbsPos is enough for
                // many portal proximity checks.
                (void)go;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return true;
}

}  // namespace

bool EnsureBound() { return RebindManagers(GetTickCount()); }

void Init() {
    // ???? set_position??????????EnsureBound??
    const bool api = ResolveApi();
    if (gMiSetPos) {
        x::runtime::LogI("Travel", "port init api ready setPosMi=1");
    } else if (api && gSetPos) {
        x::runtime::LogW("Travel", "port init set_position degraded (RVA; MI pending upgrade)");
    } else {
        x::runtime::LogW("Travel", "port init ResolveApi incomplete setPosMi=0");
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
    bool warpFirst = true;
    float x = 0.f;
    float y = 0.f;
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
        // TeleportStick / DirectEnter??????teleport_port ??worker ????????????
        // Up / CheckMove???? AbsPos ????????Doing ??????
        const bool productEnter = (job->mode == FireMode::TeleportStick ||
                                   job->mode == FireMode::DirectEnter);
        if (job->warpFirst && !productEnter) {
            if (!WarpLocalUser(job->x, job->y)) {
                snprintf(job->result, sizeof(job->result), "NO_LOCALUSER");
                return;
            }
        }

        if (job->mode == FireMode::Up || job->mode == FireMode::TeleportStick) {
            job->ok = CallUpKey(job->result, sizeof(job->result));
            if (job->ok && job->mode == FireMode::TeleportStick)
                snprintf(job->result, sizeof(job->result), "FIRED_TP_STICK");
            return;
        }

        if (job->mode == FireMode::CheckMove || job->mode == FireMode::DirectEnter) {
            job->ok = CallCheckMovePortal(job->result, sizeof(job->result));
            if (job->ok && job->mode == FireMode::DirectEnter)
                snprintf(job->result, sizeof(job->result), "FIRED_DIRECT");
            return;
        }

        // Rpc?Create(114) + Encode1(fieldKey) + EncodeStr(pn) + Send
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

// teleport_port::TeleportNativeSkillCall ?? InvokeAndWait????? FireJobOnMain ???????
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

// Wait until Ap is near stand (and inside portal trigger when rectValid).
// Without inRect, stick can succeed while pre-fire OUT_OF_RECT loops (Bugbot).
bool WaitStoodNearStand(float standX, float standY, uint32_t expectFh, const PortalInfo* portal,
                        std::string& outResult) {
    const DWORD t0 = GetTickCount();
    for (;;) {
        if (!world::IsInMapScene() || !world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            return true;
        }
        float ax = 0.f, ay = 0.f;
        if (ReadLocalAp(ax, ay)) {
            const uint32_t curFh = foothold::PeekCurFhId();
            const float dx = ax - standX;
            const float dy = ay - standY;
            const bool geoOk =
                std::fabs(dy) <= kStandYTol && (dx * dx + dy * dy) <= (kStickNearR * kStickNearR);
            const bool rectOk = !portal || PointInPortalRect(*portal, ax, ay);
            if (geoOk && rectOk) {
                outResult = "OK";
                (void)expectFh;
                (void)curFh;
                return true;
            }
        }
        if (GetTickCount() - t0 >= kStandWaitMaxMs) break;
        Sleep(kStandPollMs);
    }
    outResult = "NOT_STOOD";
    float ax = 0.f, ay = 0.f;
    const bool gotAp = ReadLocalAp(ax, ay);
    x::runtime::LogW("Travel",
                     "stand wait timeout near=(%.0f,%.0f) expectFh=%u curFh=%u ap=(%.0f,%.0f) "
                     "gotAp=%d inRect=%d",
                     standX, standY, (unsigned)expectFh, (unsigned)foothold::PeekCurFhId(),
                     gotAp ? ax : 0.f, gotAp ? ay : 0.f, gotAp ? 1 : 0,
                     (gotAp && portal) ? (PointInPortalRect(*portal, ax, ay) ? 1 : 0) : -1);
    return false;
}

bool TeleportToPortal(const PortalInfo& portal, std::string& outResult, float* outSx, float* outSy) {
    float aimX = portal.x;
    float aimY = portal.y;
    if (portal.rectValid) {
        // ???????????????????? FindMovePortal ????
        if (!PointInPortalRect(portal, aimX, aimY)) ClampIntoPortalRect(portal, aimX, aimY);
    }
    if (outSx) *outSx = aimX;
    if (outSy) *outSy = aimY;
    // ??????????/ ??PlayReady ?????????? Doing??
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        outResult = "NOT_PLAY_READY";
        return false;
    }
    // BIN 15:57?scene ??Map ??MyUser ????Doing ?????????? Ap
    float luX = 0.f, luY = 0.f;
    if (!ReadLocalAp(luX, luY)) {
        outResult = "NO_LOCALUSER";
        return false;
    }
    (void)luX;
    (void)luY;
    if (!ports::teleport::EnsureBound()) {
        outResult = "TELEPORT_UNBOUND";
        return false;
    }

    // BIN 07:26: forbid fh=0 stick. Portal: Walk chain covering door, then flattest
    // segment intersecting PortalRect, SnapOnFh.
    float standX = aimX, standY = aimY;
    uint32_t fh = 0;
    if (!ports::foothold_path::SnapStandForPortal(aimX, aimY, portal.rectL, portal.rectT,
                                                 portal.rectR, portal.rectB, portal.rectValid,
                                                 &standX, &standY, &fh) ||
        fh == 0) {
        outResult = "FH0_FORBID";
        x::runtime::LogW("Travel", "teleport forbid fh=0 portal=(%.0f,%.0f) aim=(%.0f,%.0f) "
                                   "snap=(%.0f,%.0f)",
                         portal.x, portal.y, aimX, aimY, standX, standY);
        return false;
    }
    // Snap may land outside trigger rect (BIN b71cfd). Prefer portal center X,
    // re-pick on same chain+rect rules, then force X into rect if still out.
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
            // Keep Y/fh from last snap; pin X to portal center (or rect edge).
            standX = targetX;
            ClampIntoPortalRect(portal, standX, standY);
            // Re-pin onto chosen FH so X clamp does not slide onto a slope neighbor.
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
                         "snap clamp name=%s before=(%.0f,%.0f) after=(%.0f,%.0f) rect=(%.0f,%.0f)-"
                         "(%.0f,%.0f) in=%d",
                         portal.name.c_str(), beforeX, beforeY, standX, standY, portal.rectL,
                         portal.rectT, portal.rectR, portal.rectB,
                         PointInPortalRectStrict(portal, standX, standY) ? 1 : 0);
    }
    if (outSx) *outSx = standX;
    if (outSy) *outSy = standY;

    x::runtime::LogI("Travel",
                     "stick aim name=%s portal=(%.0f,%.0f) rect=%d (%.0f,%.0f)-(%.0f,%.0f) "
                     "stand=(%.0f,%.0f) fh=%u inRect=%d",
                     portal.name.c_str(), portal.x, portal.y, portal.rectValid ? 1 : 0, portal.rectL,
                     portal.rectT, portal.rectR, portal.rectB, standX, standY, (unsigned)fh,
                     PointInPortalRect(portal, standX, standY) ? 1 : 0);

    // snapStand=false when rectValid: Native must not re-Snap (BIN b71cfd bounce).
    // BIN bbda00 101010000/west00: fill lands inRect then foothold slides Ap to
    // (-1591) outside ?20 box within ~1s ? NOT_STOOD fuse. Re-pin ASAP and
    // proceed as soon as Ap is strictly inRect (do not wait full stood window).
    ports::teleport::SetNativeCooldownMs(80);
    const bool reSnap = !portal.rectValid;
    constexpr int kRepinMax = 4;
    constexpr DWORD kQuickInRectMs = 280;
    bool stoodOk = false;
    for (int pin = 0; pin < kRepinMax; ++pin) {
        if (pin > 0) {
            if (!portal.rectValid) break;
            ports::teleport::ClearNativeSelfCd();
            ports::teleport::SetNativeCooldownMs(50);
            x::runtime::LogW("Travel", "re-pin name=%s pin=%d stand=(%.0f,%.0f) fh=%u",
                             portal.name.c_str(), pin, standX, standY, (unsigned)fh);
        }
        if (!ports::teleport::TeleportNativeSkillCall(standX, standY, fh, /*snapStand=*/reSnap)) {
            outResult = "TELEPORT_FAIL";
            return false;
        }
        if (!world::IsInMapScene() || !world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            return true;
        }
        Sleep(portal.rectValid ? 20 : kTeleportSettleMs);
        if (!world::IsInMapScene() || !world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            return true;
        }

        const DWORD t0 = GetTickCount();
        for (;;) {
            if (!world::IsInMapScene() || !world::IsPlayReady()) {
                outResult = "MAP_TRANSITION";
                return true;
            }
            float ax = 0.f, ay = 0.f;
            if (ReadLocalAp(ax, ay)) {
                if (portal.rectValid) {
                    // CheckMove needs engine trigger; fire as soon as Ap is inside.
                    if (PointInPortalRectStrict(portal, ax, ay)) {
                        x::runtime::LogI("Travel",
                                         "stood name=%s ap=(%.0f,%.0f) inRect=1 pin=%d",
                                         portal.name.c_str(), ax, ay, pin);
                        stoodOk = true;
                        break;
                    }
                } else {
                    const float dx = ax - standX;
                    const float dy = ay - standY;
                    const bool nearStand =
                        std::fabs(dy) <= kStandYTol &&
                        (dx * dx + dy * dy) <= (kStickNearR * kStickNearR);
                    if (nearStand) {
                        x::runtime::LogI("Travel",
                                         "stood name=%s ap=(%.0f,%.0f) inRect=1 pin=%d",
                                         portal.name.c_str(), ax, ay, pin);
                        stoodOk = true;
                        break;
                    }
                }
            }
            if (GetTickCount() - t0 >= (portal.rectValid ? kQuickInRectMs : kStandWaitMaxMs))
                break;
            Sleep(kStandPollMs);
        }
        if (stoodOk) break;

        float ax = 0.f, ay = 0.f;
        if (ReadLocalAp(ax, ay)) {
            x::runtime::LogW("Travel",
                             "post-tp drift name=%s pin=%d ap=(%.0f,%.0f) stand=(%.0f,%.0f) "
                             "inStrict=%d",
                             portal.name.c_str(), pin, ax, ay, standX, standY,
                             PointInPortalRectStrict(portal, ax, ay) ? 1 : 0);
        }
        // BIN d1a58e / bbda00 west00?fill ? Walk ?? Ap ???????????????
        if (portal.rectValid) {
            struct LatchJob {
                bool ok = false;
            } latch{};
            auto latchFn = [](void* p) {
                auto* j = static_cast<LatchJob*>(p);
                if (!j) return;
                j->ok = ports::teleport::ClearMotionLatchMainThread();
            };
            (void)x::runtime::main_thread::Ensure();
            (void)x::runtime::main_thread::InvokeAndWait(
                latchFn, &latch, 80, x::runtime::main_thread::JobPrio::High);
        }
        if (!portal.rectValid) break;
    }

    if (!stoodOk) {
        if (!WaitStoodNearStand(standX, standY, fh, &portal, outResult)) return false;
    }
    if (outSx) *outSx = standX;
    if (outSy) *outSy = standY;
    return true;
}

// ?????????????????????Doing????fh=0 ????????
bool StickThenEnterReady(const PortalInfo& portal, std::string& outResult) {
    const std::string mapBefore = CurrentMapKey();
    float sx = portal.x, sy = portal.y;

    auto acceptStuck = [&]() -> bool {
        const std::string mapAfter = CurrentMapKey();
        if (!mapBefore.empty() && !mapAfter.empty() && mapAfter != mapBefore) {
            outResult = "MAP_CHANGED";
            return true;
        }
        outResult = "STUCK";
        return true;
    };

    if (!TeleportToPortal(portal, outResult, &sx, &sy)) {
        if (outResult != "NOT_STOOD") return false;
    } else if (outResult == "MAP_TRANSITION") {
        x::runtime::LogI("Travel", "stick ??map transition name=%s", portal.name.c_str());
        return true;
    } else {
        return acceptStuck();
    }

    if (!world::IsPlayReady()) {
        outResult = "MAP_TRANSITION";
        return true;
    }
    x::runtime::LogW("Travel", "re-stick name=%s (not_stood)", portal.name.c_str());
    float sx2 = sx, sy2 = sy;
    if (!TeleportToPortal(portal, outResult, &sx2, &sy2)) return false;
    if (outResult == "MAP_TRANSITION") return true;
    return acceptStuck();
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
        mode != FireMode::TeleportStick && mode != FireMode::DirectEnter) {
        outResult = "PORTAL_DISABLED";
        return false;
    }

    // ?????worker ???????????????????????
    if ((mode == FireMode::TeleportStick || mode == FireMode::DirectEnter) && warpFirst) {
        // ?????ForceNative ??????????Doing?BIN 16:11?????????? ??205??
        const uint32_t rem = ports::teleport::NativeCooldownRemainingMs();
        if (rem > 0) {
            outResult = "TELEPORT_COOLDOWN";
            return false;
        }
        if (!StickThenEnterReady(p, outResult)) return false;
        // ????????????CheckMove?WM/LU ??????
        if (outResult == "MAP_TRANSITION" || outResult == "MAP_CHANGED") {
            x::runtime::LogI("Travel", "skip CheckMove (%s) name=%s", outResult.c_str(),
                             portalName.c_str());
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
            // CheckMove/Up miss silently when outside FindMovePortal rect ? do not
            // report FIRED (uniqueBridge soft-fail). Retry via transient OUT_OF_RECT.
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
    // ??????????????AbsPos ??
    job.warpFirst =
        warpFirst && !(mode == FireMode::TeleportStick || mode == FireMode::DirectEnter);
    job.x = p.x;
    job.y = p.y;
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
    case FireMode::TeleportStick:
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
