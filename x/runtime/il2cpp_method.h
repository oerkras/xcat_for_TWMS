#pragma once
// Classic TWMS — method resolve SSOT（防漂）。
// 顺序：hash 名 → 明文名 → RVA(+kind 校验) → kind 唯一匹配；RVA 仅 fallback。
// NOT .text scan.

#include <cstdint>

namespace x::runtime::il2cpp_method {

enum class TypeKind : uint8_t {
    Any = 0,
    Void,
    Bool,
    I32,
    U32,
    Ptr,  // class / object / string / szarray / genericinst
};

// Back-compat alias used by teleport pilot.
using RetKind = TypeKind;

struct MethodShape {
    int arity = 0;  // il2cpp_method_get_param_count (excludes `this`)
    TypeKind ret = TypeKind::Any;
    bool unique = true;
    bool walkParents = true;
    // Optional per-param filters (index < arity). Any = skip.
    TypeKind param[4]{};
    // Optional exact Il2CppClass* for param i (nullptr = skip). Stronger than TypeKind::Ptr.
    void* paramKlass[4]{};
};

enum class ResolvePath : uint8_t {
    Miss = 0,
    Rva = 1,
    Kind = 2,
    Hash = 3,
    Plain = 4,
};

struct ResolveResult {
    void* method = nullptr;  // MethodInfo*
    ResolvePath path = ResolvePath::Miss;
};

// Scan klass (+ parents) for MethodInfo whose methodPointer RVA matches.
void* FindMethodByRva(void* klass, uint32_t rva, bool walkParents = true);

// Unique arity+return(+param) match (optional).
void* FindMethodByKind(void* klass, const MethodShape& shape);

// Name / hash：classGetMethodFromName，失败再扫 methods（可 walkParents）。
void* FindMethodByName(void* klass, const char* name, int arity, bool walkParents = true);

// RVA first; if found and kind matches (or exports missing), keep.
// Else kind fallback when unique.
ResolveResult FindMethodCached(void* klass, uint32_t rva, const MethodShape& shape);

// 完整防漂：hash → plain → FindMethodCached(rva/kind)。
ResolveResult FindMethodResolved(void* klass, uint32_t rvaFallback, const MethodShape& shape,
                                 const char* plainName, const char* hashName);

const char* PathName(ResolvePath p);

}  // namespace x::runtime::il2cpp_method
