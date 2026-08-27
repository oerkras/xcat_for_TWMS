#pragma once
// Shared IL2CPP / GameAssembly bind for Classic TWMS (ports + main_thread_pump).
// Callers MUST use this for domain/FindAll/TypeObject; keep only feature-specific RVAs locally.
// FindAll / TypeGetObject still go through managed_main (Unity main thread + login freeze).
// Ensure() only resolves exports — safe from pump BindApis (no managed_main / no cycle).
// Unity 三件套：Ensure 先垫脚裸 RVA，domain 就绪后明文 MI 软升级。
// 日志：Il2CppBind unity managed upgrade OK hits=3/3 … | PARTIAL … still on naked RVA pad。
// 仅 Plain/Hash/Kind 计 hit；纯 Rva 路径不算升级成功。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::runtime::il2cpp {

// UnityEngine.Object / Component — shared across ports (TW Classic dump).
constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E8A380;  // remounted 2026-08-06 Resources.FindObjectsOfTypeAll(Type)
constexpr uint32_t kRvaCompGetGo = 0x4E92760;              // remounted 2026-08-06 Component.get_gameObject
constexpr uint32_t kRvaObjGetName = 0x4E9F6C0;             // remounted 2026-08-06 Object.get_name

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
// Classic TWMS GA：gchandle 是指针宽编码（IDA：free 对 handle & ~0x1FFF 取页），
// 不是老 il2cpp 的 32 位槽号。截成 uint32_t 再 free → 主泵 AV（BIN 04:52 GA+0x3d9523）。
using FnGcHandleNew = uintptr_t (*)(void* obj, bool pinned);
using FnGcHandleFree = void (*)(uintptr_t handle);
using FnGcHasStrictWbarriers = bool (*)();
using FnGcWbarrierSetField = void (*)(void* obj, void** field, void* value);
// Shape resolve (metadata) — soft exports; missing ⇒ hash-only path.
using FnClassGetFields = void* (*)(void* klass, void** iter);
using FnFieldGetOffset = size_t (*)(void* field);
using FnFieldGetName = const char* (*)(void* field);
using FnFieldGetType = void* (*)(void* field);
using FnFieldGetFlags = int (*)(void* field);
using FnClassGetFieldFromName = void* (*)(void* klass, const char* name);
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
// GC 线程登记（soft）——只给文末 GcThreadScope 用，它自持指针、不进 Exports（见那里的注释）。
using FnThreadCurrent = void* (*)();
using FnThreadAttach = void* (*)(void* domain);
using FnThreadDetach = void (*)(void* thread);

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
    FnFieldGetName fieldGetName = nullptr;
    FnFieldGetType fieldGetType = nullptr;
    FnFieldGetFlags fieldGetFlags = nullptr;
    FnClassGetFieldFromName classGetFieldFromName = nullptr;
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

// GC-safe managed-heap guards. Worker-thread objectNew / stringNew /
// runtimeClassInit → "Fatal error in GC: Collecting from unknown thread"
// (the *collecting* thread is unregistered). ONLY safe on the Unity main /
// MainPump drain thread — "pump is ticking" is NOT enough. Off-pump → no-op;
// callers retry via managed_main::Call / InvokeAndWait. ALWAYS use these
// instead of Get().objectNew / stringNew / runtimeClassInit directly.
bool ManagedAllocSafe();
void* AllocObject(void* klass);          // guarded il2cpp_object_new
void* NewString(const char* utf8);       // guarded il2cpp_string_new
bool RuntimeClassInit(void* klass);      // guarded il2cpp_runtime_class_init

// 上面三个守卫只挡显式分配，挡不住 class_from_name / class_get_methods 这类**查询** API
// 内部的隐式分配 —— 装泵前必须在 worker 上做这些查询，绕不开（泵没装好就没有主线程入口）。
// 未向运行时登记的线程上一旦触发回收，GC 扫不到它的栈，Unity 直接 Abort 并写出
// `<游戏名>.gc.log = Collecting from unknown thread`（2026-08-12 23:47:21 冷启动实测：
// 装泵后 167 ms 命中，弹框中止，重开一次即恢复 —— 纯时序赌博）。
//
// 作用域内把本线程登记进 GC，出作用域立刻摘除（不常驻，少一份托管线程表里的痕迹）。
// 泵线程与游戏自有线程本就已登记，构造时探到即整体 no-op：不重复 attach，更不误 detach。
class GcThreadScope {
public:
    // enable=false 时整体 no-op（调用方没有查询要做时别白付一次托管分配）。
    explicit GcThreadScope(bool enable = true);
    ~GcThreadScope();
    GcThreadScope(const GcThreadScope&) = delete;
    GcThreadScope& operator=(const GcThreadScope&) = delete;

    bool Attached() const { return attached_; }  // 本作用域真的做了 attach
    bool Visible() const { return visible_; }    // 线程当前对 GC 可见（原本就在 或 刚 attach）

private:
    // 自持 detach 指针：本作用域刻意不走 Ensure()/Exports —— Ensure() 内部的
    // UpgradeUnityManagedFns 自己就要做托管查询，那正是本作用域要保护的东西，顺序不能反。
    FnThreadDetach detach_ = nullptr;
    void* thread_ = nullptr;
    bool attached_ = false;
    bool visible_ = false;
};

}  // namespace x::runtime::il2cpp
