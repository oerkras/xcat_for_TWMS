#pragma once
// Shared IL2CPP / GameAssembly bind for Classic TWMS (ports + main_thread_pump).
// Callers MUST use this for domain/FindAll/TypeObject; keep only feature-specific RVAs locally.
// FindAll / TypeGetObject still go through managed_main (Unity main thread + login freeze).
// Ensure() only resolves exports — safe from pump BindApis (no managed_main / no cycle).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::runtime::il2cpp {

// UnityEngine.Object / Component — shared across ports (TW Classic dump).
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaCompGetGo = 0x4E47E00;  // remapped 2026-08-03
constexpr uint32_t kRvaObjGetName = 0x4E54D60;  // remapped 2026-08-03

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);
using FnDomainGet = void* (*)();
using FnDomainAssemblies = void* (*)(void* domain, size_t* size);
using FnAsmImage = void* (*)(void* assembly);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetType = void* (*)(void* klass);
using FnTypeGetObject = void* (*)(void* type);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnClassGetMethodFromName = void* (*)(void* klass, const char* name, int argc);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnMethodGetName = const char* (*)(const void* method);
using FnStringNew = void* (*)(const char* str);
using FnObjectGetClass = void* (*)(void* obj);
// Managed allocation. Optional — resolve failure only disables the callers that need it.
using FnObjectNew = void* (*)(void* klass);
using FnArrayNew = void* (*)(void* elementKlass, uintptr_t length);
using FnClassInstanceSize = int32_t (*)(void* klass);
using FnGcHandleNew = uint32_t (*)(void* obj, bool pinned);
using FnGcHandleFree = void (*)(uint32_t handle);
using FnGcHasStrictWbarriers = bool (*)();
using FnGcWbarrierSetField = void (*)(void* obj, void** field, void* value);
// Shape resolve (metadata) — soft exports; missing ⇒ hash-only path.
using FnClassGetFields = void* (*)(void* klass, void** iter);
using FnFieldGetOffset = size_t (*)(void* field);
using FnFieldGetType = void* (*)(void* field);
using FnFieldGetFlags = int (*)(void* field);
using FnTypeGetType = int (*)(const void* type);
using FnClassFromType = void* (*)(const void* type);
using FnTypeGetClassOrElement = void* (*)(const void* type);
using FnClassIsValuetype = bool (*)(void* klass);
using FnClassValueSize = int32_t (*)(void* klass, uint32_t* align);
using FnClassGetImage = void* (*)(void* klass);
using FnImageGetName = const char* (*)(void* image);
using FnImageGetClassCount = size_t (*)(void* image);
using FnImageGetClass = void* (*)(void* image, size_t index);
using FnMethodGetParamCount = uint32_t (*)(const void* method);
using FnMethodGetReturnType = void* (*)(const void* method);
using FnMethodGetParam = void* (*)(const void* method, uint32_t index);
// Prefab attribute scan (soft) — missing ⇒ hash-only UI resolve.
using FnClassHasAttribute = bool (*)(void* klass, void* attrKlass);
using FnCustomAttrsFromClass = void* (*)(void* klass);
using FnCustomAttrsHasAttr = bool (*)(void* ainfo, void* attrKlass);
using FnCustomAttrsGetAttr = void* (*)(void* ainfo, void* attrKlass);
using FnCustomAttrsFree = void (*)(void* ainfo);
using FnStringLength = int32_t (*)(void* str);
using FnStringChars = const wchar_t* (*)(void* str);

struct Exports {
    HMODULE ga = nullptr;
    FnFindAll findAll = nullptr;
    FnCompGo compGo = nullptr;
    FnObjName objName = nullptr;
    FnDomainGet domainGet = nullptr;
    FnDomainAssemblies domainAssemblies = nullptr;
    FnAsmImage asmImage = nullptr;
    FnClassFromName classFromName = nullptr;
    FnClassGetType classGetType = nullptr;
    FnTypeGetObject typeGetObject = nullptr;
    FnClassGetMethods classGetMethods = nullptr;
    FnClassGetMethodFromName classGetMethodFromName = nullptr;
    FnClassStaticData classStaticData = nullptr;
    FnClassParent classParent = nullptr;
    FnRuntimeClassInit runtimeClassInit = nullptr;
    FnMethodGetName methodGetName = nullptr;
    FnStringNew stringNew = nullptr;
    FnObjectGetClass objectGetClass = nullptr;
    FnObjectNew objectNew = nullptr;
    FnArrayNew arrayNew = nullptr;
    FnClassInstanceSize classInstanceSize = nullptr;
    FnGcHandleNew gcHandleNew = nullptr;
    FnGcHandleFree gcHandleFree = nullptr;
    FnGcHasStrictWbarriers gcHasStrictWbarriers = nullptr;
    FnGcWbarrierSetField gcWbarrierSetField = nullptr;
    FnClassGetFields classGetFields = nullptr;
    FnFieldGetOffset fieldGetOffset = nullptr;
    FnFieldGetType fieldGetType = nullptr;
    FnFieldGetFlags fieldGetFlags = nullptr;
    FnTypeGetType typeGetType = nullptr;
    FnClassFromType classFromType = nullptr;
    FnTypeGetClassOrElement typeGetClassOrElement = nullptr;
    FnClassIsValuetype classIsValuetype = nullptr;
    FnClassValueSize classValueSize = nullptr;
    FnClassGetImage classGetImage = nullptr;
    FnImageGetName imageGetName = nullptr;
    FnImageGetClassCount imageGetClassCount = nullptr;
    FnImageGetClass imageGetClass = nullptr;
    FnMethodGetParamCount methodGetParamCount = nullptr;
    FnMethodGetReturnType methodGetReturnType = nullptr;
    FnMethodGetParam methodGetParam = nullptr;
    FnClassHasAttribute classHasAttribute = nullptr;
    FnCustomAttrsFromClass customAttrsFromClass = nullptr;
    FnCustomAttrsHasAttr customAttrsHasAttr = nullptr;
    FnCustomAttrsGetAttr customAttrsGetAttr = nullptr;
    FnCustomAttrsFree customAttrsFree = nullptr;
    FnStringLength stringLength = nullptr;
    FnStringChars stringChars = nullptr;
};

// True when Prefab attribute exports resolved.
bool PrefabExportsReady();

// Idempotent. False until GameAssembly.dll is loaded + core exports resolve.
bool Ensure();

const Exports& Get();

HMODULE GameAssembly();
uintptr_t GaBase();

template <typename T>
T AtRva(uint32_t rva) {
    // 0 = BLOCKED / unset remount — never return GA imagebase as a fake entry.
    if (!rva) {
        return nullptr;
    }
    return reinterpret_cast<T>(GaBase() + rva);
}

void* FindClass(const char* ns, const char* name);

// True when field/image exports resolved (shape matching usable).
bool ShapeExportsReady();

// klass → System.Type object (main-thread TypeGetObject via pump).
// bypassFreeze：场景门控（world_port）需在 login-freeze 下仍能取 Type，否则永无法进图解冻。
void* ClassTypeObject(void* klass, bool bypassFreeze = false);

// Already on Unity main thread — direct il2cpp_type_get_object (no nested pump).
// Use inside main_thread jobs (auto_enter / ccu probe); workers must use ClassTypeObject.
void* ClassTypeObjectOnMain(void* klass);

// Convenience: FindClass("", name) then ClassTypeObject.
void* FindClassTypeObject(const char* className, bool bypassFreeze = false);
void* FindClassTypeObjectNs(const char* ns, const char* className, bool bypassFreeze = false);

// managed instance → System.Type (object_get_class + ClassTypeObject / pump).
void* TypeObjectFromInstance(void* managedObj);

bool LooksLikeHeapPtr(void* p);
void* ReadPtr(void* base, size_t off);
uintptr_t ArrayLen(void* arr);
void* ArrayAt(void* arr, uintptr_t i);

}  // namespace x::runtime::il2cpp
