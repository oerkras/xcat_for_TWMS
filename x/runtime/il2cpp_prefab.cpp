// Classic TWMS — Prefab attribute string → Il2CppClass (runtime metadata scan).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "il2cpp_prefab.h"

#include "il2cpp_bind.h"
#include "il2cpp_metadata_lock.h"
#include "log.h"

#include <cstring>

namespace x::runtime::il2cpp_prefab {
namespace {

namespace il2 = x::runtime::il2cpp;

// Prefab attribute class (dump TypeDef 934) — _prefabPath @0x10 · remounted 2026-08-06
constexpr char kPrefabAttrClass[] =
    "a59872c698d9039653d477bee4e415348d8ec4d3c6d0d9108f81b3c4e68fb25";
constexpr size_t kOffPrefabPath = 0x10;

bool LooksLikeHeapPtr(void* p) { return il2::LooksLikeHeapPtr(p); }

// 这里会把每个 image 的每个类都过一遍属性，单次扫描就是上万次 il2cpp 调用，是漏锁的
// 高危面：这些 API 内部先拿全局递归元数据锁，而那把锁没有 SEH 清理路径，__except 吞掉
// 异常时展开会跨过解锁代码，锁便永久挂在本线程名下 → 全进程元数据查找挂死 = 黑屏。
// 没漏时是廉价空操作。
void ReturnLeakedMetadataLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
}

bool Utf16EqualsAscii(const wchar_t* w, int n, const char* ascii) {
    if (!w || !ascii || n < 0) return false;
    int i = 0;
    for (; i < n && ascii[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(ascii[i]);
        if (c >= 0x80) return false;
        if (w[i] != static_cast<wchar_t>(c)) return false;
    }
    return ascii[i] == '\0' && i == n;
}

bool PrefabPathEquals(void* attrObj, const char* want) {
    if (!attrObj || !want || !LooksLikeHeapPtr(attrObj)) return false;
    void* str = nullptr;
    __try {
        str = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(attrObj) + kOffPrefabPath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!str || !LooksLikeHeapPtr(str)) return false;
    const auto& e = il2::Get();
    int len = 0;
    const wchar_t* chars = nullptr;
    __try {
        len = e.stringLength(str);
        chars = e.stringChars(str);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return Utf16EqualsAscii(chars, len, want);
}

bool ImageNameOk(const char* name) {
    if (!name || !name[0]) return true;
    return std::strstr(name, "Assembly-CSharp") != nullptr;
}

void* AttrKlass() {
    static void* cached = nullptr;
    if (cached) return cached;
    cached = il2::FindClass("", kPrefabAttrClass);
    return cached;
}

}  // namespace

const char* PathName(ResolvePath p) {
    switch (p) {
        case ResolvePath::Hash:
            return "hash";
        case ResolvePath::Prefab:
            return "prefab";
        default:
            return "MISS";
    }
}

void* FindClassByPrefabName(const char* prefabName) {
    if (!prefabName || !prefabName[0] || !il2::PrefabExportsReady()) return nullptr;
    void* attrKlass = AttrKlass();
    if (!attrKlass) return nullptr;
    const auto& e = il2::Get();

    void* hit = nullptr;
    int hits = 0;

    size_t nAsm = 0;
    void** asms = nullptr;
    __try {
        void* rawAsms = e.domainAssemblies(e.domainGet(), &nAsm);
        asms = static_cast<void**>(rawAsms);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("prefab/domainAssemblies");
        return nullptr;
    }
    if (!asms || nAsm == 0) return nullptr;

    for (size_t ai = 0; ai < nAsm; ++ai) {
        void* image = nullptr;
        __try {
            image = e.asmImage(asms[ai]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("prefab/asmImage");
            continue;
        }
        if (!image) continue;
        const char* iname = nullptr;
        __try {
            iname = e.imageGetName(image);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("prefab/imageGetName");
            iname = nullptr;
        }
        if (!ImageNameOk(iname)) continue;

        size_t nClass = 0;
        __try {
            nClass = e.imageGetClassCount(image);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLeakedMetadataLock("prefab/imageGetClassCount");
            continue;
        }
        for (size_t ci = 0; ci < nClass; ++ci) {
            void* klass = nullptr;
            __try {
                klass = e.imageGetClass(image, ci);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ReturnLeakedMetadataLock("prefab/imageGetClass");
                continue;
            }
            if (!klass) continue;

            bool has = false;
            __try {
                has = e.classHasAttribute(klass, attrKlass);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ReturnLeakedMetadataLock("prefab/classHasAttribute");
                continue;
            }
            if (!has) continue;

            void* ainfo = nullptr;
            void* attrObj = nullptr;
            __try {
                ainfo = e.customAttrsFromClass(klass);
                if (ainfo) attrObj = e.customAttrsGetAttr(ainfo, attrKlass);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ReturnLeakedMetadataLock("prefab/customAttrs");
                attrObj = nullptr;
            }
            const bool match = PrefabPathEquals(attrObj, prefabName);
            if (ainfo) {
                __try {
                    e.customAttrsFree(ainfo);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ReturnLeakedMetadataLock("prefab/customAttrsFree");
                }
            }
            if (!match) continue;
            ++hits;
            hit = klass;
            if (hits > 1) return nullptr;
        }
    }
    return (hits == 1) ? hit : nullptr;
}

ResolveResult FindClassCached(const char* hashName, const char* prefabName) {
    ResolveResult r{};
    if (hashName && hashName[0] && il2::Ensure()) {
        void* byHash = il2::FindClass("", hashName);
        if (byHash) {
            r.klass = byHash;
            r.path = ResolvePath::Hash;
            return r;
        }
    }
    void* byPrefab = FindClassByPrefabName(prefabName);
    if (byPrefab) {
        r.klass = byPrefab;
        r.path = ResolvePath::Prefab;
        x::runtime::LogI("Prefab", "%s via prefab (hash miss)", prefabName ? prefabName : "?");
        return r;
    }
    return r;
}

}  // namespace x::runtime::il2cpp_prefab
