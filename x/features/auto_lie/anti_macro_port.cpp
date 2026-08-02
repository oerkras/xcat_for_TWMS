#include "anti_macro_port.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_prefab.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <cstdarg>
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

// Prefab 类哈希（与 payload_status Cache 灯一致）· remount 2026-08-03
// Util 无 Prefab 属性（纯工具类）；Text/NonFinite 可走 Prefab 串兜底。
constexpr const char* kUtilClass =
    "cf3393383740b1f7566429bf39ecf713c129cbd325ecfcadad676d4eb4a147b";
constexpr const char* kNonFiniteClass =
    "dd7ae5ff8c9cf70a5364a5bda92328a4cf46e9c1a6e2e86aa3c397f0223f891";
constexpr const char* kTextCaptchaClass =
    "d9b7f7be8e623b396e528fe0106aee67628411b92042a05ce8ba380cf31703f";
constexpr const char* kTextCaptchaInfoClass =
    "f8ff8f1277a5432327c24d0e2cb95e60e12f6b7ed483fb538b6c92f73e2b6b5";
constexpr const char* kPrefabNonFinite = "UIAntiMacroNonFinite";
constexpr const char* kPrefabTextCaptcha = "UIAntiMacroTextCaptcha";

void* ResolveQuizKlass(const char* hashName, const char* prefabName) {
    if (prefabName && prefabName[0]) {
        const auto r = x::runtime::il2cpp_prefab::FindClassCached(hashName, prefabName);
        return r.klass;
    }
    return x::runtime::il2cpp::FindClass("", hashName);
}

constexpr uint32_t kRvaIsOpenAntiMacro = 0x9353F0;  // remapped 2026-08-03 Util.IsOpenAntiMacro
constexpr uint32_t kRvaTextGet = 0x932030;  // remapped 2026-08-03 GetAntiMacro
constexpr uint32_t kRvaTextIsInst = 0x9323E0;  // remapped 2026-08-03 IsInstantiated
constexpr uint32_t kRvaTextOnOk = 0x934F10;  // remapped 2026-08-03 OnOk
constexpr uint32_t kRvaNonGet = 0x925360;  // remapped 2026-08-03 GetAntiMacro
constexpr uint32_t kRvaNonIsInst = 0x925750;  // remapped 2026-08-03 IsInstantiated
constexpr uint32_t kRvaTryGetWinCursorPos = 0x9358E0;  // remapped 2026-08-03 Util.TryGetWinCursorPos

// 方法哈希（dump.cs）— static bool()/void() 同形多，哈希主路径
constexpr char kHashIsOpenAntiMacro[] =
    "c05b9f6e84f11cbb916e5280e5281ee7bb0f417db89302ee624d801a8ff2433";
constexpr char kHashTextGet[] =
    "ce47918d4bf1cf69c14fee394aa3c5de903fef82e37ed93362859180028d103";
constexpr char kHashTextIsInst[] =
    "ef32617e0c405247269994305e17258821ef82bd6b88efcd5c67e4f7949a5c7";
constexpr char kHashTextOnOk[] =
    "efdfe46ed7928a99199e8ba6bcc61c27dc76e7def8ccefbb3354c476b5bb635";
constexpr char kHashNonGet[] =
    "af38943e9e15318eb02a47fc3cfcaa3546f825c648a88948335412da2766641";
constexpr char kHashNonIsInst[] =
    "b0539adcfbdf043147dfa887e89615bd691d8368813507465210ac5bf970ad8";
constexpr char kHashTryGetWinCursorPos[] =
    "bea0d3701ecf71150f427b5493ac918025390a36ec9d518d8183cc9ca3d2611";

constexpr size_t kOffTextInputField = 0xB0;
constexpr size_t kOffTextRawImage = 0xA0;
constexpr size_t kOffNonRawImage = 0xA0;
constexpr size_t kOffNonTick = 0xE0;
constexpr size_t kOffNonRawPosList = 0xE8;
constexpr size_t kOffNonMousePosList = 0xF0;
constexpr size_t kOffInfoJpegData = 0x20;
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffTickFrame = 0x10;
constexpr size_t kOffByteArrayData = 0x20;

using FnIsOpen = bool (*)(const void* methodInfo);
using FnGetObj = void* (*)(const void* methodInfo);
using FnIsInst = bool (*)(const void* methodInfo);
using FnVoidSelf = void (*)(void* self, const void* methodInfo);
using FnSetText = void (*)(void* self, void* str, const void* methodInfo);
using FnGetTransform = void* (*)(void* self, const void* methodInfo);
using FnGetTexture = void* (*)(void* self, const void* methodInfo);
// TW Unity：仅有 EncodeToPNG(Texture2D) —— 无 EncodeToJPG。
using FnEncodePng = void* (*)(void* tex, const void* methodInfo);
using FnTryWinCursor = bool (*)(void* rectTf, float ox, float oy, Vec2* out, const void* methodInfo);

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

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plainName, const char* hashName) {
    if (plainName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plainName, shape.arity)) return mi;
    }
    if (hashName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hashName, shape.arity)) return mi;
    }
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            Log("ResolveMi kind hit rva=0x%X plain=%s", rva, plainName ? plainName : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(void* mi, uint32_t rva) {
    if (void* p = MethodPtr(mi)) return reinterpret_cast<Fn>(p);
    return AtRva<Fn>(rva);
}

void EnsureGameMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    void* util = x::runtime::il2cpp::FindClass("", kUtilClass);
    void* text = ResolveQuizKlass(kTextCaptchaClass, kPrefabTextCaptcha);
    void* non = ResolveQuizKlass(kNonFiniteClass, kPrefabNonFinite);
    if (util) {
        // static bool() 不唯一 → 哈希主
        constexpr MethodShape kOpen{0, TypeKind::Bool, false, true, {}};
        if (!gMiIsOpen)
            gMiIsOpen = ResolveMi(util, kRvaIsOpenAntiMacro, kOpen, "IsOpenAntiMacro",
                                  kHashIsOpenAntiMacro);
        // bool(RectTransform, Vector2, out Vector2) — dump 上 RectTransform 首参约 2 处
        constexpr MethodShape kCur{3, TypeKind::Bool, true, true,
                                   {TypeKind::Ptr, TypeKind::Any, TypeKind::Ptr}};
        if (!gMiTryWinCursor)
            gMiTryWinCursor = ResolveMi(util, kRvaTryGetWinCursorPos, kCur, "TryGetWinCursorPos",
                                        kHashTryGetWinCursorPos);
    }
    if (text) {
        constexpr MethodShape kGet{0, TypeKind::Ptr, false, true, {}};
        if (!gMiTextGet)
            gMiTextGet = ResolveMi(text, kRvaTextGet, kGet, "GetAntiMacro", kHashTextGet);
        constexpr MethodShape kInst{0, TypeKind::Bool, false, true, {}};
        if (!gMiTextIsInst)
            gMiTextIsInst =
                ResolveMi(text, kRvaTextIsInst, kInst, "IsInstantiated", kHashTextIsInst);
        constexpr MethodShape kOk{0, TypeKind::Void, false, true, {}};
        if (!gMiTextOnOk)
            gMiTextOnOk = ResolveMi(text, kRvaTextOnOk, kOk, "OnOk", kHashTextOnOk);
    }
    if (non) {
        constexpr MethodShape kGet{0, TypeKind::Ptr, false, true, {}};
        if (!gMiNonGet)
            gMiNonGet = ResolveMi(non, kRvaNonGet, kGet, "GetAntiMacro", kHashNonGet);
        constexpr MethodShape kInst{0, TypeKind::Bool, false, true, {}};
        if (!gMiNonIsInst)
            gMiNonIsInst =
                ResolveMi(non, kRvaNonIsInst, kInst, "IsInstantiated", kHashNonIsInst);
    }
}

void* ResolveMethod(void* klass, const char* name, int argc) {
    auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethodFromName || !klass) return nullptr;
    __try {
        return e.classGetMethodFromName(klass, name, argc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
};

void MainJob(void* user) {
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
        auto& e = x::runtime::il2cpp::Get();
        if (!e.stringNew) return;
        void* str = nullptr;
        __try {
            str = e.stringNew(ctx->answer.c_str());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
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
        auto fn = FnFromMi<FnTryWinCursor>(gMiTryWinCursor, kRvaTryGetWinCursorPos);
        __try {
            ctx->ok = fn(ctx->rectTf, ctx->lx, ctx->ly, &out, gMiTryWinCursor);
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
        auto fn = FnFromMi<FnTryWinCursor>(gMiTryWinCursor, kRvaTryGetWinCursorPos);
        if (!fn) return;
        ctx->screenPts->reserve(ctx->localPts->size());
        int okN = 0;
        for (const auto& lp : *ctx->localPts) {
            Vec2 out{};
            bool hit = false;
            __try {
                hit = fn(ctx->rectTf, lp.x, lp.y, &out, gMiTryWinCursor);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                hit = false;
            }
            POINT pt{};
            if (hit) {
                pt.x = static_cast<LONG>(out.x + 0.5f);
                pt.y = static_cast<LONG>(out.y + 0.5f);
                ++okN;
            }
            ctx->screenPts->push_back(pt);
        }
        ctx->ok = okN > 0 && !ctx->screenPts->empty();
        Log("MapBatch pts=%zu ok=%d", ctx->localPts->size(), okN);
    }
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
    __try {
        return FnFromMi<FnIsOpen>(gMiIsOpen, kRvaIsOpenAntiMacro)(gMiIsOpen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsTextCaptchaOpen() {
    if (!Ensure()) return false;
    __try {
        return FnFromMi<FnIsInst>(gMiTextIsInst, kRvaTextIsInst)(gMiTextIsInst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsNonFiniteOpen() {
    if (!Ensure()) return false;
    __try {
        return FnFromMi<FnIsInst>(gMiNonIsInst, kRvaNonIsInst)(gMiNonIsInst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* GetTextCaptcha() {
    if (!Ensure()) return nullptr;
    __try {
        return FnFromMi<FnGetObj>(gMiTextGet, kRvaTextGet)(gMiTextGet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* GetNonFinite() {
    if (!Ensure()) return nullptr;
    __try {
        return FnFromMi<FnGetObj>(gMiNonGet, kRvaNonGet)(gMiNonGet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
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
    void* list = ReadPtr(instance, kOffNonMousePosList);
    return ListSize(list);
}

bool ReadRawPosList(void* instance, std::vector<Vec2>& out) {
    out.clear();
    if (!instance) return false;
    void* list = ReadPtr(instance, kOffNonRawPosList);
    const int n = ListSize(list);
    void* items = ListItems(list);
    if (!items || n <= 0 || n > 2000) return false;
    __try {
        const uint8_t* base = reinterpret_cast<uint8_t*>(items) + kOffByteArrayData;
        out.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const float* p = reinterpret_cast<const float*>(base + static_cast<size_t>(i) * 8u);
            out[static_cast<size_t>(i)].x = p[0];
            out[static_cast<size_t>(i)].y = p[1];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
        return false;
    }
}

void* ReadNonFiniteTargetRect(void* instance) {
    if (!instance || !ResolveHelpers() || !gMiGetTransform) return nullptr;
    void* rawImage = ReadPtr(instance, kOffNonRawImage);
    if (!rawImage) return nullptr;
    auto getTf = reinterpret_cast<FnGetTransform>(MethodPtr(gMiGetTransform));
    if (!getTf) return nullptr;
    __try {
        return getTf(rawImage, gMiGetTransform);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
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
                       std::vector<POINT>& outScreen) {
    outScreen.clear();
    if (!Ensure() || !rectTransform || localPts.empty()) return false;
    // 330 点映射可能略久；给足超时，避免半成品计划
    MainCtx ctx{};
    ctx.op = MainCtx::Op::MapBatch;
    ctx.rectTf = rectTransform;
    ctx.localPts = &localPts;
    ctx.screenPts = &outScreen;
    if (!x::runtime::main_thread::InvokeAndWait(&MainJob, &ctx, 8000)) return false;
    return ctx.ok && outScreen.size() == localPts.size();
}

bool IsGameForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace x::features::auto_lie::anti_macro_port
