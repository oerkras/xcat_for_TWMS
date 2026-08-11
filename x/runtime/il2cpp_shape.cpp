// Classic TWMS — metadata shape resolve (field offset + Il2CppTypeEnum).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "il2cpp_shape.h"

#include "il2cpp_bind.h"
#include "log.h"
#include "main_thread_pump.h"

#include <cstring>

namespace x::runtime::il2cpp_shape {
namespace {

namespace il2 = x::runtime::il2cpp;

// Il2CppTypeEnum (Unity IL2CPP) — subset used by FieldKind matching.
constexpr int kTypeBoolean = 0x02;
constexpr int kTypeI4 = 0x08;
constexpr int kTypeU4 = 0x09;
constexpr int kTypeI8 = 0x0a;
constexpr int kTypeU8 = 0x0b;
constexpr int kTypeValuetype = 0x11;
constexpr int kTypeClass = 0x12;
constexpr int kTypeString = 0x0e;
constexpr int kTypeGenericInst = 0x15;
constexpr int kTypeObject = 0x1c;
constexpr int kTypeSzArray = 0x1d;

// remounted 2026-08-06 hashes（08-04 全 miss；交叉：WM+0x28→UL，FAC+0x10→NM）
// TypeDefIndex 不变：WM=1387 UL=1577 SL=1168 FAC=13772 NM=13797 SA=15147；字段形状未漂。
constexpr char kHashWorldManager[] =
    "acda742ab51e7e2e3003fd2b44fbc00eababde4300ef17ac35b5f4fd01bee68";
constexpr char kHashUserLocal[] =
    "d81db6fbb1dc9506e153d6ee92c803ded0eef9dd0bf5c0e2334f2a98cabf4b0";
// Session class (methods); facade singleton is kHashNetworkManagerFacade.
constexpr char kHashNetworkManager[] =
    "db2678aa1194eb7f137182f087dc736bd402274e9e6f3ab3c0fb14a94bdac3b";

// WorldManager dump TypeDef 1387 (2026-08-06): MyUser@0x28, Field@0x58,
// CharacterData@0xA8(hint), SecondaryStat@0xF0（fda0a837…；+0xB8 为嵌套小 struct 勿用）
constexpr FieldShape kWmFields[] = {
    {0x28, FieldKind::Ptr},
    {0x58, FieldKind::Ptr},
    {0xA8, FieldKind::Ptr},
    {0xF0, FieldKind::Ptr},
};
constexpr ClassShape kWmShape = {
    kWmFields,
    sizeof(kWmFields) / sizeof(kWmFields[0]),
    nullptr,
    0x100,
    false,
    true,
    false,
};

// User parent: only need non-null inheritance (UserLocal : User).
constexpr ClassShape kUserParentHint = {nullptr, 0, nullptr, 0, true, false, false};

// Teleport valuetype @0x3C8: bool/bool/Vector2/int/int → ~0x14..0x18 (next field @0x3E0)
constexpr FieldShape kUlFields[] = {
    {0x3C8, FieldKind::ValueTypeApprox, 0x10, 0x20},
};
constexpr ClassShape kUlShape = {
    kUlFields,
    sizeof(kUlFields) / sizeof(kUlFields[0]),
    &kUserParentHint,
    0x3E0,
    true,
    true,
    false,
};

// Session(=merged network session) TypeDef 13797: Socket@0x10, seq u32@0x18, closed bool@0x20,
// buf@0x28, cb@0x30 — MethodInfo 宿主；不是 Singleton 壳。
constexpr FieldShape kNmFields[] = {
    {0x10, FieldKind::Ptr},
    {0x18, FieldKind::I32},
    {0x20, FieldKind::Bool},
    {0x28, FieldKind::Ptr},
    {0x30, FieldKind::Ptr},
};
constexpr ClassShape kNmShape = {
    kNmFields,
    sizeof(kNmFields) / sizeof(kNmFields[0]),
    nullptr,
    0x40,
    false,
    true,
    false,
};

// NetworkManager facade TypeDef 13772 : Singleton<> —
// Session*@0x10, state obj@0x18, Queue@0x28, HashSet@0x48
constexpr char kHashNetworkManagerFacade[] =
    "a36350788113b594c786c663eb1d14a96fcd4c57fb29f8053c6774f6d87c319";
constexpr FieldShape kNmFacadeFields[] = {
    {0x10, FieldKind::Ptr},
    {0x18, FieldKind::Ptr},  // state object（非裸 I32；08-04 dump）
    {0x28, FieldKind::Ptr},
    {0x48, FieldKind::Ptr},
};
constexpr ClassShape kNmFacadeShape = {
    kNmFacadeFields,
    sizeof(kNmFacadeFields) / sizeof(kNmFacadeFields[0]),
    nullptr,
    0x50,
    true,  // : Singleton<>
    true,
    false,
};

// SecurityClient attack window — static class TypeDef 15147
constexpr char kHashSecAttack[] =
    "cf990184167a3debe30b85ee608efab18ffc750676a5f79617009d0f56bec8d";
constexpr FieldShape kSaFields[] = {
    {0x0, FieldKind::Ptr},   // Dictionary<ushort,int>
    {0x8, FieldKind::Ptr},   // Dictionary<int,int>
    {0x10, FieldKind::I32},  // detectTime
};
constexpr ClassShape kSaShape = {
    kSaFields,
    sizeof(kSaFields) / sizeof(kSaFields[0]),
    nullptr,
    0,
    false,
    true,
    true,  // staticFields
};

// SceneLogin TypeDef 1168：GO@0xA0 + 登录 UI 槽@0xC0/C8/D0 + handles List@0xF8 + bool@0x100
constexpr char kHashSceneLogin[] =
    "ae9c5ed19d1cd2635d4a9b233f1011be23a3f74181b2d0dba5ea29832435fa4";
constexpr FieldShape kSlFields[] = {
    {0xA0, FieldKind::Ptr},
    {0xC0, FieldKind::Ptr},
    {0xC8, FieldKind::Ptr},
    {0xD0, FieldKind::Ptr},
    {0xF8, FieldKind::Ptr},
    {0x100, FieldKind::Bool},
};
constexpr ClassShape kSlShape = {
    kSlFields,
    sizeof(kSlFields) / sizeof(kSlFields[0]),
    nullptr,
    0x110,
    true,
    true,
    false,
};

void* gCacheWm = nullptr;
void* gCacheUl = nullptr;
void* gCacheNm = nullptr;
void* gCacheNmFacade = nullptr;
void* gCacheSa = nullptr;
void* gCacheSl = nullptr;
ResolvePath gPathWm = ResolvePath::Miss;
ResolvePath gPathUl = ResolvePath::Miss;
ResolvePath gPathNm = ResolvePath::Miss;
ResolvePath gPathNmFacade = ResolvePath::Miss;
ResolvePath gPathSa = ResolvePath::Miss;
ResolvePath gPathSl = ResolvePath::Miss;
ResolvePath gLoggedWm = ResolvePath::Miss;
ResolvePath gLoggedUl = ResolvePath::Miss;
ResolvePath gLoggedNm = ResolvePath::Miss;
ResolvePath gLoggedNmFacade = ResolvePath::Miss;
ResolvePath gLoggedSa = ResolvePath::Miss;
ResolvePath gLoggedSl = ResolvePath::Miss;
bool gSelfCheckLogged = false;

// CorLib FIELD_ATTRIBUTE_STATIC
constexpr int kFieldAttrStatic = 0x0010;

bool IsPtrTypeEnum(int t) {
    return t == kTypeClass || t == kTypeObject || t == kTypeString || t == kTypeSzArray ||
           t == kTypeGenericInst;
}

int32_t ValueTypeByteSize(void* type) {
    const auto& e = il2::Get();
    if (!type) return -1;
    void* klass = nullptr;
    __try {
        if (e.classFromType)
            klass = e.classFromType(type);
        else if (e.typeGetClassOrElement)
            klass = e.typeGetClassOrElement(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (!klass) return -1;
    if (e.classIsValuetype) {
        bool vt = false;
        __try {
            vt = e.classIsValuetype(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        if (!vt) return -1;
    }
    if (e.classValueSize) {
        uint32_t align = 0;
        int32_t sz = -1;
        __try {
            sz = e.classValueSize(klass, &align);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        return sz;
    }
    if (e.classInstanceSize) {
        int32_t isz = 0;
        __try {
            isz = e.classInstanceSize(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        // instance_size for valuetype klass includes object header on some runtimes — tolerate.
        if (isz >= 16) return isz - 16;
        return isz;
    }
    return -1;
}

bool FieldMatches(void* klass, const FieldShape& want, bool wantStatic) {
    const auto& e = il2::Get();
    if (!klass || !e.classGetFields || !e.fieldGetOffset || !e.fieldGetType || !e.typeGetType)
        return false;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* field = e.classGetFields(klass, &iter);
            if (!field) break;
            bool isStatic = false;
            if (e.fieldGetFlags) {
                int flags = 0;
                __try {
                    flags = e.fieldGetFlags(field);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    flags = 0;
                }
                isStatic = (flags & kFieldAttrStatic) != 0;
            }
            if (wantStatic != isStatic) continue;
            const size_t off = e.fieldGetOffset(field);
            if (off != want.offset) continue;
            void* type = e.fieldGetType(field);
            if (!type) return false;
            const int te = e.typeGetType(type);
            switch (want.kind) {
                case FieldKind::Ptr:
                    return IsPtrTypeEnum(te);
                case FieldKind::I32:
                    // Enums dump as VALUETYPE; accept I4/U4 or small valuetype (~4).
                    if (te == kTypeI4 || te == kTypeU4) return true;
                    if (te == kTypeValuetype) {
                        const int32_t sz = ValueTypeByteSize(type);
                        return sz == 4 || sz < 0;  // unknown size: accept enum-like
                    }
                    return false;
                case FieldKind::I64:
                    return te == kTypeI8 || te == kTypeU8;
                case FieldKind::Bool:
                    return te == kTypeBoolean;
                case FieldKind::ValueTypeApprox: {
                    if (te != kTypeValuetype) return false;
                    const int32_t sz = ValueTypeByteSize(type);
                    if (sz < 0) return want.vtSizeLo == 0 && want.vtSizeHi == 0;
                    if (want.vtSizeLo == 0 && want.vtSizeHi == 0) return true;
                    return sz >= want.vtSizeLo && sz <= want.vtSizeHi;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

bool ClassMatches(void* klass, const ClassShape& shape) {
    if (!klass) return false;
    const auto& e = il2::Get();
    if (shape.minInstanceSize > 0 && e.classInstanceSize) {
        int32_t isz = 0;
        __try {
            isz = e.classInstanceSize(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (isz < shape.minInstanceSize) return false;
    }
    void* parent = nullptr;
    if (e.classParent) {
        __try {
            parent = e.classParent(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
    }
    if (shape.requireParent && !parent) return false;
    if (shape.parentHint) {
        if (!parent) return false;
        if (!ClassMatches(parent, *shape.parentHint)) return false;
    }
    for (size_t i = 0; i < shape.fieldCount; ++i) {
        if (!FieldMatches(klass, shape.fields[i], shape.staticFields)) return false;
    }
    return true;
}

bool ImageNameLooksGame(const char* name) {
    if (!name) return false;
    // Prefer Assembly-CSharp*; still accept others if name empty filter fails.
    return std::strstr(name, "Assembly-CSharp") != nullptr ||
           std::strstr(name, "Assembly-CSharp.dll") != nullptr;
}

}  // namespace

const char* PathName(ResolvePath p) {
    switch (p) {
        case ResolvePath::Hash:
            return "hash";
        case ResolvePath::Shape:
            return "shape";
        default:
            return "MISS";
    }
}

void* FindClassByShape(const ClassShape& shape) {
    if (!il2::ShapeExportsReady()) return nullptr;
    const auto& e = il2::Get();
    void* hit = nullptr;
    int hits = 0;
    __try {
        void* domain = e.domainGet();
        if (!domain) return nullptr;
        size_t nAsm = 0;
        void** asms = reinterpret_cast<void**>(e.domainAssemblies(domain, &nAsm));
        if (!asms || nAsm == 0) return nullptr;
        for (size_t ai = 0; ai < nAsm && ai < 512; ++ai) {
            void* image = e.asmImage(asms[ai]);
            if (!image) continue;
            if (e.imageGetName) {
                const char* iname = nullptr;
                __try {
                    iname = e.imageGetName(image);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    iname = nullptr;
                }
                // Skip obvious Unity/system images when name is available.
                if (iname && !ImageNameLooksGame(iname)) {
                    if (std::strstr(iname, "Unity") || std::strstr(iname, "System") ||
                        std::strstr(iname, "mscorlib") || std::strstr(iname, "netstandard"))
                        continue;
                }
            }
            const size_t nClass = e.imageGetClassCount(image);
            for (size_t ci = 0; ci < nClass; ++ci) {
                void* klass = e.imageGetClass(image, ci);
                if (!klass) continue;
                if (!ClassMatches(klass, shape)) continue;
                ++hits;
                hit = klass;
                if (!shape.unique && hits >= 1) return hit;
                if (shape.unique && hits > 1) return nullptr;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (shape.unique) return (hits == 1) ? hit : nullptr;
    return hit;
}

ResolveResult FindClassCached(const char* hashName, const ClassShape& shape) {
    ResolveResult r{};
    if (!hashName || !hashName[0]) return r;
    void* byHash = il2::FindClass("", hashName);
    if (byHash) {
        const bool ok = (shape.fieldCount == 0 && !shape.requireParent && !shape.parentHint &&
                         shape.minInstanceSize <= 0) ||
                        ClassMatches(byHash, shape) || !il2::ShapeExportsReady();
        if (ok) {
            r.klass = byHash;
            r.path = ResolvePath::Hash;
            return r;
        }
    }
    void* byShape = FindClassByShape(shape);
    if (byShape) {
        r.klass = byShape;
        r.path = ResolvePath::Shape;
        return r;
    }
    // Last resort: keep hash even if shape check failed (exports missing / drift).
    if (byHash) {
        r.klass = byHash;
        r.path = ResolvePath::Hash;
    }
    return r;
}

void* ResolveWorldManagerKlass() {
    if (gCacheWm) return gCacheWm;
    // 仅在泵已装好时才跳泵：InstallPump 自身会冷调本函数；此时 InvokeAndWait→Ensure
    // 会死锁（BIN 10:18：settle 后无 MainPump 日志 → 守护 ProcessDead 干净重拉）。
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { (void)ResolveWorldManagerKlass(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return gCacheWm;
    }
    const ResolveResult r = FindClassCached(kHashWorldManager, kWmShape);
    gCacheWm = r.klass;
    gPathWm = r.path;
    return gCacheWm;
}

void* ResolveUserLocalKlass() {
    if (gCacheUl) return gCacheUl;
    // LogResolveSelfCheck / Titlebar GetWM 会在 LOGIN worker 上冷解 UL=shape（BIN 11:56）。
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { (void)ResolveUserLocalKlass(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return gCacheUl;
    }
    const ResolveResult r = FindClassCached(kHashUserLocal, kUlShape);
    gCacheUl = r.klass;
    gPathUl = r.path;
    return gCacheUl;
}

void* ResolveNetworkManagerKlass() {
    if (gCacheNm) return gCacheNm;
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { (void)ResolveNetworkManagerKlass(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return gCacheNm;
    }
    const ResolveResult r = FindClassCached(kHashNetworkManager, kNmShape);
    gCacheNm = r.klass;
    gPathNm = r.path;
    return gCacheNm;
}

void* ResolveNetworkManagerFacadeKlass() {
    if (gCacheNmFacade) return gCacheNmFacade;
    if (!x::runtime::main_thread::IsOnPumpThread() &&
        x::runtime::main_thread::IsInstalled()) {
        x::runtime::main_thread::InvokeAndWait(
            [](void*) { (void)ResolveNetworkManagerFacadeKlass(); }, nullptr, 2500,
            x::runtime::main_thread::JobPrio::High);
        return gCacheNmFacade;
    }
    const ResolveResult r = FindClassCached(kHashNetworkManagerFacade, kNmFacadeShape);
    gCacheNmFacade = r.klass;
    gPathNmFacade = r.path;
    return gCacheNmFacade;
}

void* ResolveSecAttackKlass() {
    if (gCacheSa) return gCacheSa;
    const ResolveResult r = FindClassCached(kHashSecAttack, kSaShape);
    gCacheSa = r.klass;
    gPathSa = r.path;
    return gCacheSa;
}

void* ResolveSceneLoginKlass() {
    if (gCacheSl) return gCacheSl;
    const ResolveResult r = FindClassCached(kHashSceneLogin, kSlShape);
    gCacheSl = r.klass;
    gPathSl = r.path;
    if (!gCacheSl) {
        // Plain restored name (rare dumps); not cached as hash path.
        gCacheSl = il2::FindClass("", "SceneLogin");
        if (gCacheSl) gPathSl = ResolvePath::Hash;
    }
    return gCacheSl;
}

void LogResolveSelfCheck() {
    if (!il2::Ensure()) return;
    static bool sExportsWarned = false;
    if (!il2::ShapeExportsReady() && !sExportsWarned) {
        sExportsWarned = true;
        x::runtime::LogW("shape", "field/image exports missing — hash-only");
    }
    (void)ResolveWorldManagerKlass();
    (void)ResolveUserLocalKlass();
    (void)ResolveNetworkManagerKlass();
    (void)ResolveNetworkManagerFacadeKlass();
    (void)ResolveSecAttackKlass();
    (void)ResolveSceneLoginKlass();
    const bool changed = !gSelfCheckLogged || gLoggedWm != gPathWm || gLoggedUl != gPathUl ||
                         gLoggedNm != gPathNm || gLoggedNmFacade != gPathNmFacade ||
                         gLoggedSa != gPathSa || gLoggedSl != gPathSl;
    if (!changed) return;
    gSelfCheckLogged = true;
    gLoggedWm = gPathWm;
    gLoggedUl = gPathUl;
    gLoggedNm = gPathNm;
    gLoggedNmFacade = gPathNmFacade;
    gLoggedSa = gPathSa;
    gLoggedSl = gPathSl;
    x::runtime::LogI("shape", "WM=%s UL=%s NM=%s FAC=%s SA=%s SL=%s", PathName(gPathWm),
                     PathName(gPathUl), PathName(gPathNm), PathName(gPathNmFacade), PathName(gPathSa),
                     PathName(gPathSl));
}

RolePaths GetCachedPaths() {
    RolePaths p{};
    p.wm = gPathWm;
    p.ul = gPathUl;
    p.nm = gPathNm;
    p.fac = gPathNmFacade;
    p.sa = gPathSa;
    p.sl = gPathSl;
    return p;
}

}  // namespace x::runtime::il2cpp_shape
