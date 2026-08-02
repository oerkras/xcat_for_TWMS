// Classic TWMS travel_port — PortalManager / MapData / 瞬移贴门 / 直调进门 / 调试旁路。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "travel_port.h"

#include "teleport_port.h"
#include "world_port.h"
#include "foothold_path.h"
#include "foothold_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/anchor_lamps.h"
#include "input_port.h"

#include <Windows.h>

#include <atomic>
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
    "d8c5f76b17e494bace6df11330b677adfd574d1725d1222f4a44a25debd2cbe";  // remounted 2026-08-03
// WM / UserLocal / NM → il2cpp_shape Resolve*Klass（hash + shape 兜底）
constexpr char kOutPacketClass[] =
    "aeb7167893ac51cbc0cf730326f2361e6e8b797eeb940786711185ef0fd658c";
// 方法哈希（dump.cs · remount 2026-08-03）— void()/EncodeStr 等同形多，哈希主路径
constexpr char kHashCheckMovePortal[] =
    "a80fdd33b00f2554987b343c79cc26afba88d1d26717077ebb10444fab9dab4";
constexpr char kHashOutCreate[] =
    "b6adaa6d2c3b8b8020394ea48b152c880d4d45c40ab7d55b3d55760b92adb2b";
constexpr char kHashEncode1[] =
    "ae50bba021b8c6d0006db2dc7a3cfd8067beee32939ec92ecce5ce97a025b7d";
constexpr char kHashEncodeStr[] =
    "c0b982c81a1c67a8e527b17eee1eaa1569d2e691b4b9f1020261f1e7828f9e5";
constexpr char kHashSendPacket[] =
    "bcd0d90687418e2b3ff0faf5b96a9bb4028720a4eb37b78b74b873ce1dd891f";

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaCompGetGo = 0x4E47E00;  // remapped 2026-08-03
constexpr uint32_t kRvaObjGetName = 0x4E54D60;  // remapped 2026-08-03
constexpr uint32_t kRvaTfSetPos = 0x4E628B0;  // remapped 2026-08-03
// WorldManager.CheckMovePortal（夹在 SetMyUser / InitCamera 之间）
constexpr uint32_t kRvaCheckMovePortal = 0xDCB4A0;  // remapped 2026-08-03
// OutPacket / NetworkManager.Send（C→S）
constexpr uint32_t kRvaOutPacketCreate = 0x1CB7BB0;  // remapped 2026-08-03
constexpr uint32_t kRvaOutPacketEncode1Byte = 0x1CC4040;  // remapped 2026-08-03
constexpr uint32_t kRvaOutPacketEncodeStr = 0x1CC48C0;  // remapped 2026-08-03
constexpr uint32_t kRvaNmSend = 0x1CB98B0;  // remapped 2026-08-03
// CMS ClientPacket.UserPortalTeleportRequest = 114 → e59 wire 0x0072
constexpr int kClientPortalTeleport = 114;
constexpr uint16_t kWirePortalTeleport = 0x0072;

constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kOffPmPortalList = 0x10;
constexpr size_t kOffPortalData = 0x10;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;

// MapPortalData (TW dump.cs.restored)
constexpr size_t kOffMpdId = 0x10;
constexpr size_t kOffMpdType = 0x14;
constexpr size_t kOffMpdEnable = 0x18;
constexpr size_t kOffMpdPName = 0x20;
constexpr size_t kOffMpdX = 0x28;
constexpr size_t kOffMpdY = 0x2C;
constexpr size_t kOffMpdToMapId = 0x30;

constexpr size_t kOffWmMapData = 0x88;
constexpr size_t kOffMapId = 0x10;
constexpr size_t kOffMapPortals = 0x40;  // MapData.Portals List<MapPortalData>
constexpr size_t kOffWmMyUser = 0x28;  // WorldManager.MyUser（titlebar 铁证）
constexpr size_t kOffWmFieldKey = 0x80;

constexpr size_t kOffPacketBuffer = 0x10;  // Packet.Buffer byte[]
constexpr size_t kOffPacketOffset = 0x18;  // write cursor
constexpr size_t kOffOutPacketId = 0x20;

constexpr size_t kOffUserVecCtrl = 0x50;
constexpr size_t kOffVcApX = 0x98;
constexpr size_t kOffVcApY = 0xA0;
constexpr size_t kOffVcAplX = 0xB8;
constexpr size_t kOffVcAplY = 0xC0;

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
FnStrNew gStrNew = nullptr;

void* gPmKlass = nullptr;
void* gPmType = nullptr;
void* gPm = nullptr;
void* gWmType = nullptr;
void* gWm = nullptr;
void* gWmKlass = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gNmKlass = nullptr;      // Session（Send MethodInfo）
void* gNmType = nullptr;       // legacy
void* gNm = nullptr;           // Session*
void* gFacadeKlass = nullptr;
void* gFacadeType = nullptr;
void* gOutPacketKlass = nullptr;
constexpr size_t kOffNmSession = 0x10;  // Facade → Session*
DWORD gLastRebindMs = 0;

// 产品默认：瞬移落点 + 直调 CheckMovePortal（最快）
std::atomic<int> gFireMode{static_cast<int>(FireMode::DirectEnter)};
// 瞬移后等 Ap；进门前 WaitStood（几何贴近台面）。禁止 fh=0 悬空——BIN 会穿台直坠。
constexpr DWORD kTeleportSettleMs = 80;
constexpr DWORD kStandPollMs = 40;
constexpr DWORD kStandWaitMaxMs = 400;
constexpr float kStandYTol = 12.f;
constexpr float kStickNearR = 72.f;
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
    if (!klass || !gClassGetMethods || !gGA) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(gGA) + rva;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = gClassGetMethods(klass, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            if (reinterpret_cast<uintptr_t>(mi->methodPointer) == want) return mi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
    }
    if (mi && mi->methodPointer) return mi;
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

// 明文名 → 方法哈希 → RVA+kind（paramKlass 可钉死 OutPacket）。
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plainName = nullptr, const char* hashName = nullptr) {
    if (plainName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plainName, shape.arity)) return mi;
    }
    if (hashName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hashName, shape.arity)) return mi;
    }
    if (!klass) return FindMethodByRva(klass, rva);
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("Travel", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plainName ? plainName : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    *outOrig = mi->methodPointer;
    mi->methodPointer = hook;
    // 只换 methodPointer：保留 virtualMethodPointer=原生 RVA，供 shop 等端口 FindMethodByRva 仍能命中。
    // NetworkManager.Send 非虚调用走 methodPointer→Hook；sibling 用 virtual 对 RVA 反查 MI。
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
        const uint8_t* data = reinterpret_cast<uint8_t*>(buf) + 0x20;
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
                     (unsigned)id, off, hex, off > 96 ? "…" : "");
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
        if (gRuntimeClassInit) gRuntimeClassInit(klass);
        void* statics = gClassStaticData(klass);
        if (!statics && gClassParent) {
            void* parent = gClassParent(klass);
            if (parent) {
                if (gRuntimeClassInit) gRuntimeClassInit(parent);
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
    if (gGA && gFindAll) return true;
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
    gSetPos = x::runtime::il2cpp::AtRva<FnSetPos>(kRvaTfSetPos);
    return gFindAll && gClassGetMethods;
}

bool RebindManagers(DWORD now) {
    // 与 SSOT 对齐：早退前刷新 gWm，死缓存不白等 2s
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
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18))
                          : 0;
        for (int i = 0; i < n && i < 8; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
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
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18))
                          : 0;
        for (int i = 0; i < n && i < 32; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
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
                                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18))
                          : 0;
        for (int i = 0; i < n && i < 4; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
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
    using x::runtime::il2cpp_method::TypeKind;

    if (gWmKlass && !gMiCheckMove) {
        // void() 全局不唯一 → 哈希主；kind 仅软验。
        constexpr MethodShape kChk{0, TypeKind::Void, false, true, {}};
        gMiCheckMove = ResolveMi(gWmKlass, kRvaCheckMovePortal, kChk, "CheckMovePortal",
                                 kHashCheckMovePortal);
    }
    if (gOutPacketKlass) {
        // static OutPacket Create(enum) — dump 上 Create 形唯一。
        constexpr MethodShape kCreate{1, TypeKind::Ptr, true, false, {TypeKind::Any}};
        if (!gMiOutCreate)
            gMiOutCreate = ResolveMi(gOutPacketKlass, kRvaOutPacketCreate, kCreate, "Create",
                                     kHashOutCreate);
        // Encode1(sbyte) dump 唯一；EncodeStr(string) 同形多 → 哈希主。
        constexpr MethodShape kEnc{1, TypeKind::Void, true, false, {TypeKind::Any}};
        if (!gMiEncode1)
            gMiEncode1 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode1Byte, kEnc, nullptr,
                                   kHashEncode1);
        constexpr MethodShape kEncStr{1, TypeKind::Void, false, false, {TypeKind::Ptr}};
        if (!gMiEncodeStr)
            gMiEncodeStr = ResolveMi(gOutPacketKlass, kRvaOutPacketEncodeStr, kEncStr, "EncodeStr",
                                     kHashEncodeStr);
    }
    if (gNmKlass && !gCaptureInstalled.load()) {
        MethodShape kSend{};
        kSend.arity = 1;
        kSend.ret = TypeKind::Bool;
        kSend.unique = true;
        kSend.walkParents = true;
        kSend.param[0] = TypeKind::Ptr;
        if (gOutPacketKlass) kSend.paramKlass[0] = gOutPacketKlass;
        gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "SendPacket", kHashSendPacket);
        if (!gMiSend) gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "Send", kHashSendPacket);
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

    return LooksLikeHeapPtr(gPm) && LooksLikeHeapPtr(gWm);
}

std::string PadMapKey(int mapId) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%09d", mapId);
    return buf;
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
                // Transform often @ Component+0x10 → wait, Unity Component has m_CachedPtr;
                // LocalUser may hold Transform separately. Skip if unclear — AbsPos is enough for
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

int CurrentMapId() {
    // 优先 world_port SSOT（与 foothold/mobscan 同链）；travel 自绑 WM 可能短暂无 MapData。
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

// list 元素是 Portal*（再取 Data@0x10）或直接 MapPortalData*。
bool EnumPortalList(void* list, const std::string& mapKey, bool elementsArePortalObj,
                    std::vector<PortalInfo>& out) {
    if (!LooksLikeHeapPtr(list)) return false;
    const int n = ReadI32(list, kOffListSize);
    void* items = ReadPtr(list, kOffListItems);
    if (!LooksLikeHeapPtr(items) || n <= 0 || n > 512) return false;

    for (int i = 0; i < n; ++i) {
        void* elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + 0x20 +
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

    // 1) PortalManager._portalList（运行时 Portal 包装）
    if (EnsureBound() && gPm) {
        void* list = ReadPtr(gPm, kOffPmPortalList);
        if (EnumPortalList(list, outMapKey, /*elementsArePortalObj=*/true, out)) return true;
    }

    // 2) 兜底：MapData.Portals（与 foothold 同 SSOT；PM 空列表/绑错时仍可用）
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
        // TeleportStick / DirectEnter：落点已由 teleport_port 在 worker 完成；此处只触发进门。
        // Up / CheckMove：调试用 AbsPos 硬钉（不经官方 Doing 同步链）。
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
        void* s = gStrNew(job->name);
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

// teleport_port::TeleportNativeSkillCall 自带 InvokeAndWait——禁止在 FireJobOnMain 内嵌套调用。
bool ReadLocalAp(float& outX, float& outY) {
    if (!LooksLikeHeapPtr(gLocalUser)) {
        // 赶路 worker 可能尚未刷 MyUser；再绑一次
        (void)RebindManagers(GetTickCount());
    }
    if (!LooksLikeHeapPtr(gLocalUser)) return false;
    void* vc = ReadPtr(gLocalUser, kOffUserVecCtrl);
    if (!LooksLikeHeapPtr(vc)) return false;
    outX = static_cast<float>(ReadF64(vc, kOffVcApX));
    outY = static_cast<float>(ReadF64(vc, kOffVcApY));
    return true;
}

// 贴门钉台后：Ap 贴近台面即站稳（fh 已种）。卸图 → MAP_TRANSITION；超时 → NOT_STOOD。
bool WaitStoodNearStand(float standX, float standY, uint32_t expectFh, std::string& outResult) {
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
            // 钉台路径：几何贴台即可；curFh 命中更好（Peek 仍可能 0）
            if (geoOk) {
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
                     "stand wait timeout near=(%.0f,%.0f) expectFh=%u curFh=%u ap=(%.0f,%.0f) gotAp=%d",
                     standX, standY, (unsigned)expectFh, (unsigned)foothold::PeekCurFhId(),
                     gotAp ? ax : 0.f, gotAp ? ay : 0.f, gotAp ? 1 : 0);
    return false;
}

bool TeleportToPortal(float x, float y, std::string& outResult, float* outSx, float* outSy) {
    if (outSx) *outSx = x;
    if (outSy) *outSy = y;
    // 硬门禁：非地图场景 / 未 PlayReady → 禁止瞬移（防卸图 Doing）
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        outResult = "NOT_PLAY_READY";
        return false;
    }
    // BIN 15:57：scene 已 Map 但 MyUser 仍空时 Doing → 黑屏；要求能读到 Ap
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

    // BIN 07:26：fh=0 悬空落点 → ap 穿台直坠（-375→-805）；必须 Snap + 种 CurFh（同战斗贴怪）
    float standX = x, standY = y;
    uint32_t fh = 0;
    if (!ports::foothold_path::SnapStandAt(x, y, &standX, &standY, &fh) || fh == 0) {
        outResult = "FH0_FORBID";
        x::runtime::LogW("Travel", "teleport forbid fh=0 portal=(%.0f,%.0f) snap=(%.0f,%.0f)", x, y,
                         standX, standY);
        return false;
    }
    if (outSx) *outSx = standX;
    if (outSy) *outSy = standY;

    // 赶路连跳：短自冷（≥50ms）；不永久改战斗贴怪冷却（战斗侧每次会再 Set）
    ports::teleport::SetNativeCooldownMs(80);
    if (!ports::teleport::TeleportNativeSkillCall(standX, standY, fh, /*snapStand=*/true)) {
        outResult = "TELEPORT_FAIL";
        return false;
    }
    // 瞬移成功后已卸图：真换图中（贴门撞进门），返回 true + MAP_TRANSITION
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        outResult = "MAP_TRANSITION";
        return true;
    }
    Sleep(kTeleportSettleMs);
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        outResult = "MAP_TRANSITION";
        return true;
    }
    return WaitStoodNearStand(standX, standY, fh, outResult);
}

// 贴门钉台；未站稳可重贴一次。换图中禁止再 Doing。禁止 fh=0 悬空重跳穿台。
bool StickThenEnterReady(float x, float y, const char* portalName, std::string& outResult) {
    const std::string mapBefore = CurrentMapKey();
    float sx = x, sy = y;

    auto acceptStuck = [&]() -> bool {
        const std::string mapAfter = CurrentMapKey();
        if (!mapBefore.empty() && !mapAfter.empty() && mapAfter != mapBefore) {
            outResult = "MAP_CHANGED";
            return true;
        }
        outResult = "STUCK";
        return true;
    };

    if (!TeleportToPortal(x, y, outResult, &sx, &sy)) {
        if (outResult != "NOT_STOOD") return false;
    } else if (outResult == "MAP_TRANSITION") {
        x::runtime::LogI("Travel", "stick → map transition name=%s", portalName ? portalName : "?");
        return true;
    } else {
        return acceptStuck();
    }

    if (!world::IsPlayReady()) {
        outResult = "MAP_TRANSITION";
        return true;
    }
    x::runtime::LogW("Travel", "re-stick name=%s (not_stood)", portalName ? portalName : "?");
    float sx2 = sx, sy2 = sy;
    if (!TeleportToPortal(x, y, outResult, &sx2, &sy2)) return false;
    if (outResult == "MAP_TRANSITION") return true;
    return acceptStuck();
}

bool FirePortalByName(const std::string& portalName, bool warpFirst, std::string& outResult) {
    outResult.clear();
    const FireMode mode = GetFireMode();
    PortalInfo p{};
    if (!FindPortalByName(portalName, p)) {
        // Rpc-far 允许门不在当前枚举（仅按名发包）
        if (!(mode == FireMode::Rpc && !warpFirst)) {
            outResult = "NO_PORTAL";
            return false;
        }
        p.name = portalName;
    }
    // 产品路径：Enable 偶发读脏时仍允许贴门（坐标有效即可）；调试路径保留 DISABLED。
    if (warpFirst && !p.activate && mode != FireMode::Rpc &&
        mode != FireMode::TeleportStick && mode != FireMode::DirectEnter) {
        outResult = "PORTAL_DISABLED";
        return false;
    }

    // 产品路径：worker 先官方瞬移贴门（含击退重贴），再主线程触发进门
    if ((mode == FireMode::TeleportStick || mode == FireMode::DirectEnter) && warpFirst) {
        // 补给开趟 ForceNative 冷却未尽时禁止贴门 Doing（BIN 16:11：战斗连跳后立刻赶路 → 205）
        const uint32_t rem = ports::teleport::NativeCooldownRemainingMs();
        if (rem > 0) {
            outResult = "TELEPORT_COOLDOWN";
            return false;
        }
        if (!StickThenEnterReady(p.x, p.y, portalName.c_str(), outResult)) return false;
        // 贴门已触发换图：禁止再 CheckMove（WM/LU 可能已死）
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
    }

    FireJob job{};
    job.mode = mode;
    // 产品路径已瞬移：主线程不再 AbsPos 硬钉
    job.warpFirst =
        warpFirst && !(mode == FireMode::TeleportStick || mode == FireMode::DirectEnter);
    job.x = p.x;
    job.y = p.y;
    job.fieldKey = gWm ? ReadU8(gWm, kOffWmFieldKey) : 0;
    strncpy_s(job.name, portalName.c_str(), _TRUNCATE);

    // Up 可不强制主线程；其余走 managed_main（GC / MethodInfo）
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
