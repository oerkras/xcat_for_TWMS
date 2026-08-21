#include "anti_macro_port.h"

#include "lie_log.h"
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
    "ba89056b68ab91b0d96cb616a204b0b91ecb369f654cf26919d01d19959cd0e";
constexpr const char* kNonFiniteClass =
    "d3b97970f46a5e81e1df5673a4cd8a60684586e21ef352a907c5466c0ccd25b";
constexpr const char* kTextCaptchaClass =
    "b52930599ec703b2a88b015cb9ac5bc0a5c010a62aa71ca33d5dce6d9545152";
constexpr const char* kTextCaptchaInfoClass =
    "e70ede73106db5a717b61b194c28afca35f13ad309c769d74d5bdccddbba7b0";
constexpr const char* kPrefabNonFinite = "UIAntiMacroNonFinite";
constexpr const char* kPrefabTextCaptcha = "UIAntiMacroTextCaptcha";

void* ResolveQuizKlass(const char* hashName, const char* prefabName) {
    if (prefabName && prefabName[0]) {
        const auto r = x::runtime::il2cpp_prefab::FindClassCached(hashName, prefabName);
        return r.klass;
    }
    return x::runtime::il2cpp::FindClass("", hashName);
}

constexpr uint32_t kRvaIsOpenAntiMacro = 0x94BDE0;  // Util.IsOpenAntiMacro
constexpr uint32_t kRvaTextGet = 0x948a90;  // TextCaptcha.GetAntiMacro
constexpr uint32_t kRvaTextIsInst = 0x948e10;  // TextCaptcha.IsInstantiated
// 真 OnOk（Rosetta bee55c…）；旧误钉 OnSuccess@0x94b910 / abd99c…
constexpr uint32_t kRvaTextOnOk = 0x94B090;
constexpr uint32_t kRvaNonGet = 0x93b380;  // NonFinite.GetAntiMacro
constexpr uint32_t kRvaNonIsInst = 0x93b720;  // NonFinite.IsInstantiated
constexpr uint32_t kRvaTryGetWinCursorPos = 0x94c3d0;  // Util.TryGetWinCursorPos（IDB 实钉；旧文档 0x936C30 已废）
// 面板仿射主映射（对照 Artale PanelLocalToDesktop；RVA = runtime dump 包装，plain 名主路径）
constexpr uint32_t kRvaCamGetMain = 0x4E283D0;           // Camera.get_main（与 fly 同钉）
constexpr uint32_t kRvaCamWorldToScreen = 0x4E27D60;     // Camera.WorldToScreenPoint(Vector3) eye=Mono
constexpr uint32_t kRvaRectGetRect = 0x4EAA920;          // RectTransform.get_rect
constexpr uint32_t kRvaTransformPoint = 0x4EB0CE0;       // Transform.TransformPoint(Vector3)

// 方法哈希（dump.cs）— static bool()/void() 同形多，哈希主路径
constexpr char kHashIsOpenAntiMacro[] =
    "b803eb66e6a35c02e35d78878682bc9d35585c49eafbf3291d84073d6393591";
constexpr char kHashTextGet[] =
    "b4a9b39f3efbef062f57d6b7de1966fb0e1a29bf694a51cfad1d4d8868fe9f0";
constexpr char kHashTextIsInst[] =
    "da817385bb5115f12dba5e6cf6e4fa9fef958b87c3f690c9c45f32446621a93";
constexpr char kHashTextOnOk[] =
    "a42d5f6e54298833dd8d4679e178983ed4979105db68f2cf58579385d62cff5";
constexpr char kHashNonGet[] =
    "d5618b41780774e2319e94598af8928e5b3e5755f7a04256fa557e3fbcd0289";
constexpr char kHashNonIsInst[] =
    "ae123183299b430e15baac90a204a65e8bb1436049621b2d3a44d334ba27192";
constexpr char kHashTryGetWinCursorPos[] =
    "b3c1764f638d8ad27c054fca82ab27691403d6cb60f60ddc620c2a09dfe2d81";
// Quiz 字段：hash → field_get_offset（dump 常量仅 fallback）
constexpr char kHashTextRawImage[] =
    "c63dabe3b6541ee541ec022007fdd73638262aab34ad96a548be1634a1ceed1";
constexpr char kHashTextInputField[] =
    "ef3e04d34f66b4f8f090981e7af3acd7a4fac558edb9dc8fae4a91aa397f83a";
constexpr char kHashNonRawImage[] =
    "c4207c90503b640e60b9dfe33b6a3ddff323365c97b029ac28fd19543cb4d8b";
// 08-20：动画帧在 nested TickCounter（+0xE8），不是旁边 Tick 数据类（+0xE0）。
// IDA GetFrame@0x946710 = *(this+20)；CMS _frame@+0x14。BIN 62cdf8 读错对象 → frame 永不 ≥150。
constexpr char kHashNonTick[] =
    "a3394eed9487087f9acc6da3e706e387bb0376d1685f2d4f39a5a6dbc23cd4a";
// 08-20 dump：在 Transform(+0xC8) 后插了实例 bool(+0xD0)，后续字段整体 +8。
// 按类型钉哈希，禁止按旧偏移 0xE8 当 path——那个槽现在是 nested TickCounter。
constexpr char kHashNonRawPosList[] =
    "a7e297022c3e6e6ba54356d0fa71464dcff30f9987f453373fbe0eaeca9ada5";  // List<Vector2> @0xF0
constexpr char kHashNonMousePosList[] =
    "a1b471803195a60351daffa0be08e82025166b02ece8692161fbcfba120d649";  // List<Vector2Int> @0xF8
constexpr char kHashNonIsResultRecv[] =
    "b8212895edd7c902352d97f5fa412c96f02211d438288d9648c41bbe5cd8187";  // bool @0x100
constexpr char kHashNonIsSuccess[] =
    "d5c149f8beda771fb7ba34c6164d00728da3f78c76dcaa744c1dee45143851b";  // bool @0x110
constexpr char kHashInfoJpegData[] =
    "c2577137da4ffec962e765aa4b83b37064b2b09b80c0f5a9cb590381afdbcaf";
constexpr char kHashTickFrame[] =
    "bda2cfd5fc16e6d2b561108277f0cc97b91967ce7a4ced33a50978d344f85a4";  // TickCounter _frame @0x14
constexpr char kNonTickNestedName[] =
    "dc24636af25f6c2cebea73139f76867d9a9ef0e92f76171602caea686f074c3";

constexpr size_t kFbTextRawImage = 0xA0;
constexpr size_t kFbTextInputField = 0xB0;
constexpr size_t kFbNonRawImage = 0xA0;
constexpr size_t kFbNonTick = 0xE8;  // nested TickCounter（08-14 曾在 0xE0）
constexpr size_t kFbNonRawPosList = 0xF0;
constexpr size_t kFbNonMousePosList = 0xF8;
constexpr size_t kFbNonIsResultRecv = 0x100;
// recv(bool,+0x100) → tick(int,+0x104) → _pathTexture(ptr,+0x108) → _isSuccess(bool,+0x110)
// 相对仍是 +0x10；优先用 kHashNonIsSuccess 直绑，hash 失败再跟 recv 平移。
constexpr size_t kFbNonIsSuccess = 0x110;
constexpr size_t kFbInfoJpegData = 0x20;
constexpr size_t kFbTickFrame = 0x14;  // GetFrame: *(this+20)
size_t gOffTextRawImage = kFbTextRawImage;
size_t gOffTextInputField = kFbTextInputField;
size_t gOffNonRawImage = kFbNonRawImage;
size_t gOffNonTick = kFbNonTick;
size_t gOffNonRawPosList = kFbNonRawPosList;
size_t gOffNonMousePosList = kFbNonMousePosList;
size_t gOffNonIsResultRecv = kFbNonIsResultRecv;
size_t gOffNonIsSuccess = kFbNonIsSuccess;
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
    if (FieldOffHit(non, kHashNonIsSuccess, kFbNonIsSuccess, &gOffNonIsSuccess, 0x80, 0x200))
        ++hits;
    else
        gOffNonIsSuccess = gOffNonIsResultRecv + (kFbNonIsSuccess - kFbNonIsResultRecv);
    if (FieldOffHit(info, kHashInfoJpegData, kFbInfoJpegData, &gOffInfoJpegData, 0x10, 0x80))
        ++hits;
    if (FieldOffHit(tick, kHashTickFrame, kFbTickFrame, &gOffTickFrame, 0x10, 0x40)) ++hits;
    // 这条是排障基线（字段有没有绑上），得跟测谎其它日志一起留在 auto_lie.log 里。
    // Log() 定义在本文件后面，这里直接拼串走分流。
    char buf[420]{};
    snprintf(buf, sizeof(buf),
             "quiz fields path=%s hits=%d/10 txtRaw=0x%zX in=0x%zX nonRaw=0x%zX tick=0x%zX "
             "rawPos=0x%zX mouse=0x%zX recv=0x%zX succ=0x%zX jpeg=0x%zX frame=0x%zX",
             hits == 10 ? "meta" : (hits ? "meta-partial" : "fallback"), hits, gOffTextRawImage,
             gOffTextInputField, gOffNonRawImage, gOffNonTick, gOffNonRawPosList,
             gOffNonMousePosList, gOffNonIsResultRecv, gOffNonIsSuccess, gOffInfoJpegData,
             gOffTickFrame);
    lie_log::Line("AutoLiePort", buf);
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
// BIN（RVA 0x94c3d0 序言）：rcx=RectTransform*, rdx=Vector2(8B by-value), r8=out*, r9=MethodInfo*
// 误写成 (float,float) 会把 out* 挤到 R9、RDX/R8 成垃圾 → MapBatch 全点 ok=0（E175 真题）。
static_assert(sizeof(Vec2) == 8, "Vec2 must be 8B for IL2CPP Vector2-by-value ABI");
// IDA（GameAssembly runtime dump）：TryGetWinCursorPos @ RVA 0x94c3d0
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

// RawToCursorLocal 把归一化 raw 映到 CMS cursor-point 画布 (0..750, 0..500)，Y 向下。
// 这是**逻辑画布**，不是 RectTransform 的 UI 单位：面板实际只有几百 UI 单位宽。
// 0.1.114 事故：两者被当成同一空间 —— panel-affine 的 UV 恒 >1 直接 miss，
// 回退的 TryGetWinCursorPos 又按 1:1 收下 (376.9,249.3)，把轨迹推到客户区右外侧
// （实测拟合 screen = raw*500 + C，缩放正好 1.0）。任何时候先过这两个常量归一化。
constexpr float kCursorCanvasW = 750.f;
constexpr float kCursorCanvasH = 500.f;

// cursor-point → RectTransform 局部（中心为原点、Y 向上、UI 单位）。
bool CursorPointToRectLocal(const UnityRect& rect, float cursorX, float cursorY, Vec2& out) {
    if (!(rect.width > 1.f) || !(rect.height > 1.f)) return false;
    const float u = cursorX / kCursorCanvasW;
    const float v = cursorY / kCursorCanvasH;  // cursor-point 的 Y 向下
    out.x = rect.x + u * rect.width;
    out.y = rect.y + (1.f - v) * rect.height;
    return std::isfinite(out.x) && std::isfinite(out.y);
}

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
    lie_log::Line("AutoLiePort", buf);
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

    // Overlay 口径只需 get_rect + TransformPoint；camMain/wts 仅留作诊断，缺了也不挡主路径。
    gPanelMapBound = gMiGetRect && gMiTransformPoint;
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
    if (!tp) return false;

    Vec3 world{};
    __try {
        tp(&world, targetRect, &local, gMiTransformPoint);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) {
        return false;
    }

    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return false;
    const int clientHeight = client.bottom - client.top;
    // 测谎 Canvas 是 Screen Space - Overlay：UI 的 world 坐标**本身就是屏幕像素**
    // （Y 自下而上），只需翻 Y 再加客户区原点。
    //
    // 绝不能再过 Camera.main.WorldToScreenPoint：Camera.main 是跟着角色跑的游戏世界相机，
    // 它会把面板整体平移「相机位置」那么多，且偏移量随角色走位变化，所以每次测谎偏的方向
    // 都不一样。0.1.117（67A6 设备 82a4b0）实测铁证：反解出 1 世界单位 = 1.001 像素、
    // 相机中心 (925.2,560.1)，而同刻角色 AbsPos=(999→950, 554)，y 仅差 6 px；扣掉相机后
    // UI world 四角为 x:[308,1058] y:[134,634] —— 宽 750 高 500、左右边距各 308、上下各
    // 134，在 1366x768 客户区里完美居中，正是真面板该在的位置。
    desktopF.x = static_cast<double>(origin.x) + static_cast<double>(world.x);
    desktopF.y = static_cast<double>(origin.y) + static_cast<double>(clientHeight) -
                 static_cast<double>(world.y);
    desktop.x = static_cast<long>(std::lround(desktopF.x));
    desktop.y = static_cast<long>(std::lround(desktopF.y));
    return true;
}

// RectTransform.get_rect 只读壳（泵上调用）。TryGet 路径也要它把画布坐标降到 UI 单位。
bool ReadRectOf(void* targetRect, UnityRect& out) {
    if (!BindPanelMapApis() || !targetRect) return false;
    auto getRect = reinterpret_cast<FnGetRect>(MethodPtr(gMiGetRect));
    if (!getRect) return false;
    UnityRect rect{};
    __try {
        getRect(&rect, targetRect, gMiGetRect);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.width < 1.f || rect.height < 1.f) {
        return false;
    }
    out = rect;
    return true;
}

bool ResolvePanelGeometry(void* targetRect, HWND hwnd, PanelGeometry& panel) {
    panel = {};
    if (!BindPanelMapApis() || !targetRect || !hwnd) return false;

    UnityRect rect{};
    if (!ReadRectOf(targetRect, rect)) return false;
    if (rect.width < 100.f || rect.height < 100.f) return false;
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

    // 真面板总是居中在客户区里。若算出来的四角跑出窗口，说明 canvas 口径与假设不符——
    // 此时必须 affine-fail 让上层报警、由玩家手动作答；把光标甩到窗口外只会白送一次失败。
    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return false;
    const long cl = origin.x;
    const long ct = origin.y;
    const long cr = origin.x + (client.right - client.left);
    const long cb = origin.y + (client.bottom - client.top);
    const long midX = (panel.bounds.left + panel.bounds.right) / 2;
    const long midY = (panel.bounds.top + panel.bounds.bottom) / 2;
    if (midX < cl || midX > cr || midY < ct || midY > cb) {
        Log("panel reject: center (%ld,%ld) outside client LTRB=(%ld,%ld,%ld,%ld)", midX, midY, cl,
            ct, cr, cb);
        return false;
    }
    constexpr long kEdgePad = 8;  // 只容忍取整/边框误差
    if (panel.bounds.left < cl - kEdgePad || panel.bounds.top < ct - kEdgePad ||
        panel.bounds.right > cr + kEdgePad || panel.bounds.bottom > cb + kEdgePad) {
        Log("panel reject: bounds LTRB=(%ld,%ld,%ld,%ld) spills client LTRB=(%ld,%ld,%ld,%ld)",
            panel.bounds.left, panel.bounds.top, panel.bounds.right, panel.bounds.bottom, cl, ct, cr,
            cb);
        return false;
    }
    return true;
}

bool CursorPointToPanelDesktop(const PanelGeometry& panel, float cursorX, float cursorY,
                               POINT& desktop) {
    // 归一化必须用 cursor-point 画布常量，勿用 panel.rect 尺寸：rect 是面板 UI 单位
    // （本机实测约 314×222），拿它去除 0..750 的画布坐标会让 u 恒 >1，仿射整条 miss。
    // offset 与 cursorX/Y 同为画布单位；TWMS 无 CursorEnv 时恒 0。
    const double u = (static_cast<double>(cursorX) - panel.offset.x) / kCursorCanvasW;
    const double v =
        1.0 - (static_cast<double>(cursorY) - panel.offset.y) / kCursorCanvasH;
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
    if (!hwnd) {
        Log("panel-affine miss: no game hwnd");
        return false;
    }
    if (!ResolvePanelGeometry(rectTf, hwnd, panel)) {
        Log("panel-affine miss: ResolvePanelGeometry (rect/corners)");
        return false;
    }

    outScreen.reserve(cursorLocal.size());
    int uvFail = 0;
    for (const auto& lp : cursorLocal) {
        POINT pt{};
        if (!CursorPointToPanelDesktop(panel, lp.x, lp.y, pt)) {
            ++uvFail;
            outScreen.clear();
            Log("panel-affine miss: UV/bounds fail at local=(%.2f,%.2f) rect=(%.1f,%.1f,%.1f,%.1f) "
                "uvFails=%d/%zu",
                lp.x, lp.y, panel.rect.x, panel.rect.y, panel.rect.width, panel.rect.height, uvFail,
                cursorLocal.size());
            return false;
        }
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
            Vec2 local{};
            if (!CursorPointToRectLocal(panel.rect, cursorLocal[i].x, cursorLocal[i].y, local))
                continue;
            Vec2 out{};
            bool hit = false;
            __try {
                hit = fn(rectTf, local, &out, gMiTryWinCursor);
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
    // 取证：无论走哪条路都先记一份 rect，离线才能复算 cursor-point → UI 单位那一步。
    bool haveRect = false;
    UnityRect rect{};
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
        // 单点 TryGet 只剩闭环节的 log-only 诊断在用（主映射一律走仿射）；
        // 口径仍与仿射一致：画布坐标先降到 RectTransform UI 单位再喂。
        UnityRect rect{};
        Vec2 local{};
        if (!ReadRectOf(ctx->rectTf, rect) ||
            !CursorPointToRectLocal(rect, ctx->lx, ctx->ly, local)) {
            ctx->ok = false;
            return;
        }
        Vec2 out{};
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
        ctx->haveRect = ReadRectOf(ctx->rectTf, ctx->rect);

        // 面板仿射是**唯一**主映射路径。TryGet 已被 0.1.114 / 0.1.116 三台机两个版本证明
        // 系统性漏乘 canvas scaleFactor（screen = cursor×1.0 + 面板原点），映出来的轨迹被压成
        // 画布原尺寸塞进客户区一角 —— 它既不能当兜底，更不能反过来当真值去校验仿射。
        // 现在它只在 MapBatchByPanelAffine 内做首/中/末交叉诊断，差值记进 verifyMaxErr 供分析。
        PanelGeometry panel{};
        float verifyErr = -1.f;
        if (!MapBatchByPanelAffine(ctx->rectTf, *ctx->localPts, *ctx->screenPts, panel,
                                   &verifyErr)) {
            ctx->ok = false;
            ctx->mapMode = "affine-fail";
            ctx->screenPts->clear();
            Log("MapBatch mode=affine-fail pts=%zu — refuse follow (no TryGet fallback by design)",
                ctx->localPts->size());
            return;
        }
        ctx->ok = !ctx->screenPts->empty();
        ctx->mapMode = "panel-affine";
        ctx->verifyMaxErr = verifyErr;
        ctx->havePanelCorners = true;
        std::memcpy(ctx->panelCorners, panel.corners, sizeof(ctx->panelCorners));
        const Vec2& p0 = (*ctx->localPts)[0];
        const POINT& s0 = (*ctx->screenPts)[0];
        Log("MapBatch mode=panel-affine pts=%zu firstLocal=(%.2f,%.2f) firstScreen=(%ld,%ld) "
            "panel=(%ld,%ld)-(%ld,%ld) verifyMaxErr=%.2f (tryget diff, log-only)",
            ctx->localPts->size(), p0.x, p0.y, s0.x, s0.y, panel.bounds.left, panel.bounds.top,
            panel.bounds.right, panel.bounds.bottom, verifyErr);
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

// 快照过期就按「取不到 = 没开测谎」处理：宁可慢半拍发现题目，也不拿陈旧结论去动光标。
constexpr DWORD kPredStaleMs = 600;
// 连着这么久没人问，刷新线程转空档，别白占泵。
constexpr DWORD kPredIdleMs = 3000;

// 这把锁**只护一次结构体拷贝，绝不跨泵等待**。跨着持锁去等泵是个闭环陷阱：泵侧只要有谁
// 回调本 port 一句谓词，就会「worker 持锁等泵、泵等锁」当场死。今天泵侧没有这种调用者
// （LieFramePulse / PulseCursorOnPump / MainJob 都不碰），但那是巧合，不能当设计依据。
std::mutex gPredMtx;
PredSnapshot gPredCache;
DWORD gPredAtMs = 0;

// 谁在问、刷新线程活着没。
std::atomic<DWORD> gPredWantMs{0};
std::atomic<bool> gRefreshRun{false};
std::atomic<HANDLE> gRefreshThread{nullptr};
std::mutex gRefreshStartMtx;

void PredJob(void* user) {
    auto* out = static_cast<PredSnapshot*>(user);
    out->open = (gMiIsOpen && GaBase()) ? RawIsOpen() : false;
    out->text = gMiTextIsInst ? RawTextInst() : false;
    out->nonFinite = gMiNonIsInst ? RawNonInst() : false;
    // 没开着就不必去取实例：GetAntiMacro() 这时本来也只会返回 null。
    out->textInst = (out->text && gMiTextGet) ? RawTextObj() : nullptr;
    out->nonInst = (out->nonFinite && gMiNonGet) ? RawNonObj() : nullptr;
}

void PublishPred(const PredSnapshot& snap) {
    DWORD now = GetTickCount();
    if (!now) now = 1;
    std::lock_guard<std::mutex> lk(gPredMtx);
    gPredCache = snap;
    gPredAtMs = now;
}

// 专用刷新线程：**所有等泵的时间都花在这条可牺牲的线程上**。auto_lie worker 只读快照，
// 节奏不受泵拥堵影响——真题跟随时 worker 要按拍建计划/判中止，被 InvokeAndWait 顶住 800 ms
// 是能看出来的。（对照仓也是这个思路：危险且会阻塞的调用放后台线程，帧脉冲保持非阻塞。）
DWORD WINAPI PredRefreshThread(LPVOID) {
    while (gRefreshRun.load(std::memory_order_acquire)) {
        const DWORD want = gPredWantMs.load(std::memory_order_relaxed);
        const bool wanted =
            want && static_cast<int>(GetTickCount() - want) < static_cast<int>(kPredIdleMs);
        if (wanted) {
            PredSnapshot snap;
            if (x::runtime::main_thread::InvokeAndWait(&PredJob, &snap, 800)) PublishPred(snap);
            // 泵拒（换图 quiesce）或超时：不覆盖旧值，让它自然过期成 stale，
            // 上面读到过期就返回全 false，等于「这会儿没开测谎」。
        }
        Sleep(wanted ? kPredCacheMs / 2 : 250);
    }
    return 0;
}

void EnsureRefresher() {
    if (gRefreshRun.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(gRefreshStartMtx);
    if (gRefreshRun.load(std::memory_order_acquire)) return;
    gRefreshRun.store(true, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &PredRefreshThread, nullptr, 0, nullptr);
    if (!th) {
        gRefreshRun.store(false, std::memory_order_release);
        Log("谓词刷新线程 CreateThread 失败，测谎探测将持续返回「未开启」");
        return;
    }
    gRefreshThread.store(th, std::memory_order_release);
}

PredSnapshot Predicates() {
    if (x::runtime::main_thread::IsOnPumpThread()) {
        // 已经在泵上，直接算最新的，顺手把快照刷了；这条路不等任何东西。
        PredSnapshot snap;
        PredJob(&snap);
        PublishPred(snap);
        return snap;
    }

    DWORD now = GetTickCount();
    if (!now) now = 1;
    gPredWantMs.store(now, std::memory_order_relaxed);
    EnsureRefresher();

    std::lock_guard<std::mutex> lk(gPredMtx);
    if (!gPredAtMs || static_cast<int>(now - gPredAtMs) > static_cast<int>(kPredStaleMs))
        return PredSnapshot{};
    return gPredCache;
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

void StopRefresher() {
    gRefreshRun.store(false, std::memory_order_release);
    HANDLE th = gRefreshThread.exchange(nullptr, std::memory_order_acq_rel);
    if (!th) return;
    // 若在泵上收尾就绝不能等：刷新线程此刻可能正卡在 InvokeAndWait 上等这条泵，
    // 一等就是互锁。已置 gRefreshRun=false，让它自己走完当前这拍退出。
    // 现有卸载路径（StopAllFeatureWorkers）都在 bootstrap/detach 线程上，这里是保险。
    if (x::runtime::main_thread::IsOnPumpThread()) {
        CloseHandle(th);
        return;
    }
    // 最多卡在一次 InvokeAndWait(800) 里，5s 足够；超时只记一笔，不强杀。
    if (WaitForSingleObject(th, 5000) == WAIT_TIMEOUT)
        Log("StopRefresher wait timeout; 刷新线程可能仍在退出");
    CloseHandle(th);
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

int ReadNonFiniteIsSuccess(void* instance) {
    if (!instance) return -1;
    EnsureQuizFieldOff();
    __try {
        // 原始字节，不归一：0/1 之外的值就是偏移不对的证据（见头文件）。
        return static_cast<int>(
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(instance) + gOffNonIsSuccess));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool IsPredFresh() {
    // 泵上直接算，永远新鲜（Predicates 那条路不等任何东西）。
    if (x::runtime::main_thread::IsOnPumpThread()) return true;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(gPredMtx);
    return gPredAtMs != 0 && static_cast<int>(now - gPredAtMs) <= static_cast<int>(kPredStaleMs);
}

bool IsPredStale() {
    if (x::runtime::main_thread::IsOnPumpThread()) return false;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(gPredMtx);
    return gPredAtMs != 0 && static_cast<int>(now - gPredAtMs) > static_cast<int>(kPredStaleMs);
}

void* PeekNonFiniteMouseList(void* instance) {
    if (!instance) return nullptr;
    EnsureQuizFieldOff();
    return ReadPtr(instance, gOffNonMousePosList);
}

const char* RawPathWhyTag(RawPathWhy why) {
    switch (why) {
        case RawPathWhy::Ok:
            return "ok";
        case RawPathWhy::NoInst:
            return "no-inst";
        case RawPathWhy::NoList:
            return "no-list";
        case RawPathWhy::Empty:
            return "empty";
        case RawPathWhy::TooBig:
            return "too-big";
        case RawPathWhy::NoItems:
            return "no-items";
        case RawPathWhy::CapMismatch:
            return "cap-mismatch";
        case RawPathWhy::Seh:
            return "seh";
        default:
            return "?";
    }
}

bool ReadRawPosList(void* instance, std::vector<Vec2>& out, RawPathPeek* peek) {
    out.clear();
    RawPathPeek local{};
    RawPathPeek& p = peek ? *peek : local;
    p = {};
    if (!instance) {
        p.why = RawPathWhy::NoInst;
        return false;
    }
    EnsureQuizFieldOff();
    p.list = ReadPtr(instance, gOffNonRawPosList);
    if (!p.list) {
        p.why = RawPathWhy::NoList;
        return false;
    }
    p.n = ListSize(p.list);
    p.items = ListItems(p.list);
    if (p.n <= 0) {
        p.why = RawPathWhy::Empty;
        return false;
    }
    if (p.n > 2000) {
        p.why = RawPathWhy::TooBig;
        return false;
    }
    if (!p.items) {
        p.why = RawPathWhy::NoItems;
        return false;
    }
    __try {
        const uint8_t* base = reinterpret_cast<uint8_t*>(p.items) + kOffByteArrayData;
        // List<Vector2>：元素 stride=8（两 float）；勿按 Vector2Int/指针读。
        p.cap = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(p.items) + 0x18);
        if (p.cap > 0 && p.n > p.cap) {
            p.why = RawPathWhy::CapMismatch;
            return false;
        }
        out.resize(static_cast<size_t>(p.n));
        for (int i = 0; i < p.n; ++i) {
            const auto* pt =
                reinterpret_cast<const Vec2*>(base + static_cast<size_t>(i) * sizeof(Vec2));
            out[static_cast<size_t>(i)] = *pt;
        }
        p.why = RawPathWhy::Ok;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
        p.why = RawPathWhy::Seh;
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

    // 飞出游戏客户区也当塌缩/废图。
    // BIN 本单（0.1.114 / 03bc3c）：span=311×225 但 121/330 点在客户区外（mid x=1518 >
    // clientRight=1386），旧阈值「32 探针半数越界」过松 → 跟出窗外 → 测谎失败。
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
            const long pad = 24;  // 允许边框/DPI 抖动；勿大到把「半屏在窗外」放过
            // AABB 覆盖全部点：任一点越界即废。题面板永远在客户区内部，正确映射不会贴边，
            // 所以这里不留百分比容忍——旧的「32 探针半数越界」放过了本单 37% 的越界点。
            if (minSx < l - pad || maxSx > r + pad || minSy < t - pad || maxSy > b + pad) {
                return true;
            }
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
                       bool* outHavePanelCorners, MapDiag* outDiag) {
    outScreen.clear();
    if (outHavePanelCorners) *outHavePanelCorners = false;
    if (outDiag) *outDiag = MapDiag{};
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
    const bool pumped = x::runtime::main_thread::InvokeAndWait(&MainJob, &ctx, 8000);
    if (outDiag) {
        // 即便本次映射失败也回填：map-fail 档的 rect / mode 正是事后定位所需。
        outDiag->haveRect = ctx.haveRect;
        outDiag->rectX = ctx.rect.x;
        outDiag->rectY = ctx.rect.y;
        outDiag->rectW = ctx.rect.width;
        outDiag->rectH = ctx.rect.height;
        outDiag->mode = ctx.mapMode ? ctx.mapMode : "none";
        outDiag->panelFromAffine = ctx.havePanelCorners;
        outDiag->verifyMaxErr = ctx.verifyMaxErr;
    }
    if (!pumped) return false;
    if (!ctx.ok || outScreen.size() != localPts.size()) return false;

    if (outPanelCorners4 && ctx.havePanelCorners) {
        std::memcpy(outPanelCorners4, ctx.panelCorners, sizeof(POINT) * 4);
        if (outHavePanelCorners) *outHavePanelCorners = true;
    }

    long spanX = 0, spanY = 0;
    const bool collapsed = IsScreenPathCollapsed(cursorLocal, outScreen, &spanX, &spanY);
    // 诊断：客户区越界计数（与 IsScreenPathCollapsed 同口径 pad=24）
    int outsideN = 0;
    long clientL = 0, clientT = 0, clientR = 0, clientB = 0;
    HWND hwndDiag = x::features::titlebar::win::FindUnityWndClass();
    if (!hwndDiag || !IsWindow(hwndDiag)) hwndDiag = x::features::titlebar::win::FindGameWindow();
    if (hwndDiag && IsWindow(hwndDiag)) {
        RECT cr{};
        POINT origin{};
        if (GetClientRect(hwndDiag, &cr) && ClientToScreen(hwndDiag, &origin)) {
            clientL = origin.x;
            clientT = origin.y;
            clientR = origin.x + (cr.right - cr.left);
            clientB = origin.y + (cr.bottom - cr.top);
            constexpr long kPad = 24;
            for (const POINT& p : outScreen) {
                if (p.x < clientL - kPad || p.x > clientR + kPad || p.y < clientT - kPad ||
                    p.y > clientB + kPad)
                    ++outsideN;
            }
        }
    }
    Log("MapBatch decrypt=%d mode=%s firstRaw=(%.3f,%.3f) firstCursor=(%.1f,%.1f) "
        "screenSpan=(%ld,%ld) collapsed=%d outside=%d/%zu clientLTRB=(%ld,%ld,%ld,%ld) "
        "verifyMaxErr=%.2f panelCorners=%d",
        alreadyPx ? 0 : 1, ctx.mapMode ? ctx.mapMode : "?", localPts[0].x, localPts[0].y,
        cursorLocal[0].x, cursorLocal[0].y, spanX, spanY, collapsed ? 1 : 0, outsideN,
        outScreen.size(), clientL, clientT, clientR, clientB, ctx.verifyMaxErr,
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
    // 焦点日志必须留住：0.1.114 那次客户报的「鼠标跟着轨迹动、但焦点不在游戏窗口」，
    // 判据就是这条的 fg=0。挤在 x.jsonl 里会被高频通道冲掉。
    char buf[220]{};
    snprintf(buf, sizeof(buf), "%s (%s) attach-SFW force=%d set=%d fg=%d", ok ? "ok" : "weak",
             why ? why : "?", force ? 1 : 0, setOk ? 1 : 0, ok ? 1 : 0);
    lie_log::Line("AutoLieFocus", buf);
    return ok;
}

}  // namespace x::features::auto_lie::anti_macro_port
