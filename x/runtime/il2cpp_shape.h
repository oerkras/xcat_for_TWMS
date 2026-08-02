#pragma once
// Classic TWMS — IL2CPP metadata shape resolve (NOT .text signature scan).
// Hash FindClass first; on miss / failed layout check, unique field-shape match.

#include <cstddef>
#include <cstdint>

namespace x::runtime::il2cpp_shape {

enum class FieldKind : uint8_t {
    Ptr = 0,              // CLASS / OBJECT / STRING / SZARRAY / GENERICINST(ref)
    I32,                  // I4 / U4 / enum backing
    I64,                  // I8 / U8
    Bool,                 // BOOLEAN
    ValueTypeApprox,      // VALUETYPE; sizeLo..sizeHi inclusive (0 = any)
};

struct FieldShape {
    size_t offset = 0;
    FieldKind kind = FieldKind::Ptr;
    int32_t vtSizeLo = 0;  // ValueTypeApprox only
    int32_t vtSizeHi = 0;
};

struct ClassShape {
    const FieldShape* fields = nullptr;
    size_t fieldCount = 0;
    // Optional: parent must itself match this shape (e.g. UserLocal ⊂ User).
    const ClassShape* parentHint = nullptr;
    int32_t minInstanceSize = 0;
    bool requireParent = false;  // classParent != nullptr
    bool unique = true;          // FindClassByShape: adopt only if hitCount == 1
    // When true, FieldMatches only considers static fields (offsets into static_fields).
    bool staticFields = false;
};

enum class ResolvePath : uint8_t { Miss = 0, Hash = 1, Shape = 2 };

struct ResolveResult {
    void* klass = nullptr;
    ResolvePath path = ResolvePath::Miss;
};

// Full-image scan; unique==true ⇒ require exactly one match.
void* FindClassByShape(const ClassShape& shape);

// Hash first; if found and passes shape (when shape has fields), keep.
// Else shape fallback. Logs once per role via named resolvers.
ResolveResult FindClassCached(const char* hashName, const ClassShape& shape);

// Built-in roles (hashes + shapes from Dumps/runtime/out/dump.cs 2026-08-03).
void* ResolveWorldManagerKlass();
void* ResolveUserLocalKlass();
// Session class (TypeDef 13797 f0ee06b6…) — SendPacket / Close / Disconnect MethodInfo 所在。
// 历史名 NetworkManager：合并后实为 Session；单例壳见 ResolveNetworkManagerFacadeKlass。
void* ResolveNetworkManagerKlass();
// NetworkManager 壳 (TypeDef 13772 df34ff16… : Singleton<>) — Lazy 实例；Session* @0x10。
void* ResolveNetworkManagerFacadeKlass();
void* ResolveSecAttackKlass();  // SecurityClient attack-window static class

// One-shot / path-change self-check log: shape WM=… UL=… NM=… FAC=… SA=…
void LogResolveSelfCheck();

struct RolePaths {
    ResolvePath wm = ResolvePath::Miss;
    ResolvePath ul = ResolvePath::Miss;
    ResolvePath nm = ResolvePath::Miss;
    ResolvePath fac = ResolvePath::Miss;
    ResolvePath sa = ResolvePath::Miss;
};

// 当前缓存路径（未 Resolve 过则为 Miss）；不触发扫描。
RolePaths GetCachedPaths();

const char* PathName(ResolvePath p);

}  // namespace x::runtime::il2cpp_shape
