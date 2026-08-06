// Classic TWMS — method kind resolve (metadata arity / return / param types).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "il2cpp_method.h"

#include "il2cpp_bind.h"

#include <cstring>

namespace x::runtime::il2cpp_method {
namespace {

namespace il2 = x::runtime::il2cpp;

constexpr int kTypeVoid = 0x01;
constexpr int kTypeBoolean = 0x02;
constexpr int kTypeI4 = 0x08;
constexpr int kTypeU4 = 0x09;
constexpr int kTypeClass = 0x12;
constexpr int kTypeString = 0x0e;
constexpr int kTypeObject = 0x1c;
constexpr int kTypeSzArray = 0x1d;
constexpr int kTypeGenericInst = 0x15;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

bool LooksLikeHeapPtr(void* p) { return il2::LooksLikeHeapPtr(p); }

uint32_t PtrToRva(void* p) {
    if (!p) return 0;
    const uintptr_t base = il2::GaBase();
    if (!base) return 0;
    const auto a = reinterpret_cast<uintptr_t>(p);
    if (a < base) return 0;
    const uint64_t d = static_cast<uint64_t>(a - base);
    return d > 0x7FFFFFFFull ? 0u : static_cast<uint32_t>(d);
}

uint32_t MethodRva(const MethodInfoHead* mi) {
    if (!mi) return 0;
    if (const uint32_t r = PtrToRva(mi->methodPointer)) return r;
    // 部分虚方法 methodPointer 为 stub，真身在 virtualMethodPointer
    return PtrToRva(mi->virtualMethodPointer);
}

bool TypeKindMatches(int te, TypeKind want) {
    if (want == TypeKind::Any) return true;
    switch (want) {
        case TypeKind::Void:
            return te == kTypeVoid;
        case TypeKind::Bool:
            return te == kTypeBoolean;
        case TypeKind::I32:
            return te == kTypeI4;
        case TypeKind::U32:
            return te == kTypeU4;
        case TypeKind::Ptr:
            return te == kTypeClass || te == kTypeObject || te == kTypeString || te == kTypeSzArray ||
                   te == kTypeGenericInst;
        default:
            return true;
    }
}

int ReadTypeEnum(void* type) {
    if (!type) return 0;
    const auto& e = il2::Get();
    if (!e.typeGetType) return 0;
    int te = 0;
    __try {
        te = e.typeGetType(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return te;
}

bool RetMatches(void* method, TypeKind want) {
    if (want == TypeKind::Any) return true;
    const auto& e = il2::Get();
    if (!e.methodGetReturnType || !e.typeGetType) return true;  // soft
    void* type = nullptr;
    __try {
        type = e.methodGetReturnType(method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!type) return want == TypeKind::Void;
    return TypeKindMatches(ReadTypeEnum(type), want);
}

bool ParamMatches(void* method, const MethodShape& shape) {
    const auto& e = il2::Get();
    if (!e.methodGetParam || !e.typeGetType) return true;  // soft
    for (int i = 0; i < shape.arity && i < 4; ++i) {
        const TypeKind want = shape.param[i];
        void* wantKlass = shape.paramKlass[i];
        if (want == TypeKind::Any && !wantKlass) continue;
        void* type = nullptr;
        __try {
            type = e.methodGetParam(method, static_cast<uint32_t>(i));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (wantKlass) {
            if (!e.typeGetClassOrElement && !e.classFromType) return true;  // soft
            void* pk = nullptr;
            __try {
                if (e.typeGetClassOrElement)
                    pk = e.typeGetClassOrElement(type);
                else
                    pk = e.classFromType(type);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            if (pk != wantKlass) return false;
            continue;
        }
        if (!TypeKindMatches(ReadTypeEnum(type), want)) return false;
    }
    return true;
}

bool KindMatches(void* method, const MethodShape& shape) {
    if (!method) return false;
    const auto& e = il2::Get();
    if (e.methodGetParamCount) {
        uint32_t n = 0;
        __try {
            n = e.methodGetParamCount(method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (static_cast<int>(n) != shape.arity) return false;
    }
    if (!RetMatches(method, shape.ret)) return false;
    return ParamMatches(method, shape);
}

bool MethodExportsReady() {
    if (!il2::Ensure()) return false;
    const auto& e = il2::Get();
    return e.classGetMethods && e.methodGetParamCount && e.methodGetReturnType && e.typeGetType;
}

}  // namespace

const char* PathName(ResolvePath p) {
    switch (p) {
        case ResolvePath::Rva:
            return "rva";
        case ResolvePath::Kind:
            return "kind";
        case ResolvePath::Hash:
            return "hash";
        case ResolvePath::Plain:
            return "plain";
        default:
            return "MISS";
    }
}

void* FindMethodByName(void* klass, const char* name, int arity, bool walkParents) {
    if (!klass || !name || !name[0] || !il2::Ensure()) return nullptr;
    const auto& e = il2::Get();
    if (e.classGetMethodFromName) {
        void* mi = nullptr;
        __try {
            mi = e.classGetMethodFromName(klass, name, arity);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
        if (mi) {
            auto* head = reinterpret_cast<MethodInfoHead*>(mi);
            if (head->methodPointer || head->virtualMethodPointer) return mi;
        }
    }
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* cur = klass;
    for (int depth = 0; depth < 8 && LooksLikeHeapPtr(cur); ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* raw = e.classGetMethods(cur, &iter);
                if (!raw) break;
                const char* nm = nullptr;
                __try {
                    nm = e.methodGetName(raw);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    nm = nullptr;
                }
                if (!nm || std::strcmp(nm, name) != 0) continue;
                if (e.methodGetParamCount) {
                    uint32_t n = 0;
                    __try {
                        n = e.methodGetParamCount(raw);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        continue;
                    }
                    if (static_cast<int>(n) != arity) continue;
                }
                auto* head = reinterpret_cast<MethodInfoHead*>(raw);
                if (head->methodPointer || head->virtualMethodPointer) return raw;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!walkParents || !e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!LooksLikeHeapPtr(parent) || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

void* FindMethodByRva(void* klass, uint32_t rva, bool walkParents) {
    if (!klass || !rva || !il2::Ensure()) return nullptr;
    const auto& e = il2::Get();
    if (!e.classGetMethods) return nullptr;
    void* cur = klass;
    for (int depth = 0; depth < 8 && LooksLikeHeapPtr(cur); ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                auto* mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethods(cur, &iter));
                if (!mi) break;
                if (MethodRva(mi) == rva) return mi;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!walkParents || !e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!LooksLikeHeapPtr(parent) || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

void* FindMethodByKind(void* klass, const MethodShape& shape) {
    if (!klass || !MethodExportsReady()) return nullptr;
    const auto& e = il2::Get();
    void* hit = nullptr;
    int hits = 0;
    void* cur = klass;
    for (int depth = 0; depth < 8 && LooksLikeHeapPtr(cur); ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* mi = e.classGetMethods(cur, &iter);
                if (!mi) break;
                if (!KindMatches(mi, shape)) continue;
                ++hits;
                hit = mi;
                if (!shape.unique && hits >= 1) return hit;
                if (shape.unique && hits > 1) {
                    hit = nullptr;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (shape.unique && hits > 1) return nullptr;
        if (!shape.walkParents || !e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!LooksLikeHeapPtr(parent) || parent == cur) break;
        cur = parent;
    }
    if (shape.unique) return (hits == 1) ? hit : nullptr;
    return hit;
}

ResolveResult FindMethodCached(void* klass, uint32_t rva, const MethodShape& shape) {
    ResolveResult r{};
    if (!klass) return r;
    void* byRva = rva ? FindMethodByRva(klass, rva, shape.walkParents) : nullptr;
    if (byRva) {
        const bool ok = !MethodExportsReady() || KindMatches(byRva, shape);
        if (ok) {
            r.method = byRva;
            r.path = ResolvePath::Rva;
            return r;
        }
    }
    void* byKind = FindMethodByKind(klass, shape);
    if (byKind) {
        r.method = byKind;
        r.path = ResolvePath::Kind;
        return r;
    }
    if (byRva) {
        r.method = byRva;
        r.path = ResolvePath::Rva;
    }
    return r;
}

ResolveResult FindMethodResolved(void* klass, uint32_t rvaFallback, const MethodShape& shape,
                                 const char* plainName, const char* hashName) {
    ResolveResult r{};
    if (!klass) return r;
    if (hashName && hashName[0]) {
        if (void* mi = FindMethodByName(klass, hashName, shape.arity, shape.walkParents)) {
            if (!MethodExportsReady() || KindMatches(mi, shape)) {
                r.method = mi;
                r.path = ResolvePath::Hash;
                return r;
            }
        }
    }
    if (plainName && plainName[0]) {
        if (void* mi = FindMethodByName(klass, plainName, shape.arity, shape.walkParents)) {
            if (!MethodExportsReady() || KindMatches(mi, shape)) {
                r.method = mi;
                r.path = ResolvePath::Plain;
                return r;
            }
        }
    }
    return FindMethodCached(klass, rvaFallback, shape);
}

}  // namespace x::runtime::il2cpp_method
