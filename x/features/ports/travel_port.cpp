// Classic TWMS travel_port ??PortalManager / MapData / ???? / ???? / ??????
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "travel_port.h"

#include "fly_fh_ban.h"
#include "foothold_path.h"
#include "teleport_port.h"
#include "unity_kbd_port.h"
#include "world_port.h"
#include "../invuln/invuln.h"
#include "../simple_combat/heli_rotor.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_mapdata.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/bin_dir.h"
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
#include <cstdlib>
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
constexpr uint32_t kRvaSendOutPacket = 0x1CC64D0;  // Network.SendOutPacket → 直调 SendPacket RVA
// CMS ClientPacket.UserPortalTeleportRequest = 114 · wire 0x0072（Rpc 伪造仍用 enum）
constexpr int kClientPortalTeleport = 114;
constexpr uint16_t kWirePortalTeleport = 0x0072;
// CheckMovePortal 真出站（运行时 dump 种子实算 · imagebase 无关 RVA）：
//   D0D0 主路径（FindMovePortal 成功）：Create(u16) ← word_seed+0xFFFFE527 → 0x0138
//   备路 Create：word_seed+0x5CF9 → 0x0116
// StickUp 产品只 unity_kbd ↑；直调 CheckMovePortal 贴门后易断线（BIN 2026-08-08）。
constexpr uint16_t kWirePortalMove = 0x0138;
constexpr uint16_t kWirePortalMoveAlt = 0x0116;

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
void* gOutPacketKlass = nullptr;
#define kOffNmSession (x::runtime::il2cpp_network::OffNmSession())
DWORD gLastRebindMs = 0;

// ??????????+ ?? CheckMovePortal????
std::atomic<int> gFireMode{static_cast<int>(FireMode::StickUp)};
// 已站门判定：无 rect 时用门坐标近距；有 rect 用 Strict 触发框。
constexpr float kStandYTol = 12.f;
constexpr float kStickNearR = 72.f;
// Impact 贴门：对齐 F5 旋翼滑翔（heli Cruise/Station → ImpactSetVelocity）。
// 全程面板「滑翔速度」倍率（与打怪同款）；接近只切 Station，不压倍率。
// Station 带滞后：进圈锁住，飘出须超过 enter+slack 才回 Cruise（防档位抽风）。
// 到位：Station → hold（停 Move + 卸 Travel ban 落地）→ ↑（禁止掠过即火）。
constexpr DWORD kImpactStickMaxMs = 14000;  // 含 settle + hold + pre-fire land
constexpr DWORD kImpactStickPollMs = 16;  // 对齐 combat tick；heli 内部 ~90ms 发射闸
// 摘台前等 soft_login 静默闸放行的上限；到点仍未开就照旧起飞（宁可掉一下也别卡死行程）。
constexpr DWORD kDetachQuietWaitMaxMs = 2000;
constexpr float kHeliCruiseRadius = 140.f;  // 与 simple_combat PublishHeliSetpoint 同口径
// Station 滞后：进入 ≤enter；退出须 >enter+slack（对齐打怪「一次运动到站」）。
constexpr float kTravelStationEnterR = kHeliCruiseRadius;
constexpr float kTravelStationExitSlack = 80.f;
constexpr float kPortalStationDx = 40.f;    // = simple_combat::kHeliStationDx
constexpr float kPortalStationDy = 15.f;    // = simple_combat::kHeliStationDy
// 贴门瞄准相对站立点再抬高（AbsPos：更大 Y = 更高，远离掉落方向）。
// BIN：aimY-= 会把点压到台下 → 无限掉（east01 aim -389 / ap -735）。
constexpr float kPortalAimLiftY = 24.f;
constexpr float kPortalSettleSpeed = 80.f;  // 近站合速（进 hold 前还要过横/竖速与贴高）
constexpr DWORD kPortalSettleMaxMs = 2500;  // 进框后最多等多久；超时仍须 bleed 才卸推
// 进 hold 前「门边刹住」：高门 BIN 常带 v.y=64~125 就 Disarm → 掉出框 abort×2~3。
constexpr float kPortalHoldEnterVy = 35.f;  // 竖速硬门槛
// BIN 02:44：10X 贴门 hold 时仍 vx≈60 → Disarm 后甩上台。合速≤80 挡不住单轴横冲。
// 横速也收干净再卸推；**不**开 hold zero-vel（kill-switch 仍在）。
constexpr float kPortalHoldEnterVx = 30.f;
// bleed 贴「台面 Y」(Snap/portal.y)，禁止对抬高后的 aimY。
// BIN 07:09：aim=portal+24 / ap 在台下 → dy 对 aim 永远 >20 → 悬空掉落 NOT_STOOD。
// 台下常见 |ap.y-landY|≈30~40，故带宽 > lift，避免永远进不了 hold。
constexpr float kPortalHoldEnterDy = 40.f;
// AbsPos 更大 Y=更高。Disarm 时禁止人已在台面下方（BIN 07:17：hold@-325 landY=-287
// → 掉到 -587 left-trigger 循环）。最多允许比 landY 低这么多。
constexpr float kPortalHoldBelowMax = 12.f;
// 末段（进圈/触发框）旋翼 Y 用 landY+该抬高，避免 +24 悬空再卸推掉穿。
constexpr float kPortalFinalAimLiftY = 8.f;
// hold 中短暂出框宽限；未挂上 FH 时另用 nearAim 保海岸（BIN 50000 west00：
// hold≈500ms 出框 → abort → 10X Station 甩到 vy≈500 循环 → NOT_STOOD）。
constexpr DWORD kPortalHoldLeaveGraceMs = 800;
constexpr float kPortalHoldNearAimDx = 80.f;
constexpr float kPortalHoldNearAimDy = 160.f;
constexpr float kPortalHoldAbortDist = 220.f;
// 站稳真源 = AbsPos 连续静止 + 合速门槛；漂移/滑步 → 整段重等。
// BIN 02:06 曾拉到 800 防 10X 黑屏；黑屏已另修，800+250 每跳体感拖沓 → 400+120。
constexpr DWORD kPortalReadyStableMs = 400;
constexpr float kPortalReadyApDrift = 8.f;   // 相对锚点 |ΔAp|
constexpr float kPortalReadyApStep = 4.f;    // 相邻采样 |ΔAp|（滑步）
// 卸 ban 后挂不上 FH：失败上限（与「就绪即走」解耦）。
constexpr DWORD kPortalLandTimeoutMs = 4000;
// 就绪后再抽一拍确认仍站稳（含 Ap 未漂）。
constexpr DWORD kPortalPreFireLandMs = 120;
// 发门合速辅助门槛（px/s）；主门仍是 Ap 静止。
constexpr float kPortalFireSpeed = 18.f;
// 发门前 |ap.x-portal.x|：Snap 内缩曾把 aim 挪离门心（BIN 100040000 east00：
// portal=1120 aim=1096 ap=1090 → 站门左假火）。瞄准锁门 X；开火也卡门 X。
constexpr float kPortalFireMaxDx = 16.f;
// FindMovePortal 触发框松弛（pre-fire / PointInPortalRect）
constexpr float kPortalRectSlop = 12.f;
std::atomic<bool> gCaptureOn{false};
std::atomic<bool> gCaptureInstalled{false};
MethodInfoHead* gMiSend = nullptr;
void* gOrigSend = nullptr;  // Abs trampoline（直调 SendPacket 体）
MethodInfoHead* gMiCheckMove = nullptr;
MethodInfoHead* gMiOutCreate = nullptr;
MethodInfoHead* gMiEncode1 = nullptr;
MethodInfoHead* gMiEncodeStr = nullptr;

// Abs hook：SendOutPacket 直调 SendPacket RVA，MI 补丁捕不到 ↑ 进门包。
struct SendAbsHook {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};
SendAbsHook gSendAbs{};
// push rbp/rsi/rdi; sub rsp,0A0h; lea rbp,[rsp+80h] = 15B（remounted 2026-08-06）
constexpr uint8_t kSendSig[15] = {0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0xA0,
                                  0x48, 0x8D, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00};
constexpr size_t kSendSteal = 15;

std::mutex gLastCapMu;
std::string gLastCapHex;
uint16_t gLastCapId = 0;
DWORD gLastCapTick = 0;
// capture 窗内全部出站 id（诊断白枪：有没有 0x138）
constexpr size_t kCapIdRing = 24;
uint16_t gCapIdRing[kCapIdRing]{};
size_t gCapIdN = 0;

void ClearLastCap() {
    std::lock_guard<std::mutex> lock(gLastCapMu);
    gLastCapHex.clear();
    gLastCapId = 0;
    gLastCapTick = 0;
    gCapIdN = 0;
}

bool IsPortalWireId(uint16_t id) {
    return id == kWirePortalMove || id == kWirePortalMoveAlt || id == kWirePortalTeleport ||
           id == (uint16_t)kClientPortalTeleport || id == 0x0071;
}

// ↑ 后短等，看有没有打出传送 C2S（首枪假火：吞键 vs CheckMovePortal 未发 0x138）。
void LogUpPortalCap(const char* tag) {
    uint16_t id = 0;
    std::string hex;
    DWORD age = 0;
    char ring[160]{};
    {
        std::lock_guard<std::mutex> lock(gLastCapMu);
        id = gLastCapId;
        hex = gLastCapHex;
        if (gLastCapTick) age = GetTickCount() - gLastCapTick;
        int nhex = 0;
        for (size_t i = 0; i < gCapIdN && nhex + 8 < (int)sizeof(ring); ++i)
            nhex += snprintf(ring + nhex, sizeof(ring) - (size_t)nhex, "%s0x%04X",
                             i ? "," : "", (unsigned)gCapIdRing[i]);
    }
    if (id != 0) {
        x::runtime::LogI("Travel", "Up C2S %s id=0x%04X age=%ums hex=%s all=[%s]",
                         tag ? tag : "?", (unsigned)id, (unsigned)age, hex.c_str(), ring);
    } else {
        x::runtime::LogW("Travel",
                         "Up C2S NONE %s (无 0x138/0x116；窗内 all=[%s])",
                         tag ? tag : "?", ring[0] ? ring : "-");
    }
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool InstallSendAbs(void* target, void* hook) {
    if (gSendAbs.active) return true;
    if (!target || !hook) return false;
    for (size_t i = 0; i < sizeof(kSendSig); ++i) {
        if (reinterpret_cast<uint8_t*>(target)[i] != kSendSig[i]) {
            x::runtime::LogW("Travel", "Send Abs refuse: sig mismatch @%p b0=%02X", target,
                             reinterpret_cast<uint8_t*>(target)[0]);
            return false;
        }
    }
    void* tramp =
        VirtualAlloc(nullptr, kSendSteal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(gSendAbs.saved, target, kSendSteal);
    memcpy(tramp, target, kSendSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kSendSteal,
                reinterpret_cast<uint8_t*>(target) + kSendSteal);
    gOrigSend = tramp;
    DWORD old = 0;
    if (!VirtualProtect(target, kSendSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        gOrigSend = nullptr;
        return false;
    }
    WriteAbsJmp(target, hook);
    for (size_t i = 12; i < kSendSteal; ++i) reinterpret_cast<uint8_t*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, kSendSteal);
    VirtualProtect(target, kSendSteal, old, &old);
    gSendAbs.target = target;
    gSendAbs.trampoline = tramp;
    gSendAbs.stolen = kSendSteal;
    gSendAbs.active = true;
    return true;
}

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
    {
        std::lock_guard<std::mutex> lock(gLastCapMu);
        if (gCapIdN < kCapIdRing) gCapIdRing[gCapIdN++] = id;
        if (IsPortalWireId(id)) {
            gLastCapId = id;
            gLastCapHex = hex;
            gLastCapTick = GetTickCount();
        }
    }
}

bool __fastcall HookNmSend(void* self, void* outPacket, const void* method) {
    if (gCaptureOn.load() && outPacket) {
        const uint16_t id = ReadU16(outPacket, kOffOutPacketId);
        if (IsPortalWireId(id))
            DumpOutPacket(outPacket, "portal");
        else {
            // 窗内非门包只记 id，避免刷屏；LogUpPortalCap 的 all=[] 用它证「钩子活着」
            std::lock_guard<std::mutex> lock(gLastCapMu);
            if (gCapIdN < kCapIdRing) gCapIdRing[gCapIdN++] = id;
        }
    }
    auto* orig = reinterpret_cast<FnNmSend>(gOrigSend);
    return orig ? orig(self, outPacket, method) : false;
}

// 懒装：默认不钩 Send；仅 capture on（或 StickUp 且已开探针）时安装。
void EnsureSendCaptureInstalled() {
    if (gCaptureInstalled.load()) return;
    if (!gGA) return;
    void* sendBody = AtRva<void*>(kRvaNmSend);
    if (InstallSendAbs(sendBody, reinterpret_cast<void*>(&HookNmSend))) {
        gCaptureInstalled.store(true);
        x::runtime::LogI("Travel",
                         "Send Abs capture ok target=%p tramp=%p (wire portal=0x%04X/0x%04X)",
                         sendBody, gSendAbs.trampoline, (unsigned)kWirePortalMove,
                         (unsigned)kWirePortalMoveAlt);
        x::runtime::anchor_lamps::Set("TravelSend", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                     "Abs ok");
        (void)kRvaSendOutPacket;
        return;
    }
    if (gMiSend && PatchMethodInfo(gMiSend, reinterpret_cast<void*>(&HookNmSend), &gOrigSend)) {
        gCaptureInstalled.store(true);
        x::runtime::LogW("Travel", "Send Abs fail → MethodInfo fallback mi=%p", (void*)gMiSend);
        x::runtime::anchor_lamps::Set("TravelSend",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded, "MI only");
        return;
    }
    x::runtime::anchor_lamps::Set("TravelSend", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                 "MISS");
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
    if (!gOutPacketKlass) {
        gOutPacketKlass = FindClass("OutPacket");
        if (!gOutPacketKlass) gOutPacketKlass = FindClass(kOutPacketClass);
    }

    // PortalManager 与 NetworkManager facade 都不是 UnityEngine.Object（facade 还是 Singleton<>），
    // Resources.FindObjectsOfTypeAll 会被引擎的类型闸当场拒掉：永远拿不到对象，只会往客户端
    // Player.log 刷一行 "The type has to be derived from UnityEngine.Object"，同时白占一个
    // 1500ms 的主泵 job。2026-08-09 实测一局刷了 454 行（两类各 227 次，即每 2s 一轮 rebind
    // 各来一次），把引擎日志淹到 83%，换图崩溃现场无从查起。所以这两个只走单例槽，
    // 扫不到就等下一轮 rebind。
    gPm = ResolveSingleton(gPmKlass);

    gWm = world::GetWorldManager();

    gLocalUser = nullptr;
    if (gWm) {
        void* mu = ReadPtr(gWm, kOffWmMyUser);
        if (LooksLikeHeapPtr(mu)) gLocalUser = mu;
    }
    // UserLocal 是 UnityEngine.Object，FindAll 对它有效；InterStage 仍要禁（WM.MyUser 够用）。
    const bool allowFindAll = world::IsPlayReady();
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

    // 同上：facade 不吃 FindAll，只走单例槽。
    void* facade = ResolveSingleton(gFacadeKlass);
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
        // Send Abs 探针默认不装；仅 SetCaptureEnabled(true) → EnsureSendCaptureInstalled。
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

// StickUp/Up：拟人走路同款 unity_kbd（InputSystem 设备态）。
// BIN 12:08：managed_main→FireJob→InjectKeyHold→EnqueueAndWait(OnKey)
// 主线程里再等主线程 → ~2s KEY_FAIL + 卡游戏。按住时长只 Sleep 在 worker。
constexpr DWORD kPortalUpHoldMs = 160;
constexpr DWORD kPortalUpPollMs = 16;
// StickUp：最短按住 kPortalUpHoldMs；若仍同图且 PlayReady，最多撑到本值等 MapId
//（BIN 02:34 hop2：160ms 松键后 ~29ms MapId 才变 → 半截进门 InterStage 黑屏）。
constexpr DWORD kPortalUpHoldMaxMs = 700;
// MapId 已变后额外按住：旧 drain 只撑到 HoldMs，若变图发生在 160ms 之后会立刻松键
//（BIN 02:42：drain@203ms → held=203 → Field 永不回）。
constexpr DWORD kPortalUpPostChangeDrainMs = 220;
// StickUp 首枪后补发间隔：覆盖服端偶发吞 ↑（仍站在门内时）。
constexpr DWORD kPortalUpFollowGapMs = 72;

// BIN 2026-08-09 01:01：首枪 ↑ 后 MapId 已变仍补第二枪 → InterStage 黑屏。
// 现路径：MapId/PlayReady 闸后再决定是否补枪；hold 内 MapId 变了默认撑满再松（UpDrain）。
// 回退：
//   · XCAT_TRAVEL_MAPID_GATE=0  或 DLL 旁 travel_mapid_gate.off → 关掉 MapId 闸
//   · XCAT_TRAVEL_UP2=0        或 DLL 旁 travel_up2.off        → 关补第二枪
constexpr bool kTravelMapIdGateDefault = true;
// BIN 18:47 already_in：首枪 HoldUntil timeout until=0（键按了门未吃），up2 关则空等
// PostFireQuiet 2.5s×2 才 soft 过。默认开补枪：仅首枪未换图时走（已换图早退）。
constexpr bool kTravelUp2Default = true;
// BIN 01:27 hop2：MapId 刚闪变就 releaseUp → 半截传送黑屏；默认撑满 hold。
// 回退：XCAT_TRAVEL_UP_DRAIN=0 或 travel_up_drain.off → 立刻松键（旧行为）。
constexpr bool kTravelUpDrainDefault = true;

bool EnvFlagOff(const char* name) {
    char env[16]{};
    if (GetEnvironmentVariableA(name, env, sizeof(env)) == 0) return false;
    return env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F';
}

bool EnvFlagOn(const char* name) {
    char env[16]{};
    if (GetEnvironmentVariableA(name, env, sizeof(env)) == 0) return false;
    return env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' || env[0] == 'T';
}

bool MarkerOffFile(const char* fileName) {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0] || !fileName || !fileName[0]) return false;
    char path[MAX_PATH]{};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", bin, fileName);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool TravelMapIdGateEnabled() {
    if (EnvFlagOff("XCAT_TRAVEL_MAPID_GATE") || MarkerOffFile("travel_mapid_gate.off")) {
        return false;
    }
    if (EnvFlagOn("XCAT_TRAVEL_MAPID_GATE")) return true;
    return kTravelMapIdGateDefault;
}

bool TravelUp2Enabled() {
    if (EnvFlagOff("XCAT_TRAVEL_UP2") || MarkerOffFile("travel_up2.off")) return false;
    if (EnvFlagOn("XCAT_TRAVEL_UP2")) return true;
    return kTravelUp2Default;
}

bool TravelUpDrainEnabled() {
    if (EnvFlagOff("XCAT_TRAVEL_UP_DRAIN") || MarkerOffFile("travel_up_drain.off")) {
        return false;
    }
    if (EnvFlagOn("XCAT_TRAVEL_UP_DRAIN")) return true;
    return kTravelUpDrainDefault;
}

// BIN 2026-08-09 01:22：MapId 闸已跳过补枪，但首枪 ↑ 仍黑屏。
// 曾在 enter-armed 直前 SetImpactNext(0,0)；BIN 01:44 仍黑屏 → 默认关。
// 清零改到 hold 始（fhBan ON，对齐 F6）。需要时：XCAT_TRAVEL_PREFIRE_ZERO=1
constexpr bool kTravelPreFireZeroDefault = false;

bool TravelPreFireZeroEnabled() {
    if (EnvFlagOff("XCAT_TRAVEL_PREFIRE_ZERO") || MarkerOffFile("travel_prefire_zero.off")) {
        return false;
    }
    if (EnvFlagOn("XCAT_TRAVEL_PREFIRE_ZERO")) return true;
    return kTravelPreFireZeroDefault;
}

// hold 始清零：BIN 02:06 仍 `hold zero-vel ok` → 首枪 ↑ → 永久 InterStage。
// 默认关；进门前禁止再写 SetImpactNext。需要时：XCAT_TRAVEL_HOLD_ZERO=1
constexpr bool kTravelHoldZeroDefault = false;

bool TravelHoldZeroEnabled() {
    if (EnvFlagOff("XCAT_TRAVEL_HOLD_ZERO") || MarkerOffFile("travel_hold_zero.off")) {
        return false;
    }
    if (EnvFlagOn("XCAT_TRAVEL_HOLD_ZERO")) return true;
    return kTravelHoldZeroDefault;
}

void TryClearImpactZero(const char* tag, const char* phase) {
    ports::teleport::ImpactVelOpts zopts{};
    zopts.minAbs = 0.f;
    zopts.quietLog = true;
    const bool ok = ports::teleport::ImpactSetVelocity(
        0.f, 0.f, ports::teleport::ImpactRoute::SetImpactNext, zopts);
    x::runtime::LogI("Travel", "%s zero-vel %s tag=%s (SetImpactNext 0,0)",
                     phase ? phase : "impact", ok ? "ok" : "fail", tag ? tag : "-");
    if (ok) Sleep(48);
}

void TryPreFireClearImpact(const char* tag) {
    if (!TravelPreFireZeroEnabled()) {
        x::runtime::LogI("Travel", "prefire zero-vel skipped (kill-switch) tag=%s",
                         tag ? tag : "-");
        return;
    }
    if (!world::IsPlayReady()) {
        x::runtime::LogI("Travel", "prefire zero-vel skipped (not_play_ready) tag=%s",
                         tag ? tag : "-");
        return;
    }
    TryClearImpactZero(tag, "prefire");
}

void TryHoldClearImpact(const char* tag) {
    if (!TravelHoldZeroEnabled()) {
        x::runtime::LogI("Travel", "hold zero-vel skipped (kill-switch) tag=%s",
                         tag ? tag : "-");
        return;
    }
    if (!world::IsPlayReady()) {
        x::runtime::LogI("Travel", "hold zero-vel skipped (not_play_ready) tag=%s",
                         tag ? tag : "-");
        return;
    }
    // 调用约定：先 heli::Disarm，且 fhBan 仍 ON（对齐 Fly::TryDisarmZeroImpactVel）。
    TryClearImpactZero(tag, "hold");
}

bool MapIdChangedFrom(int expectMapId) {
    if (expectMapId <= 0 || !TravelMapIdGateEnabled()) return false;
    const int mid = world::GetMapId();
    return mid > 0 && mid != expectMapId;
}

struct UpUntilCtx {
    int expectMapId = 0;
    bool transit = false;
    bool mapChanged = false;
};

bool UpHoldUntilFn(void* user) {
    auto* ctx = static_cast<UpUntilCtx*>(user);
    if (!ctx) return true;
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        ctx->transit = true;
        return true;
    }
    if (MapIdChangedFrom(ctx->expectMapId)) {
        ctx->mapChanged = true;
        return true;
    }
    return false;
}

// 必须在 worker 调用（禁止嵌在 FireJobOnMain 里）。
// HoldUntil：最短 kPortalUpHoldMs，最长 kPortalUpHoldMaxMs；MapId/transit 结束。
// until 早于 min / MapId 晚闪：drain + afterUntilDrain（BIN 02:34/02:42）。
bool CallUpKey(char* result, size_t resultCap, int expectMapId = 0) {
    if (!x::runtime::main_thread::Ensure()) {
        snprintf(result, resultCap, "KEY_FAIL");
        return false;
    }
    (void)unity_kbd::EnsureBound();

    UpUntilCtx ctx{};
    ctx.expectMapId = expectMapId;
    char detail[96]{};
    const DWORD postDrain = TravelUpDrainEnabled() ? kPortalUpPostChangeDrainMs : 0;
    const bool ok = unity_kbd::HoldUntil(
        unity_kbd::kKeyUpArrow, kPortalUpHoldMs, kPortalUpHoldMaxMs, &UpHoldUntilFn, &ctx, detail,
        sizeof(detail), postDrain);
    if (!ok) {
        x::runtime::LogW("Travel", "kbd Up HoldUntil fail detail=%s why=%s", detail,
                         unity_kbd::LastFail());
        snprintf(result, resultCap, "KEY_FAIL");
        return false;
    }

    if (ctx.transit || !world::IsInMapScene() || !world::IsPlayReady()) {
        x::runtime::LogI("Travel", "kbd Up transit detail=%s", detail);
        snprintf(result, resultCap, "MAP_TRANSITION");
        return true;
    }
    if (ctx.mapChanged || MapIdChangedFrom(expectMapId)) {
        x::runtime::LogI("Travel", "kbd Up done mapId %d->%d (gate) detail=%s", expectMapId,
                         world::GetMapId(), detail);
        snprintf(result, resultCap, "MAP_CHANGED");
        return true;
    }
    x::runtime::LogI("Travel", "kbd Up pulse min=%ums max=%ums detail=%s (unity_kbd)",
                     (unsigned)kPortalUpHoldMs, (unsigned)kPortalUpHoldMaxMs, detail);
    snprintf(result, resultCap, "FIRED_UP");
    return true;
}

void FireJobOnMain(void* user) {
    auto* job = reinterpret_cast<FireJob*>(user);
    if (!job) return;
    __try {
        // StickUp 在 TryFireEnter 里只 CallUpKey；此处勿再接 StickUp（防嵌套）。
        if (job->mode == FireMode::Up || job->mode == FireMode::StickUp) {
            snprintf(job->result, sizeof(job->result), "USE_KBD_UP");
            job->ok = false;
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

// StickUp / Up：unity_kbd ↑；掠过触发区当拍开火，不要求刹停。
// 不改 teleport_port / fly / combat 的默认 opts 或公共 API。
bool TryFireEnterOnMain(FireMode mode, std::string& outResult) {
    // Up / StickUp：仅 unity_kbd ↑（worker 上 CallUpKey，勿嵌 OnKey）。
    // 禁止 StickUp 直调 CheckMovePortal：贴门后 ~100ms 断线（BIN DirectEnter/CHK 全灭）。
    if (mode == FireMode::Up || mode == FireMode::StickUp) {
        const bool probe = gCaptureOn.load();
        if (probe) {
            ClearLastCap();
            (void)EnsureBound();
            EnsureSendCaptureInstalled();
        }

        if (probe) ClearLastCap();
        const int map0 = world::GetMapId();
        char buf[80]{};
        const bool ok = CallUpKey(buf, sizeof(buf), map0);
        if (!world::IsInMapScene() || !world::IsPlayReady() ||
            std::strcmp(buf, "MAP_TRANSITION") == 0 ||
            std::strcmp(buf, "MAP_CHANGED") == 0 || MapIdChangedFrom(map0)) {
            outResult = (std::strcmp(buf, "MAP_CHANGED") == 0 || MapIdChangedFrom(map0))
                            ? "MAP_CHANGED"
                            : "MAP_TRANSITION";
            x::runtime::LogI("Travel", "stick-up1 done res=%s map0=%d mapNow=%d skipUp2=1",
                             outResult.c_str(), map0, world::GetMapId());
            return true;
        }
        Sleep(80);
        // BIN 02:34 hop2：CallUpKey 标 pulse 后 Sleep 窗内 MapId 已变，旧逻辑未再查
        // 就 stick-up2 skipped → FIRED_STICK_UP（松键后进门 → InterStage 卡死）。
        if (!world::IsInMapScene() || !world::IsPlayReady() || MapIdChangedFrom(map0)) {
            outResult = MapIdChangedFrom(map0) ? "MAP_CHANGED" : "MAP_TRANSITION";
            x::runtime::LogI("Travel",
                             "stick-up1 post-sleep res=%s map0=%d mapNow=%d skipUp2=1",
                             outResult.c_str(), map0, world::GetMapId());
            return true;
        }
        if (probe) LogUpPortalCap(mode == FireMode::StickUp ? "stick-up1" : "up-pulse1");

        if (ok && mode == FireMode::StickUp) {
            if (!TravelUp2Enabled()) {
                x::runtime::LogI("Travel", "stick-up2 skipped (kill-switch) map0=%d", map0);
                outResult = "FIRED_STICK_UP";
                return true;
            }
            if (probe) ClearLastCap();
            Sleep(kPortalUpFollowGapMs);
            if (!world::IsInMapScene() || !world::IsPlayReady() || MapIdChangedFrom(map0)) {
                outResult = MapIdChangedFrom(map0) ? "MAP_CHANGED" : "MAP_TRANSITION";
                x::runtime::LogI("Travel", "stick-up2 aborted res=%s map0=%d mapNow=%d",
                                 outResult.c_str(), map0, world::GetMapId());
                return true;
            }
            char buf2[80]{};
            if (CallUpKey(buf2, sizeof(buf2), map0)) {
                if (!world::IsInMapScene() || !world::IsPlayReady() ||
                    std::strcmp(buf2, "MAP_TRANSITION") == 0 ||
                    std::strcmp(buf2, "MAP_CHANGED") == 0 || MapIdChangedFrom(map0)) {
                    outResult = (std::strcmp(buf2, "MAP_CHANGED") == 0 || MapIdChangedFrom(map0))
                                    ? "MAP_CHANGED"
                                    : "MAP_TRANSITION";
                    x::runtime::LogI("Travel", "stick-up2 mid-transition res=%s map0=%d mapNow=%d",
                                     outResult.c_str(), map0, world::GetMapId());
                    return true;
                }
                x::runtime::LogI("Travel", "kbd Up follow-up (stick cover)");
                Sleep(80);
                if (probe) LogUpPortalCap("stick-up2");
            }
            outResult = "FIRED_STICK_UP";
            return true;
        }

        outResult = buf;
        return ok;
    }

    FireJob job{};
    job.mode = mode;
    job.fieldKey = gWm ? ReadU8(gWm, kOffWmFieldKey) : 0;
    if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, 2500)) {
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

// Impact 贴门：对齐 F5 旋翼「滑翔到站位点再干活」。
//   瞄准 SnapStandForPortal → 全程面板倍率 Cruise → Station（滞后）→ hold → ↑。
// hold 期禁止再 Station（会打掉 CurFh 门前抖）。
bool ImpactStickToPortal(const PortalInfo& portal, FireMode enterMode, std::string& outResult,
                         float* outSx, float* outSy) {
    namespace heli = x::features::simple_combat::heli;

    auto portalStationOk = [](float px, float py, float ax, float ay) {
        return std::fabs(ax - px) <= kPortalStationDx && std::fabs(ay - py) <= kPortalStationDy;
    };
    auto speedLenOk = [](float vx, float vy, float lim) {
        return std::sqrt(vx * vx + vy * vy) <= lim;
    };

    // 瞄准 X = 门心；Snap 只取可站 Y → landY（落地/bleed 真源）。
    // aimY = landY + lift（仅旋翼接近用）。禁止 bleed 对抬高 aim（台下悬空）。
    // AbsPos：更大 Y = 更高；抬高必须 +=，禁止减小 Y。
    float aimX = portal.x;
    float landY = portal.y;
    float aimY = portal.y;
    uint32_t aimFh = 0;
    {
        float sx = portal.x, sy = portal.y;
        uint32_t sfh = 0;
        if (foothold_path::SnapStandForPortal(portal.x, portal.y, portal.rectL, portal.rectT,
                                             portal.rectR, portal.rectB, portal.rectValid, &sx, &sy,
                                             &sfh) &&
            sfh != 0) {
            // Y 须仍在触发框（抬高前）；X 一律 portal.x
            if (!portal.rectValid || PointInPortalRect(portal, portal.x, sy)) {
                if (std::fabs(sx - portal.x) > 0.5f) {
                    x::runtime::LogI("Travel",
                                     "heli stick snapX ignored name=%s portalX=%.0f snap=(%.0f,%.0f) "
                                     "→ aimX=portal",
                                     portal.name.c_str(), portal.x, sx, sy);
                }
                landY = sy;
                aimY = sy;
                aimFh = sfh;
            }
        }
    }
    aimY += kPortalAimLiftY;
    if (portal.rectValid) {
        if (!PointInPortalRect(portal, aimX, aimY)) ClampIntoPortalRect(portal, aimX, aimY);
        // landY 保持台面；若抬高后 aim 被夹，不回写 landY
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

    if (!x::features::invuln::IsEnabled()) {
        outResult = "INVULN_OFF";
        x::runtime::LogW("Travel", "heli stick refuse invuln_off name=%s", portal.name.c_str());
        return false;
    }

    // 贴门速度 = 面板「滑翔速度」（与打怪同 Travel SpeedScale）；黑屏已另修，不再封顶 2X。
    struct TravelStickSpeedGuard {
        float prev = 1.f;
        TravelStickSpeedGuard() {
            prev = heli::SpeedScale(heli::Owner::Travel);
            heli::SetSpeedScale(heli::Owner::Travel, prev);
        }
        ~TravelStickSpeedGuard() { heli::SetSpeedScale(heli::Owner::Travel, prev); }
    } speedGuard;

    // Station 滞后：进 ≤enter（或触发框）锁 approach；回 Cruise 须 dist>enter+slack 且离框。
    bool approachLatched = false;
    auto wantStation = [&](float dist, bool inTrig) -> bool {
        if (inTrig || dist <= kTravelStationEnterR) {
            approachLatched = true;
            return true;
        }
        if (approachLatched && dist <= kTravelStationEnterR + kTravelStationExitSlack)
            return true;
        if (approachLatched) {
            approachLatched = false;
            x::runtime::LogI("Travel",
                             "heli stick mode phase=cruise name=%s dist=%.0f (left approach)",
                             portal.name.c_str(), dist);
        }
        return false;
    };

    {
        const float d0 = std::sqrt((aimX - luX) * (aimX - luX) + (aimY - luY) * (aimY - luY));
        const bool st0 = wantStation(d0, InPortalTrigger(portal));
        x::runtime::LogI("Travel",
                         "heli stick aim name=%s portal=(%.0f,%.0f) aim=(%.0f,%.0f) landY=%.0f "
                         "ap=(%.0f,%.0f) rect=%d aimFh=%u liftY=%.0f speed=%.2fX (panel) mode=%s "
                         "enterR=%.0f exitR=%.0f dist=%.0f",
                         portal.name.c_str(), portal.x, portal.y, aimX, aimY, landY, luX, luY,
                         portal.rectValid ? 1 : 0, aimFh, kPortalAimLiftY, speedGuard.prev,
                         st0 ? "approach" : "cruise", kTravelStationEnterR,
                         kTravelStationEnterR + kTravelStationExitSlack, d0);
        if (st0) {
            x::runtime::LogI("Travel",
                             "heli stick mode phase=approach name=%s dist=%.0f (latched)",
                             portal.name.c_str(), d0);
        }
    }

    struct TravelFhBanGuard {
        bool armed = false;
        bool leaveArmed = false;
        void Arm() {
            leaveArmed = false;
            if (armed) return;
            ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, true);
            armed = true;
            x::runtime::LogI("Travel", "heli stick fhBan=1 mask=0x%x",
                             ports::fly_fh_ban::ActiveMask());
        }
        void DisarmForLand() {
            if (!armed) {
                leaveArmed = true;
                return;
            }
            ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
            armed = false;
            leaveArmed = true;
            x::runtime::LogI("Travel", "heli stick fhBan land mask=0x%x",
                             ports::fly_fh_ban::ActiveMask());
        }
        void LeaveArmedForEnter() { leaveArmed = true; }
        ~TravelFhBanGuard() {
            heli::Disarm(heli::Owner::Travel);
            heli::Release(heli::Owner::Travel);
            if (!armed || leaveArmed) return;
            ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
            x::runtime::LogI("Travel", "heli stick fhBan off mask=0x%x",
                             ports::fly_fh_ban::ActiveMask());
        }
    } fhBan;
    // 先拿旋翼、确认静默闸已放行，再摘台——反过来就是自由落体：
    // soft_login 静默期 heli::Tick 整拍 return false（guard=soft_hold / soft_land_quiet），
    // 而 fhBan.Arm() 的 detach=1 会立刻把人从台上摘下来，没人托就一路掉到底。
    // BIN 2026-08-12 勇士之村 102000000：回城卷换图的 session churn 误触 soft hold，
    // detach 后 ap 从 -1258 垂直掉到 -1588（330px，X 零位移），0.65s 后闸开才横飞贴门。
    // 只等 IsHoldActive：真正整拍闸死 Travel 冲量的只有 soft_hold；land_quiet 那半段在
    // 摘台后 onFh=0，heli::Tick 照常算冲量。BIN 2026-08-13 00:31:10 实测 quiet=1（仅
    // land_quiet）时摘台，300ms 后就正常横飞到位——用 IsGameplayQuiet 会白等满 2000ms。
    (void)heli::TryAcquire(heli::Owner::Travel);
    {
        const DWORD gateT0 = GetTickCount();
        bool waited = false;
        while (x::features::soft_login_probe::IsHoldActive()) {
            if (GetTickCount() - gateT0 >= kDetachQuietWaitMaxMs) break;
            if (!world::IsInMapScene() || !world::IsPlayReady()) break;
            waited = true;
            Sleep(kImpactStickPollMs);
        }
        if (waited) {
            x::runtime::LogI("Travel",
                             "heli stick detach wait hold name=%s waited=%ums hold=%d",
                             portal.name.c_str(),
                             static_cast<unsigned>(GetTickCount() - gateT0),
                             x::features::soft_login_probe::IsHoldActive() ? 1 : 0);
        }
    }
    fhBan.Arm();

    const DWORD t0 = GetTickCount();
    DWORD settleSince = 0;
    DWORD holdSince = 0;
    DWORD readySince = 0;  // onFh∧低速∧Ap静；滑移则整段清零重等
    float readyApX = 0.f, readyApY = 0.f;
    float prevApX = 0.f, prevApY = 0.f;
    bool havePrevAp = false;
    DWORD leftTrigSince = 0;  // hold 中离开触发框的起点；宽限内不 abort
    bool holdPhase = false;   // 已进 hold（卸 ban + 停旋翼）
    bool stoodOnFh = false;   // CurFh 已挂上（日志/掉台提示）
    bool loggedBleedNudge = false;
    const float panelScale = speedGuard.prev;  // 面板滑翔速度（与打怪同）
    bool softApproach = false;  // abort 后重贴标记（速度仍用 panelScale）
    int tickN = 0;
    int fireN = 0;
    int failStreak = 0;
    heli::Telemetry tm{};
    for (;;) {
        const DWORD now = GetTickCount();
        if (!world::IsInMapScene() || !world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            fhBan.LeaveArmedForEnter();
            x::runtime::LogI("Travel", "heli stick map transition name=%s ticks=%d fire=%d",
                             portal.name.c_str(), tickN, fireN);
            return true;
        }
        if (now - t0 >= kImpactStickMaxMs) {
            outResult = "NOT_STOOD";
            float ax = 0.f, ay = 0.f;
            const bool got = ReadLocalAp(ax, ay);
            ports::teleport::FlightState fs{};
            const bool haveFs = ports::teleport::QueryFlightState(fs) && fs.ok;
            x::runtime::LogW("Travel",
                             "heli stick timeout name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) "
                             "onFh=%d v=(%.0f,%.0f) ticks=%d fire=%d failStreak=%d",
                             portal.name.c_str(), got ? ax : 0.f, got ? ay : 0.f, aimX, aimY,
                             haveFs && fs.onFh ? 1 : 0, haveFs ? fs.vx : tm.vx,
                             haveFs ? fs.vy : tm.vy, tickN, fireN, failStreak);
            return false;
        }

        float px = 0.f, py = 0.f;
        if (!ReadLocalAp(px, py)) {
            ++failStreak;
            if (failStreak >= 30) {
                outResult = "IMPACT_STICK_FAIL";
                x::runtime::LogW("Travel", "heli stick no_ap name=%s streak=%d",
                                 portal.name.c_str(), failStreak);
                return false;
            }
            Sleep(kImpactStickPollMs);
            continue;
        }

        const float distAim =
            std::sqrt((aimX - px) * (aimX - px) + (aimY - py) * (aimY - py));
        const bool inTrigNow = InPortalTrigger(portal);
        if (inTrigNow) {
            leftTrigSince = 0;
            if (settleSince == 0) {
                settleSince = now;
                x::runtime::LogI("Travel",
                                 "heli stick settle begin name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) "
                                 "ticks=%d",
                                 portal.name.c_str(), px, py, aimX, aimY, tickN);
            }
        } else if (holdPhase && leftTrigSince == 0) {
            leftTrigSince = now;
        }
        // hold 保海岸：宽限出框，或未站稳但仍贴近 aim（掉出台阶瞬间不立刻 10X 重贴）。
        const bool graceHold = holdPhase && leftTrigSince != 0 &&
                               (now - leftTrigSince) < kPortalHoldLeaveGraceMs;
        const bool nearAimCoast =
            holdPhase && !stoodOnFh && distAim < kPortalHoldAbortDist &&
            std::fabs(px - aimX) <= kPortalHoldNearAimDx &&
            std::fabs(py - aimY) <= kPortalHoldNearAimDy;
        const bool holdInZone = inTrigNow || graceHold || nearAimCoast;

        ports::teleport::FlightState fs{};
        const bool haveFs = ports::teleport::QueryFlightState(fs) && fs.ok;
        const float liveVx = haveFs ? fs.vx : tm.vx;
        const float liveVy = haveFs ? fs.vy : tm.vy;
        const bool onFh = haveFs && fs.onFh;

        // hold 期必须停旋翼：Station/Impact 会把刚挂上的 CurFh 打掉 → 门前抖 10s+（BIN 11:51）。
        // 接近阶段才 Cruise/Station；hold 只靠重力落到 aimFh。
        if (!holdPhase) {
            heli::SetSpeedScale(heli::Owner::Travel, panelScale);
            // 末段贴台面飞：抬高过大 → hold 悬空卸推 → 掉穿（104000100 west00）。
            const bool finalApproach =
                inTrigNow || distAim <= kTravelStationEnterR ||
                std::fabs(px - aimX) <= kTravelStationEnterR;
            const float heliY =
                finalApproach ? (landY + kPortalFinalAimLiftY) : aimY;
            const float dx = aimX - px;
            const float dy = heliY - py;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const bool wasApproach = approachLatched;
            const bool station = wantStation(dist, inTrigNow);
            if (station && !wasApproach) {
                x::runtime::LogI("Travel",
                                 "heli stick mode phase=approach name=%s dist=%.0f%s",
                                 portal.name.c_str(), dist,
                                 softApproach ? " restick=1" : " (latched)");
            }
            heli::Setpoint sp{};
            sp.x = aimX;
            sp.y = heliY;
            sp.mode = station ? heli::Mode::Station : heli::Mode::Cruise;
            heli::SetSetpoint(heli::Owner::Travel, sp);

            tm = {};
            const bool fired = heli::Tick(heli::Owner::Travel, now, &tm);
            ++tickN;
            if (fired) {
                ++fireN;
                failStreak = 0;
            } else if (tm.guard && tm.guard[0] &&
                       std::strcmp(tm.guard, "cadence") != 0 &&
                       std::strcmp(tm.guard, "deadband") != 0) {
                ++failStreak;
                if (failStreak >= 60) {
                    outResult = "IMPACT_STICK_FAIL";
                    x::runtime::LogW(
                        "Travel",
                        "heli stick fail name=%s guard=%s streak=%d ticks=%d mode=%s",
                        portal.name.c_str(), tm.guard ? tm.guard : "?", failStreak, tickN,
                        heli::ModeName(sp.mode));
                    return false;
                }
            } else {
                failStreak = 0;
            }
        }

        if (holdInZone) {
            // hold 入场：水平对门心；竖直贴台面且不得已在台下（AbsPos 更大 Y=更高）。
            const bool stOk = std::fabs(px - aimX) <= kPortalStationDx;
            const bool settleSpdOk =
                speedLenOk(liveVx, liveVy, kPortalSettleSpeed) ||
                (tm.guard && std::strcmp(tm.guard, "deadband") == 0);
            const bool nearDeckY = std::fabs(py - landY) <= kPortalHoldEnterDy;
            // py >= landY - belowMax：禁止 hold@台下再卸推掉穿。
            const bool onOrAboveDeck = py >= (landY - kPortalHoldBelowMax);
            // bleed：竖速 + 横速都收住再 Disarm（禁 hold 硬刹；横速门槛防 10X 甩台）。
            const bool bleedOk = std::fabs(liveVy) <= kPortalHoldEnterVy &&
                                 std::fabs(liveVx) <= kPortalHoldEnterVx && nearDeckY &&
                                 onOrAboveDeck;
            const bool settleTimeout =
                settleSince != 0 && (now - settleSince) >= kPortalSettleMaxMs;
            const bool canHold = inTrigNow && stOk && settleSpdOk && bleedOk;

            if (!holdPhase && canHold) {
                holdPhase = true;
                holdSince = now;
                readySince = 0;
                stoodOnFh = false;
                loggedBleedNudge = false;
                softApproach = false;
                havePrevAp = false;
                heli::SetSpeedScale(heli::Owner::Travel, panelScale);
                heli::Disarm(heli::Owner::Travel);
                TryHoldClearImpact(portal.name.c_str());  // fhBan 仍 ON，对齐 F6
                fhBan.DisarmForLand();
                x::runtime::LogI(
                    "Travel",
                    "heli stick hold begin name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) landY=%.0f "
                    "v=(%.0f,%.0f) onFh=%d st=%d spd=%d bleed=1 holdZero "
                    "ready=onFh+lowV+apStill stable=%ums coast=1",
                    portal.name.c_str(), px, py, aimX, aimY, landY, liveVx, liveVy, onFh ? 1 : 0,
                    stOk ? 1 : 0, settleSpdOk ? 1 : 0, (unsigned)kPortalReadyStableMs);
            } else if (!holdPhase && inTrigNow && settleTimeout && !bleedOk) {
                if (!loggedBleedNudge) {
                    loggedBleedNudge = true;
                    x::runtime::LogI(
                        "Travel",
                        "heli stick bleed wait name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) "
                        "landY=%.0f v=(%.0f,%.0f) need|vx|<=%.0f need|vy|<=%.0f "
                        "|ap.y-landY|<=%.0f aboveDeck(py>=landY-%.0f)=%d (no zero-vel)",
                        portal.name.c_str(), px, py, aimX, aimY, landY, liveVx, liveVy,
                        kPortalHoldEnterVx, kPortalHoldEnterVy, kPortalHoldEnterDy,
                        kPortalHoldBelowMax, onOrAboveDeck ? 1 : 0);
                }
            } else if (!holdPhase && inTrigNow && settleTimeout && bleedOk && stOk) {
                holdPhase = true;
                holdSince = now;
                readySince = 0;
                stoodOnFh = false;
                softApproach = false;
                havePrevAp = false;
                heli::SetSpeedScale(heli::Owner::Travel, panelScale);
                heli::Disarm(heli::Owner::Travel);
                TryHoldClearImpact(portal.name.c_str());
                fhBan.DisarmForLand();
                x::runtime::LogI(
                    "Travel",
                    "heli stick hold begin name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) landY=%.0f "
                    "v=(%.0f,%.0f) onFh=%d st=%d spd=%d settleTo=1 bleed=1 holdZero "
                    "ready=onFh+lowV+apStill stable=%ums coast=1",
                    portal.name.c_str(), px, py, aimX, aimY, landY, liveVx, liveVy, onFh ? 1 : 0,
                    stOk ? 1 : 0, settleSpdOk ? 1 : 0, (unsigned)kPortalReadyStableMs);
            }

            // hold 中掉到台下：立刻重贴，勿等掉到下层 FH 才 left-trigger abort。
            if (holdPhase && !stoodOnFh && py < (landY - kPortalHoldEnterDy)) {
                holdPhase = false;
                leftTrigSince = 0;
                readySince = 0;
                softApproach = true;
                havePrevAp = false;
                approachLatched = true;
                fhBan.Arm();
                x::runtime::LogI(
                    "Travel",
                    "heli stick hold abort (below deck) name=%s ap=(%.0f,%.0f) landY=%.0f "
                    "v=(%.0f,%.0f) onFh=%d speed=%.2fX",
                    portal.name.c_str(), px, py, landY, liveVx, liveVy, onFh ? 1 : 0,
                    panelScale);
            }

            if (holdPhase) {
                // 残留 → 等站稳再进门。停推后靠自然衰减 + AbsPos 静止；不硬写 (0,0)。
                const bool fireSpdOk = speedLenOk(liveVx, liveVy, kPortalFireSpeed);
                const bool onPortalX = std::fabs(px - portal.x) <= kPortalFireMaxDx;
                const bool apStepOk =
                    !havePrevAp || (std::fabs(px - prevApX) <= kPortalReadyApStep &&
                                    std::fabs(py - prevApY) <= kPortalReadyApStep);
                if (inTrigNow && onFh && fireSpdOk && onPortalX && apStepOk) {
                    if (!stoodOnFh) {
                        stoodOnFh = true;
                        x::runtime::LogI(
                            "Travel",
                            "heli stick stood onFh name=%s ap=(%.0f,%.0f) v=(%.0f,%.0f) "
                            "aimFh=%u (waiting Ap settle %ums)",
                            portal.name.c_str(), px, py, liveVx, liveVy, aimFh,
                            (unsigned)kPortalReadyStableMs);
                    }
                    if (readySince == 0) {
                        readySince = now;
                        readyApX = px;
                        readyApY = py;
                    } else if (std::fabs(px - readyApX) > kPortalReadyApDrift ||
                               std::fabs(py - readyApY) > kPortalReadyApDrift) {
                        x::runtime::LogI(
                            "Travel",
                            "heli stick wait settle (ap-drift) name=%s ap=(%.0f,%.0f) "
                            "anchor=(%.0f,%.0f) → re-wait %ums",
                            portal.name.c_str(), px, py, readyApX, readyApY,
                            (unsigned)kPortalReadyStableMs);
                        readySince = 0;  // 整段重等，禁止滑窗凑满
                    }
                } else {
                    if (stoodOnFh && !onFh) {
                        x::runtime::LogI("Travel", "heli stick lost onFh name=%s ap=(%.0f,%.0f)",
                                         portal.name.c_str(), px, py);
                    } else if (stoodOnFh && onFh && !fireSpdOk) {
                        x::runtime::LogI(
                            "Travel",
                            "heli stick wait settle (speed) name=%s ap=(%.0f,%.0f) "
                            "v=(%.0f,%.0f)",
                            portal.name.c_str(), px, py, liveVx, liveVy);
                    } else if (stoodOnFh && onFh && !apStepOk) {
                        x::runtime::LogI(
                            "Travel",
                            "heli stick wait settle (ap-step) name=%s ap=(%.0f,%.0f) "
                            "prev=(%.0f,%.0f)",
                            portal.name.c_str(), px, py, prevApX, prevApY);
                    } else if (stoodOnFh && onFh && !onPortalX) {
                        x::runtime::LogI(
                            "Travel",
                            "heli stick wait settle (portal-x) name=%s ap=(%.0f,%.0f) "
                            "portalX=%.0f maxDx=%.0f",
                            portal.name.c_str(), px, py, portal.x, kPortalFireMaxDx);
                    }
                    stoodOnFh = onFh;
                    readySince = 0;
                }
                prevApX = px;
                prevApY = py;
                havePrevAp = true;

                const bool ready =
                    readySince != 0 && (now - readySince) >= kPortalReadyStableMs;
                const bool landTimeout =
                    !onFh && (now - holdSince) >= kPortalLandTimeoutMs;

                if (landTimeout) {
                    outResult = "NOT_STOOD";
                    x::runtime::LogW(
                        "Travel",
                        "heli stick land timeout name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) "
                        "onFh=0 v=(%.0f,%.0f) waited=%ums",
                        portal.name.c_str(), px, py, aimX, aimY, liveVx, liveVy,
                        (unsigned)(now - holdSince));
                    return false;
                }

                if (ready) {
                    x::runtime::LogI(
                        "Travel",
                        "heli stick pre-fire land name=%s ap=(%.0f,%.0f) onFh=%d "
                        "v=(%.0f,%.0f) ready=%ums wait=%ums",
                        portal.name.c_str(), px, py, onFh ? 1 : 0, liveVx, liveVy,
                        (unsigned)(now - readySince), (unsigned)kPortalPreFireLandMs);
                    const DWORD land0 = GetTickCount();
                    while (GetTickCount() - land0 < kPortalPreFireLandMs) {
                        if (!world::IsInMapScene() || !world::IsPlayReady()) {
                            outResult = "MAP_TRANSITION";
                            fhBan.LeaveArmedForEnter();
                            return true;
                        }
                        Sleep(kImpactStickPollMs);
                    }
                    float fx = px, fy = py;
                    (void)ReadLocalAp(fx, fy);
                    ports::teleport::FlightState fs2{};
                    const bool have2 = ports::teleport::QueryFlightState(fs2) && fs2.ok;
                    const bool apStill =
                        std::fabs(fx - readyApX) <= kPortalReadyApDrift &&
                        std::fabs(fy - readyApY) <= kPortalReadyApDrift;
                    const bool onPortalX2 = std::fabs(fx - portal.x) <= kPortalFireMaxDx;
                    const bool stillReady =
                        have2 && fs2.onFh &&
                        speedLenOk(fs2.vx, fs2.vy, kPortalFireSpeed) &&
                        PointInPortalRect(portal, fx, fy) && apStill && onPortalX2;
                    if (!stillReady) {
                        stoodOnFh = have2 && fs2.onFh;
                        readySince = 0;
                        x::runtime::LogI(
                            "Travel",
                            "heli stick pre-fire not-ready name=%s ap=(%.0f,%.0f) "
                            "onFh=%d v=(%.0f,%.0f) inRect=%d apStill=%d onPortalX=%d",
                            portal.name.c_str(), fx, fy, have2 && fs2.onFh ? 1 : 0,
                            have2 ? fs2.vx : 0.f, have2 ? fs2.vy : 0.f,
                            PointInPortalRect(portal, fx, fy) ? 1 : 0, apStill ? 1 : 0,
                            onPortalX2 ? 1 : 0);
                    } else {
                        x::runtime::LogI(
                            "Travel",
                            "heli stick enter-armed name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) "
                            "v=(%.0f,%.0f) onFh=1 ready=%ums ticks=%d fire=%d mode=%s "
                            "fhBan=0",
                            portal.name.c_str(), fx, fy, aimX, aimY, fs2.vx, fs2.vy,
                            (unsigned)(GetTickCount() - readySince), tickN, fireN,
                            FireModeName(enterMode));
                        TryPreFireClearImpact(portal.name.c_str());
                        if (!world::IsInMapScene() || !world::IsPlayReady()) {
                            outResult = "MAP_TRANSITION";
                            fhBan.LeaveArmedForEnter();
                            return true;
                        }
                        const bool ok = TryFireEnterOnMain(enterMode, outResult);
                        if (ok || outResult == "MAP_TRANSITION" ||
                            outResult == "MAP_CHANGED" ||
                            outResult.rfind("FIRED_", 0) == 0) {
                            fhBan.LeaveArmedForEnter();
                        }
                        return ok;
                    }
                }
            }
        } else if (holdPhase) {
            // 真甩远才 abort；否则会 10X Station 弹飞（BIN 15:58 map 50000 west00）。
            holdPhase = false;
            holdSince = 0;
            readySince = 0;
            leftTrigSince = 0;
            stoodOnFh = false;
            softApproach = true;
            heli::SetSpeedScale(heli::Owner::Travel, panelScale);
            (void)heli::TryAcquire(heli::Owner::Travel);
            if (!fhBan.armed) fhBan.Arm();
            x::runtime::LogI(
                "Travel",
                "heli stick hold abort (left trigger) name=%s ap=(%.0f,%.0f) aim=(%.0f,%.0f) "
                "dist=%.0f v=(%.0f,%.0f) onFh=%d speed=%.2fX",
                portal.name.c_str(), px, py, aimX, aimY, distAim, liveVx, liveVy, onFh ? 1 : 0,
                panelScale);
        }

        Sleep(kImpactStickPollMs);
    }
}

// 已在门内：状态就绪（框内 + onFh + 低速）即可补火；否则短等落地，不再盲等 1.5s。
// 远处：旋翼滑翔 → 进框 Station 收速 → hold 等就绪 → 开火。
bool StickThenEnterReady(const PortalInfo& portal, FireMode enterMode, std::string& outResult) {
    if (AlreadyStoodAtPortal(portal) || InPortalTrigger(portal)) {
        float ax = 0.f, ay = 0.f;
        (void)ReadLocalAp(ax, ay);
        x::runtime::LogI("Travel",
                         "stick already_in wait-ready name=%s ap=(%.0f,%.0f) "
                         "need=onFh+lowV+apStill stable=%ums",
                         portal.name.c_str(), ax, ay, (unsigned)kPortalReadyStableMs);
        const DWORD t0 = GetTickCount();
        DWORD readySince = 0;
        float readyApX = ax, readyApY = ay;
        float prevApX = ax, prevApY = ay;
        bool havePrevAp = false;
        while (GetTickCount() - t0 < kPortalLandTimeoutMs) {
            if (!world::IsInMapScene() || !world::IsPlayReady()) {
                outResult = "MAP_TRANSITION";
                return true;
            }
            if (!InPortalTrigger(portal)) {
                break;
            }
            ports::teleport::FlightState fs{};
            const bool haveFs = ports::teleport::QueryFlightState(fs) && fs.ok;
            const float spd =
                haveFs ? std::sqrt(fs.vx * fs.vx + fs.vy * fs.vy) : 9999.f;
            (void)ReadLocalAp(ax, ay);
            const bool apStepOk =
                !havePrevAp || (std::fabs(ax - prevApX) <= kPortalReadyApStep &&
                                std::fabs(ay - prevApY) <= kPortalReadyApStep);
            const bool readyNow =
                haveFs && fs.onFh && spd <= kPortalFireSpeed && apStepOk;
            const DWORD now = GetTickCount();
            if (readyNow) {
                if (readySince == 0) {
                    readySince = now;
                    readyApX = ax;
                    readyApY = ay;
                } else if (std::fabs(ax - readyApX) > kPortalReadyApDrift ||
                           std::fabs(ay - readyApY) > kPortalReadyApDrift) {
                    x::runtime::LogI(
                        "Travel",
                        "stick already_in wait settle (ap-drift) name=%s ap=(%.0f,%.0f) "
                        "→ re-wait",
                        portal.name.c_str(), ax, ay);
                    readySince = 0;
                }
                if (readySince != 0 && now - readySince >= kPortalReadyStableMs) {
                    x::runtime::LogI("Travel",
                                     "stick already_in ready name=%s ap=(%.0f,%.0f) "
                                     "onFh=1 v=(%.0f,%.0f) waited=%ums",
                                     portal.name.c_str(), ax, ay, haveFs ? fs.vx : 0.f,
                                     haveFs ? fs.vy : 0.f, (unsigned)(now - t0));
                    outResult = "OK";
                    return true;
                }
            } else {
                if (haveFs && fs.onFh && !apStepOk) {
                    x::runtime::LogI(
                        "Travel",
                        "stick already_in wait settle (ap-step) name=%s ap=(%.0f,%.0f)",
                        portal.name.c_str(), ax, ay);
                }
                readySince = 0;
            }
            prevApX = ax;
            prevApY = ay;
            havePrevAp = true;
            Sleep(kImpactStickPollMs);
        }
        if (InPortalTrigger(portal) || AlreadyStoodAtPortal(portal)) {
            // 超时仍未就绪：禁止 blind 补火（易 205）；交给 Impact 卸推/落地再判。
            x::runtime::LogW("Travel",
                             "stick already_in ready-timeout name=%s ap=(%.0f,%.0f) "
                             "→ ImpactStick (no blind fire)",
                             portal.name.c_str(), ax, ay);
            return ImpactStickToPortal(portal, enterMode, outResult, nullptr, nullptr);
        }
        return ImpactStickToPortal(portal, enterMode, outResult, nullptr, nullptr);
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

    // StickUp/Up 补火：与贴门内开火同一条（含 follow-up ↑）。
    if (mode == FireMode::Up || mode == FireMode::StickUp) {
        TryPreFireClearImpact(portalName.c_str());
        if (!world::IsPlayReady()) {
            outResult = "MAP_TRANSITION";
            x::runtime::LogI("Travel", "skip Up after prefire (not play ready) name=%s",
                             portalName.c_str());
            return true;
        }
        return TryFireEnterOnMain(mode, outResult);
    }

    FireJob job{};
    job.mode = mode;
    job.fieldKey = gWm ? ReadU8(gWm, kOffWmFieldKey) : 0;
    strncpy_s(job.name, portalName.c_str(), _TRUNCATE);

    if (!x::runtime::managed_main::Call(&FireJobOnMain, &job, 2500)) {
        outResult = "MAIN_TIMEOUT";
        return false;
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
    if (on) EnsureSendCaptureInstalled();
    x::runtime::LogI("Travel", "capture=%d installed=%d", on ? 1 : 0,
                     gCaptureInstalled.load() ? 1 : 0);
}

bool IsCaptureEnabled() { return gCaptureOn.load(); }

}  // namespace x::features::ports::travel
