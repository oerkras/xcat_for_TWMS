#pragma once
// Classic TWMS — resolve UI klass by Prefab attribute string (metadata). NOT .text scan.
// dump.cs: [PrefabAttr("UIAntiMacroTextCaptcha", null, null)] class <hash>

namespace x::runtime::il2cpp_prefab {

enum class ResolvePath : unsigned char { Miss = 0, Hash = 1, Prefab = 2 };

struct ResolveResult {
    void* klass = nullptr;
    ResolvePath path = ResolvePath::Miss;
};

// Scan Assembly-CSharp* for unique PrefabAttr._prefabPath == name.
void* FindClassByPrefabName(const char* prefabName);

// Hash FindClass first; on miss / optional validate, Prefab scan fallback.
ResolveResult FindClassCached(const char* hashName, const char* prefabName);

const char* PathName(ResolvePath p);

}  // namespace x::runtime::il2cpp_prefab
