#include "anti_macro_port.h"

#include "../titlebar/titlebar_win.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_prefab.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace x::features::auto_lie::anti_macro_port {
namespace {

using x::runtime::il2cpp::AtRva;
using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::GaBase;
using x::runtime::il2cpp::ReadPtr;

// Prefab 类哈希（与 payload_status Cache 灯一致）· remount 2026-08-06 dump / 2026-08-08 复核
// Util 无 Prefab 属性（纯工具类）；Text/NonFinite 可走 Prefab 串兜底。
constexpr const char* kUtilClass =
    "ec3b217aea4af2d5989a2428d24c7dd36a7bcaef541c9c40fa4c5eb46d1c0aa";
constexpr const char* kNonFiniteClass =
    "c102f2dcae27c38fdf35899451cc71c06a9d55efb9aaee885b3127c09b30e42";
constexpr const char* kTextCaptchaClass =
    "fe6f28a2457d4914b37d8735f0bed623b33bc35d00af4a5c577418ffd436b1c";
constexpr const char* kTextCaptchaInfoClass =
    "ea26363915361ad5bb38fc8e26cb80b69af622a810c2100332e412ca792040b";
constexpr const char* kPrefabNonFinite = "UIAntiMacroNonFinite";
constexpr const char* kPrefabTextCaptcha = "UIAntiMacroTextCaptcha";

void* ResolveQuizKlass(const char* hashName, const char* prefabName) {
    if (prefabName && prefabName[0]) {
        const auto r = x::runtime::il2cpp_prefab::FindClassCached(hashName, prefabName);
        return r.klass;
    }
    return x::runtime::il2cpp::FindClass("", hashName);
}

constexpr uint32_t kRvaIsOpenAntiMacro = 0x937E00;  // Util.IsOpenAntiMacro
constexpr uint32_t kRvaTextGet = 0x934920;  // TextCaptcha.GetAntiMacro
constexpr uint32_t kRvaTextIsInst = 0x934D10;  // TextCaptcha.IsInstantiated
// 真 OnOk（Rosetta bee55c…）；旧误钉 OnSuccess@0x9378C0 / abd99c…
constexpr uint32_t kRvaTextOnOk = 0x936F00;
constexpr uint32_t kRvaNonGet = 0x927DB0;  // NonFinite.GetAntiMacro
constexpr uint32_t kRvaNonIsInst = 0x928110;  // NonFinite.IsInstantiated
constexpr uint32_t kRvaTryGetWinCursorPos = 0x938290;  // Util.TryGetWinCursorPos（IDB 实钉；旧文档 0x936C30 已废）
// 面板仿射主映射（对照 Artale PanelLocalToDesktop；RVA = runtime dump 包装，plain 名主路径）
constexpr uint32_t kRvaCamGetMain = 0x4DECFC0;           // Camera.get_main（与 fly 同钉）
constexpr uint32_t kRvaCamWorldToScreen = 0x5DEC950;     // Camera.WorldToScreenPoint(Vector3) eye=Mono
constexpr uint32_t kRvaRectGetRect = 0x5E6F510;          // RectTransform.get_rect
constexpr uint32_t kRvaTransformPoint = 0x5E758D0;       // Transform.TransformPoint(Vector3)

// 方法哈希（dump.cs）— static bool()/void() 同形多，哈希主路径
constexpr char kHashIsOpenAntiMacro[] =
    "c902c3aee0cd190d1c0817b48f75f871b6d8bb6957bcc899d7748851a61d2ba";
constexpr char kHashTextGet[] =
    "d687a5691943a91180a147112414f2c75a33efaeece4f5133702840b3816748";
constexpr char kHashTextIsInst[] =
    "a8af1b81967cb3b4cacdb1233b02ddbe9aadc8dccf101190483baccc48f6553";
constexpr char kHashTextOnOk[] =
    "bee55c238b1db01fce8031cff4dfc13e3055ec00e1cdb38d2701831c5d694ff";
constexpr char kHashNonGet[] =
    "ba5e6786ccf2f82fa288006796af373ff40ae8ee33545f3b0049fd77ab9b015";
constexpr char kHashNonIsInst[] =
    "a9592c9ed66c631c5d8ff5373b7a7cb983abb45766575222d23f9f1d51d0205";
constexpr char kHashTryGetWinCursorPos[] =
    "eae4d921e42f8b37b6d334fa8ad82a05fb26c47fa4bd34795aaf865ace27b79";
// Quiz 字段：hash → field_get_offset（dump 常量仅 fallback）
constexpr char kHashTextRawImage[] =
    "f7cca706a82942451055bacbbff2a9fd6686523c29b0ccc549acd056819e7b1";
constexpr char kHashTextInputField[] =
    "e3c776557ef677d5d075fa5c00d1a76d2e6e5aec0c274f3a2e17186302f6d33";
constexpr char kHashNonRawImage[] =
    "c1dce0e2279557f203ea993ab5c2ec59adabcfb4b6fcb72b5629674d765fde9";
constexpr char kHashNonTick[] =
    "bbbd3dd792ce7fecb2f329d0b82c0a059f8be592d1640ebe7be0c061c706e97";
constexpr char kHashNonRawPosList[] =
    "d04c64957129da50394b71468bb35f3c55bc0af45486222f6676183adb9ccc7";  // List<Vector2>
constexpr char kHashNonMousePosList[] =
    "a3cdf17266d326410afb418df59ecbd326b4ab692168f314ee03694d4064933";  // List<Vector2Int>
constexpr char kHashNonIsResultRecv[] =
    "e377d738cd6a9d305856166c2356eb9b809a801ca244ecadf610804966892a6";  // bool _isResultRecv
constexpr char kHashInfoJpegData[] =
    "f6c574d29d885e52644d264341a94ef8b28c31a85140a6ea5bd8b3b1d99a0b4";
constexpr char kHashTickFrame[] =
    "b3b8ddeca1cf2136ce487d0f0af015b91a96d47c16bdf51427bb82a4a3b5561";
constexpr char kNonTickNestedName[] =
    "a8b6a1fb5d8b65034eae86de20e237c6a824e741a674297d0f1d003f998382f";

constexpr size_t kFbTextRawImage = 0xA0;
constexpr size_t kFbTextInputField = 0xB0;
constexpr size_t kFbNonRawImage = 0xA0;
constexpr size_t kFbNonTick = 0xE0;
constexpr size_t kFbNonRawPosList = 0xE8;
constexpr size_t kFbNonMousePosList = 0xF0;
constexpr size_t kFbNonIsResultRecv = 0xF8;
constexpr size_t kFbInfoJpegData = 0x20;
constexpr size_t kFbTickFrame = 0x10;
size_t gOffTextRawImage = kFbTextRawImage;
size_t gOffTextInputField = kFbTextInputField;
size_t gOffNonRawImage = kFbNonRawImage;
size_t gOffNonTick = kFbNonTick;
size_t gOffNonRawPosList = kFbNonRawPosList;
size_t gOffNonMousePosList = kFbNonMousePosList;
size_t gOffNonIsResultRecv = kFbNonIsResultRecv;
size_t gOffInfoJpegData = kFbInfoJpegData;
size_t gOffTickFrame = kFbTickFrame;
#define kOffTextRawImage (gOffTextRawImage)
#define kOffTextInputField (gOffTextInputField)
#define kOffNonRawImage (gOffNonRawImage)
#define kOffNonTick (gOffNonTick)
#define kOffInfoJpegData (gOffInfoJpegData)
#define kOffTickFrame (gOffTickFrame)
bool gQuizFieldOffTried = false;
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffByteArrayData (x::runtime::il2cpp_container::OffArrayData())

// 本文件自带一套私有的 FindMethodByName / ResolveMethod / 字段查找，直接调 il2cpp 导出，
// 不走 runtime/il2cpp_method.cpp。这些导出内部先拿全局递归元数据锁再解引用 klass，而那把
// 锁没有 SEH 清理路径：__except 吞掉异常时展开会跨过解锁代码，锁就永久挂在本线程名下，
// 之后全进程（含 Unity 主线程与加载线程）的元数据查找全部挂死 = 黑屏。
// 2026-08-09 04:46 的现场里泄漏者正是本 TU 的 worker（tid 42852，recursion=3）。
// 没漏时下面这个调用是廉价空操作。
void ReturnLeakedMetadataLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
}

bool FieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t minOff,
                 size_t maxOff) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("classGetFieldFromName");
        field = nullptr;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 屏障对没持锁的情况是空操作，所以宁可每个 __except 都挂上：漏一处就可能是一次
        // 永久黑屏，而多挂一处的代价是零。
        ReturnLeakedMetadataLock("antiMacro/fieldGetOffset");
        return false;
    }
    if (off < minOff || off >= maxOff) return false;
    *out = off;
    return true;
}

using FnClassGetNestedTypes = void* (*)(void* klass, void** iter);

void* FindNonTickKlass(void* nonKlass) {
    if (!nonKlass) return nullptr;
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (ga) {
        auto nested = reinterpret_cast<FnClassGetNestedTypes>(
            GetProcAddress(ga, "il2cpp_class_get_nested_types"));
        const auto& e = x::runtime::il2cpp::Get();
        if (nested && e.classGetFieldFromName) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* nk = nested(nonKlass, &iter);
                    if (!nk) break;
                    void* f = e.classGetFieldFromName(nk, kHashTickFrame);
                    if (f) return nk;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ReturnLeakedMetadataLock("classGetNestedTypes");
            }
        }
    }
    return x::runtime::il2cpp::FindClass("", kNonTickNestedName);
}

void EnsureQuizFieldOff() {
    if (gQuizFieldOffTried) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    gQuizFieldOffTried = true;
    void* text = ResolveQuizKlass(kTextCaptchaClass, kPrefabTextCaptcha);
    void* non = ResolveQuizKlass(kNonFiniteClass, kPrefabNonFinite);
    void* info = x::runtime::il2cpp::FindClass("", kTextCaptchaInfoClass);
    void* tick = FindNonTickKlass(non);
    int hits = 0;
    if (FieldOffHit(text, kHashTextRawImage, kFbTextRawImage, &gOffTextRawImage, 0x80, 0x200))
        ++hits;
    if (FieldOffHit(text, kHashTextInputField, kFbTextInputField, &gOffTextInputField, 0x80, 0x200))
        ++hits;
    if (FieldOffHit(non, kHashNonRawImage, kFbNonRawImage, &gOffNonRawImage, 0x80, 0x200)) ++hits;
    if (FieldOffHit(non, kHashNonTick, kFbNonTick, &gOffNonTick, 0x80, 0x200)) ++hits;
    if (FieldOffHit(non, kHashNonRawPosList, kFbNonRawPosList, &gOffNonRawPosList, 0x80, 0x200))
        ++hits;
    if (FieldOffHit(non, kHashNonMousePosList, kFbNonMousePosList, &gOffNonMousePosList, 0x80,
                    0x200))
        ++hits;
    if (FieldOffHit(non, kHashNonIsResultRecv, kFbNonIsResultRecv, &gOffNonIsResultRecv, 0x80,
                    0x200))
        ++hits;
    if (FieldOffHit(info, kHashInfoJpegData, kFbInfoJpegData, &gOffInfoJpegData, 0x10, 0x80))
        ++hits;
    if (FieldOffHit(tick, kHashTickFrame, kFbTickFrame, &gOffTickFrame, 0x10, 0x40)) ++hits;
    x::runtime::LogI("AutoLiePort",
                     "quiz fields path=%s hits=%d/9 txtRaw=0x%zX in=0x%zX nonRaw=0x%zX tick=0x%zX "
                     "rawPos=0x%zX mouse=0x%zX recv=0x%zX jpeg=0x%zX frame=0x%zX",
                     hits == 9 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                     gOffTextRawImage, gOffTextInputField, gOffNonRawImage, gOffNonTick,
                     gOffNonRawPosList, gOffNonMousePosList, gOffNonIsResultRecv, gOffInfoJpegData,
                     gOffTickFrame);
}

using FnIsOpen = bool (*)(const void* methodInfo);
using FnGetObj = void* (*)(const void* methodInfo);
using FnIsInst = bool (*)(const void* methodInfo);
using FnVoidSelf = void (*)(void* self, const void* methodInfo);
using FnSetText = void (*)(void* self, void* str, const void* methodInfo);
using FnGetTransform = void* (*)(void* self, const void* methodInfo);
using FnGetTexture = void* (*)(void* self, const void* methodInfo);
// TW Unity：仅有 EncodeToPNG(Texture2D) —— 无 EncodeToJPG。
using FnEncodePng = void* (*)(void* tex, const void* methodInfo);
// BIN（RVA 0x938290 序言）：rcx=RectTransform*, rdx=Vector2(8B by-value), r8=out*, r9=MethodInfo*
// 误写成 (float,float) 会把 out* 挤到 R9、RDX/R8 成垃圾 → MapBatch 全点 ok=0（E175 真题）。
static_assert(sizeof(Vec2) == 8, "Vec2 must be 8B for IL2CPP Vector2-by-value ABI");
// IDA（GameAssembly runtime dump）：TryGetWinCursorPos @ RVA 0x938290
//   prologue: rdi=rcx(RectTransform*), rbx=rdx(Vector2 by-value), rsi=r8(Vector2* out)
//   返回 bool；写出 Windows 屏幕坐标 float（内含 Y 翻转），与 SetCursorPos 同空间。
//   MethodShape 第 2 参必须是 Any（valuetype Vector2），禁止写成 Ptr / 拆 float×2。
using FnTryWinCursor = bool (*)(void* rectTf, Vec2 localPos, Vec2* out, const void* methodInfo);

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};
struct UnityRect {
    float x = 0.f, y = 0.f, width = 0.f, height = 0.f;
};
struct PointD {
    double x = 0.0, y = 0.0;
};
struct PanelGeometry {
    UnityRect rect{};
    Vec2 offset{};  // TWMS NonFinite 暂无 CursorEnv；固定 0
    POINT corners[4]{};
    PointD cornersF[4]{};
    RECT bounds{};
};

// Unity valuetype 隐式返回指针 ABI（与 fly get_position / ScreenToWorld 同款）
using FnCamMain = void* (*)(const void* methodInfo);
using FnGetRect = UnityRect* (*)(UnityRect* ret, void* self, const void* methodInfo);
using FnTransformPoint = Vec3* (*)(Vec3* ret, void* self, const Vec3* local, const void* methodInfo);
using FnWorldToScreen = Vec3* (*)(Vec3* ret, void* cam, const Vec3* world);

std::mutex gMtx;
bool gReady = false;
void* gMiSetText = nullptr;
void* gMiGetTransform = nullptr;
void* gMiGetTexture = nullptr;
void* gMiEncodePng = nullptr;
void* gMiIsOpen = nullptr;
void* gMiTextGet = nullptr;
void* gMiTextIsInst = nullptr;
void* gMiTextOnOk = nullptr;
void* gMiNonGet = nullptr;
void* gMiNonIsInst = nullptr;
void* gMiTryWinCursor = nullptr;
void* gMiCamMain = nullptr;
void* gMiGetRect = nullptr;
void* gMiTransformPoint = nullptr;
void* gMiWorldToScreen = nullptr;
bool gPanelMapBound = false;
bool gPanelMapTried = false;

void Log(const char* fmt, ...) {
    char buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    x::runtime::LogI("AutoLiePort", "%s", buf);
}

void* MethodPtr(void* methodInfo) {
    if (!methodInfo) return nullptr;
    __try {
        return *reinterpret_cast<void**>(methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    auto* mi = reinterpret_cast<MethodInfoHead*>(
        x::runtime::il2cpp_method::FindMethodByRva(klass, rva, true));
    return (mi && mi->methodPointer) ? mi : nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("antiMacro/classGetMethodFromName");
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
            ReturnLeakedMetadataLock("antiMacro/classGetMethods");
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("antiMacro/classParent");
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plainName, const char* hashName,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plainName, hashName);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

template <typename Fn>
Fn FnFromMi(void* mi, uint32_t rva) {
    if (void* p = MethodPtr(mi)) return reinterpret_cast<Fn>(p);
    return AtRva<Fn>(rva);
}

void EnsureGameMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    void* util = x::runtime::il2cpp::FindClass("", kUtilClass);
    void* text = ResolveQuizKlass(kTextCaptchaClass, kPrefabTextCaptcha);
    void* non = ResolveQuizKlass(kNonFiniteClass, kPrefabNonFinite);

    int hashHits = 0;
    auto fill = [&](void*& slot, void* klass, uint32_t rva, const MethodShape& shape,
                    const char* plain, const char* hash) {
        if (slot || !klass) return;
        ResolvePath path = ResolvePath::Miss;
        slot = ResolveMi(klass, rva, shape, plain, hash, &path);
        if (slot && path == ResolvePath::Hash) ++hashHits;
    };

    if (util) {
        constexpr MethodShape kOpen{0, TypeKind::Bool, false, true, {}};
        fill(gMiIsOpen, util, kRvaIsOpenAntiMacro, kOpen, "IsOpenAntiMacro",
             kHashIsOpenAntiMacro);
        // arity=3 static：RectTransform* / Vector2(by-value→Any) / Vector2* out
        constexpr MethodShape kCur{3, TypeKind::Bool, true, true,
                                   {TypeKind::Ptr, TypeKind::Any, TypeKind::Ptr}};
        fill(gMiTryWinCursor, util, kRvaTryGetWinCursorPos, kCur, "TryGetWinCursorPos",
             kHashTryGetWinCursorPos);
    }
    if (text) {
        constexpr MethodShape kGet{0, TypeKind::Ptr, false, true, {}};
        fill(gMiTextGet, text, kRvaTextGet, kGet, "GetAntiMacro", kHashTextGet);
        constexpr MethodShape kInst{0, TypeKind::Bool, false, true, {}};
        fill(gMiTextIsInst, text, kRvaTextIsInst, kInst, "IsInstantiated", kHashTextIsInst);
        constexpr MethodShape kOk{0, TypeKind::Void, false, true, {}};
        fill(gMiTextOnOk, text, kRvaTextOnOk, kOk, "OnOk", kHashTextOnOk);
    }
    if (non) {
        constexpr MethodShape kGet{0, TypeKind::Ptr, false, true, {}};
        fill(gMiNonGet, non, kRvaNonGet, kGet, "GetAntiMacro", kHashNonGet);
        constexpr MethodShape kInst{0, TypeKind::Bool, false, true, {}};
        fill(gMiNonIsInst, non, kRvaNonIsInst, kInst, "IsInstantiated", kHashNonIsInst);
    }

    static bool sLogged = false;
    const int hits = (gMiIsOpen ? 1 : 0) + (gMiTryWinCursor ? 1 : 0) + (gMiTextGet ? 1 : 0) +
                     (gMiTextIsInst ? 1 : 0) + (gMiTextOnOk ? 1 : 0) + (gMiNonGet ? 1 : 0) +
                     (gMiNonIsInst ? 1 : 0);
    if (!sLogged && hits > 0) {
        sLogged = true;
        Log("methods path=%s hits=%d/7 hash=%d open=%d cursor=%d textGet=%d textInst=%d onOk=%d "
            "nonGet=%d nonInst=%d",
            hashHits == 7 ? "meta" : (hashHits ? "meta-partial" : "rva/kind"), hits, hashHits,
            gMiIsOpen ? 1 : 0, gMiTryWinCursor ? 1 : 0, gMiTextGet ? 1 : 0, gMiTextIsInst ? 1 : 0,
            gMiTextOnOk ? 1 : 0, gMiNonGet ? 1 : 0, gMiNonIsInst ? 1 : 0);
    }
}

void* ResolveMethod(void* klass, const char* name, int argc) {
    auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethodFromName || !klass) return nullptr;
    __try {
        return e.classGetMethodFromName(klass, name, argc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/resolveMethod");
        return nullptr;
    }
}

bool ResolveHelpers() {
    if (gMiSetText && gMiGetTransform && gMiGetTexture && gMiEncodePng) return true;
    void* inputField = x::runtime::il2cpp::FindClass("UnityEngine.UI", "InputField");
    if (!inputField) inputField = x::runtime::il2cpp::FindClass("", "UIInputField");
    if (inputField) {
        gMiSetText = ResolveMethod(inputField, "set_text", 1);
        if (!gMiSetText) gMiSetText = ResolveMethod(inputField, "SetText", 1);
    }
    void* comp = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
    if (comp) gMiGetTransform = ResolveMethod(comp, "get_transform", 0);
    void* raw = x::runtime::il2cpp::FindClass("UnityEngine.UI", "RawImage");
    if (raw) gMiGetTexture = ResolveMethod(raw, "get_texture", 0);
    void* enc = x::runtime::il2cpp::FindClass("UnityEngine", "ImageConversion");
    if (enc) {
        gMiEncodePng = ResolveMethod(enc, "EncodeToPNG", 1);
        if (!gMiEncodePng) gMiEncodePng = ResolveMethod(enc, "EncodeToPNG", 0);
    }
    return gMiSetText && gMiGetTransform;
}

bool BindPanelMapApis() {
    if (gPanelMapBound) return true;
    if (gPanelMapTried) return false;
    gPanelMapTried = true;
    if (!x::runtime::il2cpp::Ensure()) return false;

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    void* camKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Camera");
    void* rectKlass = x::runtime::il2cpp::FindClass("UnityEngine", "RectTransform");
    void* tfKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Transform");
    if (!tfKlass) tfKlass = rectKlass;

    if (camKlass) {
        constexpr MethodShape kMain{0, TypeKind::Ptr, true, true, {}};
        if (!gMiCamMain)
            gMiCamMain = ResolveMi(camKlass, kRvaCamGetMain, kMain, "get_main", nullptr);
        // arity=1 Vector3 包装（内部 eye=Mono=2）；避开四参眼位重载（与 fly STW 同口径）
        constexpr MethodShape kWts{1, TypeKind::Any, true, true, {TypeKind::Any}};
        if (!gMiWorldToScreen)
            gMiWorldToScreen =
                ResolveMi(camKlass, kRvaCamWorldToScreen, kWts, "WorldToScreenPoint", nullptr);
    }
    if (rectKlass) {
        constexpr MethodShape kRect{0, TypeKind::Any, true, true, {}};
        if (!gMiGetRect)
            gMiGetRect = ResolveMi(rectKlass, kRvaRectGetRect, kRect, "get_rect", nullptr);
    }
    if (tfKlass) {
        constexpr MethodShape kTp{1, TypeKind::Any, true, true, {TypeKind::Any}};
        if (!gMiTransformPoint)
            gMiTransformPoint =
                ResolveMi(tfKlass, kRvaTransformPoint, kTp, "TransformPoint", nullptr);
    }

    gPanelMapBound = gMiCamMain && gMiWorldToScreen && gMiGetRect && gMiTransformPoint;
    Log("panel-map bind ok=%d camMain=%d wts=%d getRect=%d tp=%d", gPanelMapBound ? 1 : 0,
        gMiCamMain ? 1 : 0, gMiWorldToScreen ? 1 : 0, gMiGetRect ? 1 : 0,
        gMiTransformPoint ? 1 : 0);
    return gPanelMapBound;
}

HWND FindGameHwndLocal() {
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) hwnd = x::features::titlebar::win::FindGameWindow();
    if (hwnd && IsWindow(hwnd)) return hwnd;
    return nullptr;
}

bool PanelLocalToDesktop(void* targetRect, const Vec3& local, HWND hwnd, POINT& desktop,
                         PointD& desktopF) {
    if (!BindPanelMapApis() || !targetRect || !hwnd) return false;
    auto tp = reinterpret_cast<FnTransformPoint>(MethodPtr(gMiTransformPoint));
    auto camMain = reinterpret_cast<FnCamMain>(MethodPtr(gMiCamMain));
    auto wts = reinterpret_cast<FnWorldToScreen>(MethodPtr(gMiWorldToScreen));
    if (!tp || !camMain || !wts) return false;

    Vec3 world{};
    Vec3 screen{};
    void* cam = nullptr;
    __try {
        tp(&world, targetRect, &local, gMiTransformPoint);
        cam = camMain(gMiCamMain);
        if (!cam) return false;
        wts(&screen, cam, &world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z) ||
        !std::isfinite(screen.x) || !std::isfinite(screen.y)) {
        return false;
    }

    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return false;
    const int clientHeight = client.bottom - client.top;
    // Unity 屏 Y 自下而上 → Windows 桌面 Y 自上而下（对照 Artale PanelLocalToDesktop）
    desktopF.x = static_cast<double>(origin.x) + static_cast<double>(screen.x);
    desktopF.y = static_cast<double>(origin.y) + static_cast<double>(clientHeight) -
                 static_cast<double>(screen.y);
    desktop.x = static_cast<long>(std::lround(desktopF.x));
    desktop.y = static_cast<long>(std::lround(desktopF.y));
    return true;
}

bool ResolvePanelGeometry(void* targetRect, HWND hwnd, PanelGeometry& panel) {
    panel = {};
    if (!BindPanelMapApis() || !targetRect || !hwnd) return false;
    auto getRect = reinterpret_cast<FnGetRect>(MethodPtr(gMiGetRect));
    if (!getRect) return false;

    UnityRect rect{};
    __try {
        getRect(&rect, targetRect, gMiGetRect);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.width < 100.f || rect.height < 100.f) {
        return false;
    }
    panel.rect = rect;

    const float x0 = rect.x;
    const float y0 = rect.y;
    const float x1 = rect.x + rect.width;
    const float y1 = rect.y + rect.height;
    const Vec3 localCorners[4] = {
        {x0, y0, 0.f},
        {x1, y0, 0.f},
        {x1, y1, 0.f},
        {x0, y1, 0.f},
    };
    for (int i = 0; i < 4; ++i) {
        if (!PanelLocalToDesktop(targetRect, localCorners[i], hwnd, panel.corners[i],
                                 panel.cornersF[i])) {
            return false;
        }
    }

    double minX = panel.cornersF[0].x, maxX = panel.cornersF[0].x;
    double minY = panel.cornersF[0].y, maxY = panel.cornersF[0].y;
    for (int i = 1; i < 4; ++i) {
        minX = (std::min)(minX, panel.cornersF[i].x);
        minY = (std::min)(minY, panel.cornersF[i].y);
        maxX = (std::max)(maxX, panel.cornersF[i].x);
        maxY = (std::max)(maxY, panel.cornersF[i].y);
    }
    panel.bounds = {static_cast<long>(std::floor(minX)), static_cast<long>(std::floor(minY)),
                    static_cast<long>(std::ceil(maxX)), static_cast<long>(std::ceil(maxY))};
    if (panel.bounds.right - panel.bounds.left < 100 ||
        panel.bounds.bottom - panel.bounds.top < 100) {
        return false;
    }

    const double predictedX = panel.cornersF[0].x + (panel.cornersF[1].x - panel.cornersF[0].x) +
                              (panel.cornersF[3].x - panel.cornersF[0].x);
    const double predictedY = panel.cornersF[0].y + (panel.cornersF[1].y - panel.cornersF[0].y) +
                              (panel.cornersF[3].y - panel.cornersF[0].y);
    if (std::hypot(predictedX - panel.cornersF[2].x, predictedY - panel.cornersF[2].y) > 3.0) {
        return false;
    }
    return true;
}

bool CursorPointToPanelDesktop(const PanelGeometry& panel, float cursorX, float cursorY,
                               POINT& desktop) {
    const double localX =
        static_cast<double>(cursorX) - panel.rect.width * 0.5 - panel.offset.x;
    const double localY =
        panel.rect.height * 0.5 - static_cast<double>(cursorY) - panel.offset.y;
    const double u = (localX - panel.rect.x) / panel.rect.width;
    const double v = (localY - panel.rect.y) / panel.rect.height;
    constexpr double kPanelEpsilon = 0.01;
    if (!std::isfinite(u) || !std::isfinite(v) || u < -kPanelEpsilon || u > 1.0 + kPanelEpsilon ||
        v < -kPanelEpsilon || v > 1.0 + kPanelEpsilon) {
        return false;
    }

    const double dxX = panel.cornersF[1].x - panel.cornersF[0].x;
    const double dxY = panel.cornersF[1].y - panel.cornersF[0].y;
    const double dyX = panel.cornersF[3].x - panel.cornersF[0].x;
    const double dyY = panel.cornersF[3].y - panel.cornersF[0].y;
    desktop.x =
        static_cast<long>(std::lround(panel.cornersF[0].x + u * dxX + v * dyX));
    desktop.y =
        static_cast<long>(std::lround(panel.cornersF[0].y + u * dxY + v * dyY));
    return desktop.x >= panel.bounds.left && desktop.x <= panel.bounds.right &&
           desktop.y >= panel.bounds.top && desktop.y <= panel.bounds.bottom;
}

bool MapBatchByPanelAffine(void* rectTf, const std::vector<Vec2>& cursorLocal,
                           std::vector<POINT>& outScreen, PanelGeometry& panel,
                           float* outVerifyMaxErr) {
    outScreen.clear();
    if (outVerifyMaxErr) *outVerifyMaxErr = -1.f;
    HWND hwnd = FindGameHwndLocal();
    if (!hwnd || !ResolvePanelGeometry(rectTf, hwnd, panel)) return false;

    outScreen.reserve(cursorLocal.size());
    for (const auto& lp : cursorLocal) {
        POINT pt{};
        if (!CursorPointToPanelDesktop(panel, lp.x, lp.y, pt)) return false;
        outScreen.push_back(pt);
    }
    if (outScreen.size() != cursorLocal.size()) return false;

    // TryGet 交叉验证首/中/末（对照 Artale；不挡主路径，只记最大误差）
    auto fn = FnFromMi<FnTryWinCursor>(gMiTryWinCursor, kRvaTryGetWinCursorPos);
    float maxErr = 0.f;
    int verifyN = 0;
    if (fn && !cursorLocal.empty()) {
        const size_t n = cursorLocal.size();
        const size_t idxs[3] = {0, n / 2, n - 1};
        for (size_t k = 0; k < 3; ++k) {
            const size_t i = idxs[k];
            Vec2 out{};
            bool hit = false;
            __try {
                hit = fn(rectTf, cursorLocal[i], &out, gMiTryWinCursor);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                hit = false;
            }
            if (!hit) continue;
            const float dx = out.x - static_cast<float>(outScreen[i].x);
            const float dy = out.y - static_cast<float>(outScreen[i].y);
            const float err = std::sqrt(dx * dx + dy * dy);
            if (err > maxErr) maxErr = err;
            ++verifyN;
        }
    }
    if (outVerifyMaxErr && verifyN > 0) *outVerifyMaxErr = maxErr;
    return true;
}

bool MapBatchByTryGet(void* rectTf, const std::vector<Vec2>& cursorLocal,
                      std::vector<POINT>& outScreen, int* outOkN, int* outSehN) {
    outScreen.clear();
    auto fn = FnFromMi<FnTryWinCursor>(gMiTryWinCursor, kRvaTryGetWinCursorPos);
    if (!fn) return false;
    outScreen.reserve(cursorLocal.size());
    int okN = 0;
    int sehN = 0;
    for (const auto& lp : cursorLocal) {
        Vec2 out{};
        bool hit = false;
        __try {
            hit = fn(rectTf, lp, &out, gMiTryWinCursor);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hit = false;
            ++sehN;
        }
        POINT pt{};
        if (hit) {
            pt.x = static_cast<LONG>(out.x + 0.5f);
            pt.y = static_cast<LONG>(out.y + 0.5f);
            ++okN;
        }
        outScreen.push_back(pt);
    }
    if (outOkN) *outOkN = okN;
    if (outSehN) *outSehN = sehN;
    return okN > 0 && !outScreen.empty();
}

bool CopyByteArray(void* arr, std::vector<uint8_t>& out) {
    out.clear();
    const uintptr_t n = ArrayLen(arr);
    if (!arr || n == 0 || n > 8 * 1024 * 1024) return false;
    __try {
        const uint8_t* data = reinterpret_cast<uint8_t*>(arr) + kOffByteArrayData;
        out.assign(data, data + n);
        return !out.empty();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
        return false;
    }
}

CaptchaImageKind DetectKind(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        return CaptchaImageKind::Jpeg;
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E &&
        bytes[3] == 0x47)
        return CaptchaImageKind::Png;
    return CaptchaImageKind::Unknown;
}

int ListSize(void* list) {
    if (!list) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(list) + kOffListSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* ListItems(void* list) {
    return ReadPtr(list, kOffListItems);
}

bool ReadJpegFromInfoObject(void* info, std::vector<uint8_t>& out) {
    if (!info) return false;
    EnsureQuizFieldOff();
    void* jpeg = ReadPtr(info, kOffInfoJpegData);
    if (!CopyByteArray(jpeg, out)) return false;
    return DetectKind(out) == CaptchaImageKind::Jpeg || !out.empty();
}

// Soft：FindAll(AntiMacroTextCaptchaInfo) —— Info 若仍被 GC 根住则可得真 jpegData。
bool TryDumpJpegFromLiveInfo(std::vector<uint8_t>& out) {
    out.clear();
    auto& e = x::runtime::il2cpp::Get();
    if (!e.findAll) return false;
    void* klass = x::runtime::il2cpp::FindClass("", kTextCaptchaInfoClass);
    if (!klass) return false;
    // 已在 Unity 主线程 Job 内：直接 TypeGetObject，勿再嵌套 pump。
    void* typeObj = x::runtime::il2cpp::ClassTypeObjectOnMain(klass);
    if (!typeObj) return false;
    void* arr = nullptr;
    __try {
        arr = e.findAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    for (uintptr_t i = 0; i < n && i < 32; ++i) {
        void* info = ArrayAt(arr, i);
        if (!info) continue;
        if (ReadJpegFromInfoObject(info, out) && DetectKind(out) == CaptchaImageKind::Jpeg) {
            Log("jpegData from live Info ok size=%zu", out.size());
            return true;
        }
    }
    out.clear();
    return false;
}

bool EncodeRawImagePng(void* captcha, std::vector<uint8_t>& out) {
    out.clear();
    if (!captcha || !gMiGetTexture || !gMiEncodePng) return false;
    void* rawImage = ReadPtr(captcha, kOffTextRawImage);
    if (!rawImage) return false;
    auto getTex = reinterpret_cast<FnGetTexture>(MethodPtr(gMiGetTexture));
    auto enc = reinterpret_cast<FnEncodePng>(MethodPtr(gMiEncodePng));
    if (!getTex || !enc) return false;
    void* tex = nullptr;
    void* pngArr = nullptr;
    __try {
        tex = getTex(rawImage, gMiGetTexture);
        if (!tex) return false;
        pngArr = enc(tex, gMiEncodePng);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!CopyByteArray(pngArr, out)) return false;
    if (DetectKind(out) != CaptchaImageKind::Png) {
        // 仍可能是有效图；若魔数不对则拒绝，避免泵喂坏文件
        if (out.size() < 32) {
            out.clear();
            return false;
        }
    }
    Log("EncodeToPNG ok size=%zu", out.size());
    return true;
}

struct MainCtx {
    enum class Op : uint8_t {
        DumpImage,
        Submit,
        MapCursor,
        MapBatch,
    } op = Op::DumpImage;
    bool ok = false;
    std::vector<uint8_t>* image = nullptr;
    CaptchaImageKind* kind = nullptr;
    std::string answer;
    void* rectTf = nullptr;
    float lx = 0.f;
    float ly = 0.f;
    POINT screen{};
    const std::vector<Vec2>* localPts = nullptr;
    std::vector<POINT>* screenPts = nullptr;
    POINT panelCorners[4]{};
    bool havePanelCorners = false;
    const char* mapMode = "none";
    float verifyMaxErr = -1.f;
};

void MainJob(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("anti_macro.Main");
    auto* ctx = reinterpret_cast<MainCtx*>(user);
    if (!ctx) return;
    ResolveHelpers();
    if (ctx->op == MainCtx::Op::DumpImage) {
        if (!ctx->image || !ctx->kind) return;
        *ctx->kind = CaptchaImageKind::Unknown;
        ctx->image->clear();

        // 1) 真源：封包 jpegData（若 Info 仍活）
        if (TryDumpJpegFromLiveInfo(*ctx->image)) {
            *ctx->kind = CaptchaImageKind::Jpeg;
            ctx->ok = true;
            return;
        }

        // 2) 兜底：本仓 Unity 无 JPG API → EncodeToPNG
        void* captcha = nullptr;
        __try {
            captcha = FnFromMi<FnGetObj>(gMiTextGet, kRvaTextGet)(gMiTextGet);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            captcha = nullptr;
        }
        if (!captcha) {
            Log("dump fail: no TextCaptcha instance");
            return;
        }
        if (!gMiEncodePng) {
            Log("dump fail: EncodeToPNG MethodInfo missing");
            return;
        }
        if (EncodeRawImagePng(captcha, *ctx->image) && !ctx->image->empty()) {
            *ctx->kind = CaptchaImageKind::Png;
            ctx->ok = true;
            return;
        }
        Log("dump fail: EncodeToPNG empty/exception");
        ctx->ok = false;
        return;
    }
    if (ctx->op == MainCtx::Op::Submit) {
        void* captcha = nullptr;
        __try {
            captcha = FnFromMi<FnGetObj>(gMiTextGet, kRvaTextGet)(gMiTextGet);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            captcha = nullptr;
        }
        if (!captcha) return;
        void* input = ReadPtr(captcha, kOffTextInputField);
        if (!input || !gMiSetText) return;
        void* str = x::runtime::il2cpp::NewString(ctx->answer.c_str());
        if (!str) return;
        auto setText = reinterpret_cast<FnSetText>(MethodPtr(gMiSetText));
        auto onOk = FnFromMi<FnVoidSelf>(gMiTextOnOk, kRvaTextOnOk);
        __try {
            if (setText) setText(input, str, gMiSetText);
            onOk(captcha, gMiTextOnOk);
            ctx->ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ctx->ok = false;
        }
        return;
    }
    if (ctx->op == MainCtx::Op::MapCursor) {
        if (!ctx->rectTf) return;
        Vec2 out{};
        const Vec2 local{ctx->lx, ctx->ly};
        auto fn = FnFromMi<FnTryWinCursor>(gMiTryWinCursor, kRvaTryGetWinCursorPos);
        __try {
            ctx->ok = fn(ctx->rectTf, local, &out, gMiTryWinCursor);
            if (ctx->ok) {
                ctx->screen.x = static_cast<LONG>(out.x + 0.5f);
                ctx->screen.y = static_cast<LONG>(out.y + 0.5f);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ctx->ok = false;
        }
        return;
    }
    if (ctx->op == MainCtx::Op::MapBatch) {
        if (!ctx->rectTf || !ctx->localPts || !ctx->screenPts) return;
        ctx->screenPts->clear();
        ctx->havePanelCorners = false;
        ctx->mapMode = "none";
        ctx->verifyMaxErr = -1.f;

        PanelGeometry panel{};
        float verifyErr = -1.f;
        constexpr float kPanelVerifyMaxErrPx = 24.f;  // 仿射与 TryGet 差过大 → 相机/空间不对，回退
        if (MapBatchByPanelAffine(ctx->rectTf, *ctx->localPts, *ctx->screenPts, panel,
                                  &verifyErr)) {
            if (verifyErr >= 0.f && verifyErr > kPanelVerifyMaxErrPx) {
                Log("MapBatch panel-affine reject verifyMaxErr=%.2f > %.0f → tryget", verifyErr,
                    kPanelVerifyMaxErrPx);
                ctx->screenPts->clear();
                ctx->havePanelCorners = false;
            } else {
                ctx->ok = !ctx->screenPts->empty();
                ctx->mapMode = "panel-affine";
                ctx->verifyMaxErr = verifyErr;
                ctx->havePanelCorners = true;
                std::memcpy(ctx->panelCorners, panel.corners, sizeof(ctx->panelCorners));
                const Vec2& p0 = (*ctx->localPts)[0];
                const POINT& s0 = (*ctx->screenPts)[0];
                Log("MapBatch mode=panel-affine pts=%zu firstLocal=(%.2f,%.2f) firstScreen=(%ld,%ld) "
                    "panel=(%ld,%ld)-(%ld,%ld) verifyMaxErr=%.2f",
                    ctx->localPts->size(), p0.x, p0.y, s0.x, s0.y, panel.bounds.left,
                    panel.bounds.top, panel.bounds.right, panel.bounds.bottom, verifyErr);
                return;
            }
        }

        int okN = 0, sehN = 0;
        if (!MapBatchByTryGet(ctx->rectTf, *ctx->localPts, *ctx->screenPts, &okN, &sehN)) {
            ctx->ok = false;
            ctx->mapMode = "tryget-fail";
            Log("MapBatch mode=tryget-fail pts=%zu (panel-affine miss)", ctx->localPts->size());
            return;
        }
        ctx->ok = okN > 0 && !ctx->screenPts->empty();
        ctx->mapMode = "tryget-fallback";
        ctx->verifyMaxErr = verifyErr;
        const Vec2& p0 = (*ctx->localPts)[0];
        const POINT& s0 = (*ctx->screenPts)[0];
        Log("MapBatch mode=tryget-fallback pts=%zu ok=%d seh=%d firstLocal=(%.2f,%.2f) "
            "firstScreen=(%ld,%ld) panelVerifyMaxErr=%.2f",
            ctx->localPts->size(), okN, sehN, p0.x, p0.y, s0.x, s0.y, verifyErr);
    }
}

// --- 托管调用的裸壳 ---
// 公开接口会先把请求投到泵上（见下面 Predicates），泵上的 job 直接用这些裸壳，避免绕回去递归。
bool RawIsOpen() {
    __try {
        return FnFromMi<FnIsOpen>(gMiIsOpen, kRvaIsOpenAntiMacro)(gMiIsOpen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/isOpen");
        return false;
    }
}

bool RawTextInst() {
    __try {
        return FnFromMi<FnIsInst>(gMiTextIsInst, kRvaTextIsInst)(gMiTextIsInst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/textIsInst");
        return false;
    }
}

bool RawNonInst() {
    __try {
        return FnFromMi<FnIsInst>(gMiNonIsInst, kRvaNonIsInst)(gMiNonIsInst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/nonIsInst");
        return false;
    }
}

void* RawTextObj() {
    __try {
        return FnFromMi<FnGetObj>(gMiTextGet, kRvaTextGet)(gMiTextGet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/textGet");
        return nullptr;
    }
}

void* RawNonObj() {
    __try {
        return FnFromMi<FnGetObj>(gMiNonGet, kRvaNonGet)(gMiNonGet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/nonGet");
        return nullptr;
    }
}

// --- 三个谓词一律在主泵上求值 ---
// 这三条路都会进 il2cpp 的 Class::Init（GameAssembly sub_7FF849078670，取全局元数据锁）。
// Init 要的目标类是从 MethodInfo+0x38 的 rgctx 表惰性解出来的——在 worker 上解出来的是垃圾：
// klass+0x60 读到 -1，当场在 GameAssembly+0x3dfbd0 触发 AV。同样的调用在泵上从没炸过。
//
// 2026-08-09 06:39 实测钉死了这个对比：41.79 在泵上把三个谓词各跑一次，干干净净；0.8 秒后
// worker(tid 57340) 调同样三个方法，四次全炸。此前一版按「一次性类初始化」理解、只预热一次，
// 日志证明想错了——这是每次调用都要走的路，换线程就必炸。
//
// 炸完的后果比漏锁严重得多：Init 在出错前已经把 klass+0x135 的「正在初始化」位置上了，异常
// 一展开再没人清。主线程之后碰到这个类就永远回不来，客户端黑屏——上一轮元数据锁已按时归还，
// 照样没救回来，因为坏的是初始化状态，不是锁。
//
// 所以每次都投到泵上算，结果缓存 150 ms，免得 30 Hz 的 tick 把泵打爆。换图 quiesce 期间泵会
// 直接拒非 High 的 job，此时按「没开测谎」处理即可：那段本来也不可能弹测谎 UI。
constexpr DWORD kPredCacheMs = 150;

struct PredSnapshot {
    bool open = false;
    bool text = false;
    bool nonFinite = false;
    // 两个实例指针顺带在同一个 job 里取回：GetAntiMacro() 也是托管静态方法，同样不能在
    // worker 上调，而它紧跟在「开着吗」后面被 follower 调用。搭同一趟车，不额外占泵。
    void* textInst = nullptr;
    void* nonInst = nullptr;
};

std::mutex gPredMtx;
PredSnapshot gPredCache;
DWORD gPredAtMs = 0;

void PredJob(void* user) {
    auto* out = static_cast<PredSnapshot*>(user);
    out->open = (gMiIsOpen && GaBase()) ? RawIsOpen() : false;
    out->text = gMiTextIsInst ? RawTextInst() : false;
    out->nonFinite = gMiNonIsInst ? RawNonInst() : false;
    // 没开着就不必去取实例：GetAntiMacro() 这时本来也只会返回 null。
    out->textInst = (out->text && gMiTextGet) ? RawTextObj() : nullptr;
    out->nonInst = (out->nonFinite && gMiNonGet) ? RawNonObj() : nullptr;
}

PredSnapshot Predicates() {
    std::lock_guard<std::mutex> lk(gPredMtx);
    const DWORD now = GetTickCount();
    if (gPredAtMs && static_cast<int>(now - gPredAtMs) < static_cast<int>(kPredCacheMs))
        return gPredCache;

    PredSnapshot snap;
    if (x::runtime::main_thread::IsOnPumpThread()) {
        PredJob(&snap);
    } else if (!x::runtime::main_thread::InvokeAndWait(&PredJob, &snap, 800)) {
        snap = PredSnapshot{};
    }
    gPredAtMs = now;
    gPredCache = snap;
    return snap;
}

}  // namespace

bool Ensure() {
    std::lock_guard<std::mutex> lk(gMtx);
    if (gReady) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    if (!x::runtime::il2cpp::FindClass("", kUtilClass)) return false;
    if (!ResolveQuizKlass(kTextCaptchaClass, kPrefabTextCaptcha)) return false;
    if (!ResolveQuizKlass(kNonFiniteClass, kPrefabNonFinite)) return false;
    ResolveHelpers();
    EnsureGameMethodInfos();
    EnsureQuizFieldOff();
    gReady = true;
    Log("ready util/text/nonfinite encodePng=%d open=%d textGet=%d cursor=%d",
        gMiEncodePng ? 1 : 0, gMiIsOpen ? 1 : 0, gMiTextGet ? 1 : 0, gMiTryWinCursor ? 1 : 0);
    return true;
}

BindReady ProbeBindReady() {
    BindReady r{};
    std::lock_guard<std::mutex> lk(gMtx);
    r.il2cpp = x::runtime::il2cpp::Ensure();
    if (!r.il2cpp) return r;
    r.gaBase = GaBase() != 0;
    r.klassUtil = x::runtime::il2cpp::FindClass("", kUtilClass) != nullptr;
    r.klassText = ResolveQuizKlass(kTextCaptchaClass, kPrefabTextCaptcha) != nullptr;
    r.klassNonFinite = ResolveQuizKlass(kNonFiniteClass, kPrefabNonFinite) != nullptr;
    ResolveHelpers();
    r.inputSetText = gMiSetText != nullptr;
    r.getTransform = gMiGetTransform != nullptr;
    r.encodePng = gMiEncodePng != nullptr;
    r.quizOk = r.gaBase && r.klassUtil && r.klassText && r.inputSetText;
    r.mouseOk = r.gaBase && r.klassUtil && r.klassNonFinite && r.getTransform;
    r.ok = r.quizOk && r.mouseOk;
    return r;
}

bool IsOpenAntiMacro() {
    if (!Ensure() || !GaBase()) return false;
    return Predicates().open;
}

bool IsTextCaptchaOpen() {
    if (!Ensure()) return false;
    return Predicates().text;
}

bool IsNonFiniteOpen() {
    if (!Ensure()) return false;
    return Predicates().nonFinite;
}

void* GetTextCaptcha() {
    if (!Ensure()) return nullptr;
    return Predicates().textInst;
}

void* GetNonFinite() {
    if (!Ensure()) return nullptr;
    return Predicates().nonInst;
}

bool DumpTextCaptchaImage(std::vector<uint8_t>& out, CaptchaImageKind& kind) {
    out.clear();
    kind = CaptchaImageKind::Unknown;
    if (!Ensure()) return false;
    MainCtx ctx{};
    ctx.op = MainCtx::Op::DumpImage;
    ctx.image = &out;
    ctx.kind = &kind;
    if (!x::runtime::main_thread::InvokeAndWait(&MainJob, &ctx, 2500)) return false;
    return ctx.ok && !out.empty() && kind != CaptchaImageKind::Unknown;
}

bool SubmitTextCaptchaAnswer(const std::string& answer) {
    if (!Ensure() || answer.empty()) return false;
    MainCtx ctx{};
    ctx.op = MainCtx::Op::Submit;
    ctx.answer = answer;
    if (!x::runtime::main_thread::InvokeAndWait(&MainJob, &ctx, 2500)) return false;
    return ctx.ok;
}

int ReadNonFiniteTickFrame(void* instance) {
    if (!instance) return -1;
    EnsureQuizFieldOff();
    void* tick = ReadPtr(instance, kOffNonTick);
    if (!tick) return -1;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(tick) + kOffTickFrame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ReadMouseSampleCount(void* instance) {
    if (!instance) return 0;
    EnsureQuizFieldOff();
    void* list = ReadPtr(instance, gOffNonMousePosList);
    return ListSize(list);
}

bool ReadNonFiniteIsResultRecv(void* instance) {
    if (!instance) return false;
    EnsureQuizFieldOff();
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(instance) +
                                           gOffNonIsResultRecv) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* PeekNonFiniteMouseList(void* instance) {
    if (!instance) return nullptr;
    EnsureQuizFieldOff();
    return ReadPtr(instance, gOffNonMousePosList);
}

bool ReadRawPosList(void* instance, std::vector<Vec2>& out) {
    out.clear();
    if (!instance) return false;
    EnsureQuizFieldOff();
    void* list = ReadPtr(instance, gOffNonRawPosList);
    const int n = ListSize(list);
    void* items = ListItems(list);
    if (!items || n <= 0 || n > 2000) return false;
    __try {
        const uint8_t* base = reinterpret_cast<uint8_t*>(items) + kOffByteArrayData;
        // List<Vector2>：元素 stride=8（两 float）；勿按 Vector2Int/指针读。
        const size_t maxLen = *reinterpret_cast<const int*>(
            reinterpret_cast<const uint8_t*>(items) + 0x18);
        if (maxLen > 0 && static_cast<size_t>(n) > maxLen) return false;
        out.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto* p =
                reinterpret_cast<const Vec2*>(base + static_cast<size_t>(i) * sizeof(Vec2));
            out[static_cast<size_t>(i)] = *p;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
        return false;
    }
}

namespace {

// Component.get_transform 也是托管方法，同样只许在泵上调（follower 的 BuildAndPublishPlan /
// TickCalibration 都在 worker 上）。这条不像谓词那样每 tick 都要，直接一次一个 job 即可。
struct RectCtx {
    void* rawImage = nullptr;
    void* result = nullptr;
};

void RectJob(void* user) {
    auto* c = static_cast<RectCtx*>(user);
    auto getTf = reinterpret_cast<FnGetTransform>(MethodPtr(gMiGetTransform));
    if (!getTf) return;
    __try {
        c->result = getTf(c->rawImage, gMiGetTransform);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("antiMacro/getTransform");
        c->result = nullptr;
    }
}

}  // namespace

void* ReadNonFiniteTargetRect(void* instance) {
    if (!instance || !ResolveHelpers() || !gMiGetTransform) return nullptr;
    RectCtx ctx{};
    ctx.rawImage = ReadPtr(instance, kOffNonRawImage);
    if (!ctx.rawImage) return nullptr;
    if (x::runtime::main_thread::IsOnPumpThread())
        RectJob(&ctx);
    else if (!x::runtime::main_thread::InvokeAndWait(&RectJob, &ctx, 800))
        return nullptr;
    return ctx.result;
}

// CMS/枫星 DecryptPointToCursorPoint：归一化 path → 750×500 题面像素局部。
constexpr float kRawDecryptAddX = 0.75f;
constexpr float kRawDecryptAddY = 0.5f;
constexpr float kRawDecryptScale = 500.f;
constexpr float kCanvasPixelSpanMin = 50.f;  // 已是题面像素则不再二次放大
constexpr long kScreenCollapseMaxPx = 8;    // 本地有跨度但屏幕 ≤ 此值 = 塌缩

Vec2 RawToCursorLocal(Vec2 raw) {
    return Vec2{(raw.x + kRawDecryptAddX) * kRawDecryptScale,
                (raw.y + kRawDecryptAddY) * kRawDecryptScale};
}

bool RawPathLooksLikeCanvasPixels(const std::vector<Vec2>& raw) {
    if (raw.size() < 2) return false;
    float minX = raw[0].x, maxX = raw[0].x, minY = raw[0].y, maxY = raw[0].y;
    for (const auto& p : raw) {
        minX = (std::min)(minX, p.x);
        maxX = (std::max)(maxX, p.x);
        minY = (std::min)(minY, p.y);
        maxY = (std::max)(maxY, p.y);
    }
    const float span = (std::max)(maxX - minX, maxY - minY);
    return span >= kCanvasPixelSpanMin;
}

bool IsScreenPathCollapsed(const std::vector<Vec2>& localPts, const std::vector<POINT>& screen,
                           long* outSpanX, long* outSpanY) {
    if (outSpanX) *outSpanX = 0;
    if (outSpanY) *outSpanY = 0;
    if (localPts.size() < 2 || screen.size() != localPts.size()) return true;

    float minLx = localPts[0].x, maxLx = localPts[0].x;
    float minLy = localPts[0].y, maxLy = localPts[0].y;
    long minSx = screen[0].x, maxSx = screen[0].x;
    long minSy = screen[0].y, maxSy = screen[0].y;
    for (size_t i = 0; i < localPts.size(); ++i) {
        minLx = (std::min)(minLx, localPts[i].x);
        maxLx = (std::max)(maxLx, localPts[i].x);
        minLy = (std::min)(minLy, localPts[i].y);
        maxLy = (std::max)(maxLy, localPts[i].y);
        minSx = (std::min)(minSx, screen[i].x);
        maxSx = (std::max)(maxSx, screen[i].x);
        minSy = (std::min)(minSy, screen[i].y);
        maxSy = (std::max)(maxSy, screen[i].y);
    }
    const long spanX = maxSx - minSx;
    const long spanY = maxSy - minSy;
    if (outSpanX) *outSpanX = spanX;
    if (outSpanY) *outSpanY = spanY;

    const float localSpan = (std::max)(maxLx - minLx, maxLy - minLy);
    if (localSpan < 1e-3f) return true;  // 本地本身无轨迹
    if ((std::max)(spanX, spanY) <= kScreenCollapseMaxPx) return true;

    // 飞出游戏客户区（E175：Y=1511 而 client 高 768）也当塌缩/废图。
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) hwnd = x::features::titlebar::win::FindGameWindow();
    if (hwnd && IsWindow(hwnd)) {
        RECT cr{};
        POINT origin{};
        if (GetClientRect(hwnd, &cr) && ClientToScreen(hwnd, &origin)) {
            const long l = origin.x;
            const long t = origin.y;
            const long r = origin.x + (cr.right - cr.left);
            const long b = origin.y + (cr.bottom - cr.top);
            const long pad = 64;
            int outside = 0;
            const int n = static_cast<int>(screen.size());
            const int probe = (std::min)(n, 32);
            for (int i = 0; i < probe; ++i) {
                const size_t idx =
                    probe <= 1 ? 0u : static_cast<size_t>(i) * static_cast<size_t>(n - 1) /
                                          static_cast<size_t>(probe - 1);
                const POINT& p = screen[idx];
                if (p.x < l - pad || p.x > r + pad || p.y < t - pad || p.y > b + pad) ++outside;
            }
            if (outside * 2 >= probe) return true;
        }
    }
    return false;
}

bool TryMapWinCursor(void* rectTransform, float localX, float localY, POINT& outScreen) {
    if (!Ensure() || !rectTransform) return false;
    MainCtx ctx{};
    ctx.op = MainCtx::Op::MapCursor;
    ctx.rectTf = rectTransform;
    ctx.lx = localX;
    ctx.ly = localY;
    if (!x::runtime::main_thread::InvokeAndWait(&MainJob, &ctx, 800)) return false;
    if (!ctx.ok) return false;
    outScreen = ctx.screen;
    return true;
}

bool MapWinCursorBatch(void* rectTransform, const std::vector<Vec2>& localPts,
                       std::vector<POINT>& outScreen, POINT* outPanelCorners4,
                       bool* outHavePanelCorners) {
    outScreen.clear();
    if (outHavePanelCorners) *outHavePanelCorners = false;
    if (!Ensure() || !rectTransform || localPts.empty()) return false;

    // 归一化 raw → 题面像素局部（已是像素则原样）。
    std::vector<Vec2> cursorLocal;
    cursorLocal.reserve(localPts.size());
    const bool alreadyPx = RawPathLooksLikeCanvasPixels(localPts);
    for (const auto& p : localPts) {
        cursorLocal.push_back(alreadyPx ? p : RawToCursorLocal(p));
    }

    // 主线程绑定面板 API（失败仍可走 TryGet 回退）
    (void)BindPanelMapApis();

    MainCtx ctx{};
    ctx.op = MainCtx::Op::MapBatch;
    ctx.rectTf = rectTransform;
    ctx.localPts = &cursorLocal;
    ctx.screenPts = &outScreen;
    if (!x::runtime::main_thread::InvokeAndWait(&MainJob, &ctx, 8000)) return false;
    if (!ctx.ok || outScreen.size() != localPts.size()) return false;

    if (outPanelCorners4 && ctx.havePanelCorners) {
        std::memcpy(outPanelCorners4, ctx.panelCorners, sizeof(POINT) * 4);
        if (outHavePanelCorners) *outHavePanelCorners = true;
    }

    long spanX = 0, spanY = 0;
    const bool collapsed = IsScreenPathCollapsed(cursorLocal, outScreen, &spanX, &spanY);
    Log("MapBatch decrypt=%d mode=%s firstRaw=(%.3f,%.3f) firstCursor=(%.1f,%.1f) "
        "screenSpan=(%ld,%ld) collapsed=%d verifyMaxErr=%.2f panelCorners=%d",
        alreadyPx ? 0 : 1, ctx.mapMode ? ctx.mapMode : "?", localPts[0].x, localPts[0].y,
        cursorLocal[0].x, cursorLocal[0].y, spanX, spanY, collapsed ? 1 : 0, ctx.verifyMaxErr,
        ctx.havePanelCorners ? 1 : 0);
    if (collapsed) {
        // 保留 outScreen 供 caller 落盘取证；勿清空。
        return false;
    }
    return true;
}

namespace {

HWND ResolveGameHwnd() {
    HWND hwnd = x::features::titlebar::win::FindUnityWndClass();
    if (!hwnd || !IsWindow(hwnd)) hwnd = x::features::titlebar::win::FindGameWindow();
    if (hwnd && IsWindow(hwnd)) return hwnd;
    return nullptr;
}

DWORD gLastFgAttemptMs = 0;
constexpr DWORD kFgRetryMs = 400;

}  // namespace

bool IsGameForeground() {
    HWND hwnd = ResolveGameHwnd();
    if (!hwnd) return false;
    if (IsIconic(hwnd)) return false;
    return GetForegroundWindow() == hwnd;
}

bool TryBringGameForeground(const char* why, bool force) {
    // 对照仓（Artale）默认外部策略：在 **worker** 上 AttachThreadInput + SFW。
    // 经典版踩坑：把 SFW 丢进 MainPump InvokeAndWait / 无 Attach 的外线程 → 泵死。
    // 铁律：只在非泵线程直调；最小化只 ShowWindowAsync（不在此处 Sleep，避免堵 Tick）。
    HWND hwnd = ResolveGameHwnd();
    if (!hwnd) {
        x::runtime::LogW("AutoLieFocus", "skip (%s): no game hwnd", why ? why : "?");
        return false;
    }
    if (x::runtime::main_thread::IsOnPumpThread()) {
        x::runtime::LogW("AutoLieFocus", "refuse (%s): on pump thread", why ? why : "?");
        return IsGameForeground();
    }
    if (GetForegroundWindow() == hwnd && !IsIconic(hwnd)) return true;

    const DWORD now = GetTickCount();
    if (!force && gLastFgAttemptMs && now - gLastFgAttemptMs < kFgRetryMs) {
        return IsGameForeground();
    }
    gLastFgAttemptMs = now;

    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);

    AllowSetForegroundWindow(ASFW_ANY);

    const HWND fore = GetForegroundWindow();
    const DWORD curTid = GetCurrentThreadId();
    const DWORD foreTid = fore ? GetWindowThreadProcessId(fore, nullptr) : 0;
    const DWORD targetTid = GetWindowThreadProcessId(hwnd, nullptr);
    const bool attachFore =
        fore && foreTid && foreTid != curTid && AttachThreadInput(curTid, foreTid, TRUE);
    const bool attachTarget = targetTid && targetTid != curTid && targetTid != foreTid &&
                              AttachThreadInput(curTid, targetTid, TRUE);

    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    const BOOL setOk = SetForegroundWindow(hwnd);
    if (!IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOW);

    if (attachTarget) AttachThreadInput(curTid, targetTid, FALSE);
    if (attachFore) AttachThreadInput(curTid, foreTid, FALSE);

    const bool ok = IsGameForeground();
    x::runtime::LogI("AutoLieFocus", "%s (%s) attach-SFW force=%d set=%d fg=%d",
                     ok ? "ok" : "weak", why ? why : "?", force ? 1 : 0, setOk ? 1 : 0,
                     ok ? 1 : 0);
    return ok;
}

}  // namespace x::features::auto_lie::anti_macro_port
