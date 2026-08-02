// Classic TWMS — shared GameAssembly / IL2CPP export bind.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "il2cpp_bind.h"

#include "managed_main.h"

#include <atomic>

namespace x::runtime::il2cpp {
namespace {

Exports gExp{};
std::atomic<bool> gReady{false};

}  // namespace

bool Ensure() {
    if (gReady.load(std::memory_order_acquire) && gExp.ga && gExp.findAll && gExp.domainGet)
        return true;

    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) return false;

    Exports e{};
    e.ga = ga;
    e.findAll = reinterpret_cast<FnFindAll>(reinterpret_cast<uint8_t*>(ga) + kRvaFindObjectsOfTypeAll);
    e.compGo = reinterpret_cast<FnCompGo>(reinterpret_cast<uint8_t*>(ga) + kRvaCompGetGo);
    e.objName = reinterpret_cast<FnObjName>(reinterpret_cast<uint8_t*>(ga) + kRvaObjGetName);
    e.domainGet = reinterpret_cast<FnDomainGet>(GetProcAddress(ga, "il2cpp_domain_get"));
    e.domainAssemblies =
        reinterpret_cast<FnDomainAssemblies>(GetProcAddress(ga, "il2cpp_domain_get_assemblies"));
    e.asmImage = reinterpret_cast<FnAsmImage>(GetProcAddress(ga, "il2cpp_assembly_get_image"));
    e.classFromName =
        reinterpret_cast<FnClassFromName>(GetProcAddress(ga, "il2cpp_class_from_name"));
    e.classGetType = reinterpret_cast<FnClassGetType>(GetProcAddress(ga, "il2cpp_class_get_type"));
    e.typeGetObject =
        reinterpret_cast<FnTypeGetObject>(GetProcAddress(ga, "il2cpp_type_get_object"));
    e.classGetMethods =
        reinterpret_cast<FnClassGetMethods>(GetProcAddress(ga, "il2cpp_class_get_methods"));
    e.classGetMethodFromName = reinterpret_cast<FnClassGetMethodFromName>(
        GetProcAddress(ga, "il2cpp_class_get_method_from_name"));
    e.classStaticData = reinterpret_cast<FnClassStaticData>(
        GetProcAddress(ga, "il2cpp_class_get_static_field_data"));
    if (!e.classStaticData)
        e.classStaticData =
            reinterpret_cast<FnClassStaticData>(GetProcAddress(ga, "il2cpp_class_get_static_fields"));
    e.classParent = reinterpret_cast<FnClassParent>(GetProcAddress(ga, "il2cpp_class_get_parent"));
    e.runtimeClassInit =
        reinterpret_cast<FnRuntimeClassInit>(GetProcAddress(ga, "il2cpp_runtime_class_init"));
    e.methodGetName =
        reinterpret_cast<FnMethodGetName>(GetProcAddress(ga, "il2cpp_method_get_name"));
    e.stringNew = reinterpret_cast<FnStringNew>(GetProcAddress(ga, "il2cpp_string_new"));
    e.objectGetClass =
        reinterpret_cast<FnObjectGetClass>(GetProcAddress(ga, "il2cpp_object_get_class"));
    // Allocation set — deliberately excluded from the hard check below: only the data-plane
    // element pool needs these, and every other port must keep binding without them.
    e.objectNew = reinterpret_cast<FnObjectNew>(GetProcAddress(ga, "il2cpp_object_new"));
    e.arrayNew = reinterpret_cast<FnArrayNew>(GetProcAddress(ga, "il2cpp_array_new"));
    e.classInstanceSize =
        reinterpret_cast<FnClassInstanceSize>(GetProcAddress(ga, "il2cpp_class_instance_size"));
    e.gcHandleNew = reinterpret_cast<FnGcHandleNew>(GetProcAddress(ga, "il2cpp_gchandle_new"));
    e.gcHandleFree = reinterpret_cast<FnGcHandleFree>(GetProcAddress(ga, "il2cpp_gchandle_free"));
    e.gcHasStrictWbarriers = reinterpret_cast<FnGcHasStrictWbarriers>(
        GetProcAddress(ga, "il2cpp_gc_has_strict_wbarriers"));
    e.gcWbarrierSetField =
        reinterpret_cast<FnGcWbarrierSetField>(GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field"));
    // Soft: shape resolve — missing APIs only disable FindClassByShape, not core bind.
    e.classGetFields =
        reinterpret_cast<FnClassGetFields>(GetProcAddress(ga, "il2cpp_class_get_fields"));
    e.fieldGetOffset =
        reinterpret_cast<FnFieldGetOffset>(GetProcAddress(ga, "il2cpp_field_get_offset"));
    e.fieldGetType = reinterpret_cast<FnFieldGetType>(GetProcAddress(ga, "il2cpp_field_get_type"));
    e.fieldGetFlags =
        reinterpret_cast<FnFieldGetFlags>(GetProcAddress(ga, "il2cpp_field_get_flags"));
    e.typeGetType = reinterpret_cast<FnTypeGetType>(GetProcAddress(ga, "il2cpp_type_get_type"));
    e.classFromType =
        reinterpret_cast<FnClassFromType>(GetProcAddress(ga, "il2cpp_class_from_type"));
    e.typeGetClassOrElement = reinterpret_cast<FnTypeGetClassOrElement>(
        GetProcAddress(ga, "il2cpp_type_get_class_or_element_class"));
    e.classIsValuetype =
        reinterpret_cast<FnClassIsValuetype>(GetProcAddress(ga, "il2cpp_class_is_valuetype"));
    e.classValueSize =
        reinterpret_cast<FnClassValueSize>(GetProcAddress(ga, "il2cpp_class_value_size"));
    e.classGetImage =
        reinterpret_cast<FnClassGetImage>(GetProcAddress(ga, "il2cpp_class_get_image"));
    e.imageGetName = reinterpret_cast<FnImageGetName>(GetProcAddress(ga, "il2cpp_image_get_name"));
    e.imageGetClassCount =
        reinterpret_cast<FnImageGetClassCount>(GetProcAddress(ga, "il2cpp_image_get_class_count"));
    e.imageGetClass =
        reinterpret_cast<FnImageGetClass>(GetProcAddress(ga, "il2cpp_image_get_class"));
    e.methodGetParamCount = reinterpret_cast<FnMethodGetParamCount>(
        GetProcAddress(ga, "il2cpp_method_get_param_count"));
    e.methodGetReturnType = reinterpret_cast<FnMethodGetReturnType>(
        GetProcAddress(ga, "il2cpp_method_get_return_type"));
    e.methodGetParam =
        reinterpret_cast<FnMethodGetParam>(GetProcAddress(ga, "il2cpp_method_get_param"));
    // Soft: Prefab attribute scan.
    e.classHasAttribute =
        reinterpret_cast<FnClassHasAttribute>(GetProcAddress(ga, "il2cpp_class_has_attribute"));
    e.customAttrsFromClass = reinterpret_cast<FnCustomAttrsFromClass>(
        GetProcAddress(ga, "il2cpp_custom_attrs_from_class"));
    e.customAttrsHasAttr =
        reinterpret_cast<FnCustomAttrsHasAttr>(GetProcAddress(ga, "il2cpp_custom_attrs_has_attr"));
    e.customAttrsGetAttr =
        reinterpret_cast<FnCustomAttrsGetAttr>(GetProcAddress(ga, "il2cpp_custom_attrs_get_attr"));
    e.customAttrsFree =
        reinterpret_cast<FnCustomAttrsFree>(GetProcAddress(ga, "il2cpp_custom_attrs_free"));
    e.stringLength = reinterpret_cast<FnStringLength>(GetProcAddress(ga, "il2cpp_string_length"));
    e.stringChars = reinterpret_cast<FnStringChars>(GetProcAddress(ga, "il2cpp_string_chars"));

    if (!e.findAll || !e.domainGet || !e.domainAssemblies || !e.asmImage || !e.classFromName ||
        !e.classGetType || !e.typeGetObject)
        return false;

    gExp = e;
    gReady.store(true, std::memory_order_release);
    return true;
}

const Exports& Get() { return gExp; }

bool ShapeExportsReady() {
    if (!Ensure()) return false;
    return gExp.classGetFields && gExp.fieldGetOffset && gExp.fieldGetType && gExp.typeGetType &&
           gExp.imageGetClassCount && gExp.imageGetClass && gExp.domainGet &&
           gExp.domainAssemblies && gExp.asmImage;
}

bool PrefabExportsReady() {
    if (!Ensure()) return false;
    return ShapeExportsReady() && gExp.classHasAttribute && gExp.customAttrsFromClass &&
           gExp.customAttrsGetAttr && gExp.customAttrsFree && gExp.stringLength && gExp.stringChars;
}

HMODULE GameAssembly() { return Ensure() ? gExp.ga : nullptr; }

uintptr_t GaBase() {
    return Ensure() ? reinterpret_cast<uintptr_t>(gExp.ga) : 0;
}

void* FindClass(const char* ns, const char* name) {
    if (!Ensure() || !name) return nullptr;
    __try {
        void* domain = gExp.domainGet();
        if (!domain) return nullptr;
        size_t n = 0;
        void** asms = reinterpret_cast<void**>(gExp.domainAssemblies(domain, &n));
        if (!asms || n == 0) return nullptr;
        for (size_t i = 0; i < n && i < 512; ++i) {
            void* image = gExp.asmImage(asms[i]);
            if (!image) continue;
            void* klass = gExp.classFromName(image, ns ? ns : "", name);
            if (klass) return klass;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void* ClassTypeObject(void* klass, bool bypassFreeze) {
    if (!Ensure() || !klass || !gExp.classGetType || !gExp.typeGetObject) return nullptr;
    void* type = nullptr;
    __try {
        type = gExp.classGetType(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!type) return nullptr;
    return managed_main::TypeGetObject(gExp.typeGetObject, type, 2000, bypassFreeze);
}

void* ClassTypeObjectOnMain(void* klass) {
    if (!Ensure() || !klass || !gExp.classGetType || !gExp.typeGetObject) return nullptr;
    __try {
        void* type = gExp.classGetType(klass);
        if (!type) return nullptr;
        return gExp.typeGetObject(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* FindClassTypeObjectNs(const char* ns, const char* className, bool bypassFreeze) {
    return ClassTypeObject(FindClass(ns, className), bypassFreeze);
}

void* FindClassTypeObject(const char* className, bool bypassFreeze) {
    return FindClassTypeObjectNs("", className, bypassFreeze);
}

void* TypeObjectFromInstance(void* managedObj) {
    if (!Ensure() || !managedObj || !gExp.objectGetClass) return nullptr;
    void* klass = nullptr;
    __try {
        klass = gExp.objectGetClass(managedObj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return ClassTypeObject(klass);
}

bool LooksLikeHeapPtr(void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL;
}

void* ReadPtr(void* base, size_t off) {
    if (!base) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uintptr_t ArrayLen(void* arr) {
    if (!arr) return 0;
    __try {
        return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* ArrayAt(void* arr, uintptr_t i) {
    if (!arr) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 + i * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace x::runtime::il2cpp
