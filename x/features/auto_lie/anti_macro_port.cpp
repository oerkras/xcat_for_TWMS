#include "anti_macro_port.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
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

// Prefab 类哈希（与 payload_status Cache 灯一致）· remount 2026-08-04
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

constexpr uint32_t kRvaIsOpenAntiMacro = 0x937E00;  // remounted 2026-08-04 Util.IsOpenAntiMacro
constexpr uint32_t kRvaTextGet = 0x934920;  // remounted 2026-08-04 GetAntiMacro
constexpr uint32_t kRvaTextIsInst = 0x934D10;  // remounted 2026-08-04 IsInstantiated
constexpr uint32_t kRvaTextOnOk = 0x9378C0;  // remounted 2026-08-04 OnOk
constexpr uint32_t kRvaNonGet = 0x927DB0;  // remounted 2026-08-04 GetAntiMacro
constexpr uint32_t kRvaNonIsInst = 0x928110;  // remounted 2026-08-04 IsInstantiated
constexpr uint32_t kRvaTryGetWinCursorPos = 0x938290;  // remounted 2026-08-04 Util.TryGetWinCursorPos

// 方法哈希（dump.cs）— static bool()/void() 同形多，哈希主路径
constexpr char kHashIsOpenAntiMacro[] =
    "c902c3aee0cd190d1c0817b48f75f871b6d8bb6957bcc899d7748851a61d2ba";
constexpr char kHashTextGet[] =
    "d687a5691943a91180a147112414f2c75a33efaeece4f5133702840b3816748";
constexpr char kHashTextIsInst[] =
    "a8af1b81967cb3b4cacdb1233b02ddbe9aadc8dccf101190483baccc48f6553";
constexpr char kHashTextOnOk[] =
    "abd99c92633b6f9b1806336f4ac0305b15a65910977ea6597f09da1f3eb9836";
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
constexpr size_t kFbInfoJpegData = 0x20;
constexpr size_t kFbTickFrame = 0x10;
size_t gOffTextRawImage = kFbTextRawImage;
size_t gOffTextInputField = kFbTextInputField;
size_t gOffNonRawImage = kFbNonRawImage;
size_t gOffNonTick = kFbNonTick;
size_t gOffNonRawPosList = kFbNonRawPosList;
size_t gOffNonMousePosList = kFbNonMousePosList;
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
        field = nullptr;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    if (FieldOffHit(info, kHashInfoJpegData, kFbInfoJpegData, &gOffInfoJpegData, 0x10, 0x80))
        ++hits;
    if (FieldOffHit(tick, kHashTickFrame, kFbTickFrame, &gOffTickFrame, 0x10, 0x40)) ++hits;
    x::runtime::LogI("AutoLiePort",
                     "quiz fields path=%s hits=%d/8 txtRaw=0x%zX in=0x%zX nonRaw=0x%zX tick=0x%zX "
                     "rawPos=0x%zX mouse=0x%zX jpeg=0x%zX frame=0x%zX",
                     hits == 8 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                     gOffTextRawImage, gOffTextInputField, gOffNonRawImage, gOffNonTick,
                     gOffNonRawPosList, gOffNonMousePosList, gOffInfoJpegData, gOffTickFrame);
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
