#include "payload_control.h"



#include "../../common/xcat_payload_control.h"

#include "../features/auto_enter/auto_enter.h"

#include "../features/fly/fly.h"

#include "../features/invuln/invuln.h"



#ifndef WIN32_LEAN_AND_MEAN

#define WIN32_LEAN_AND_MEAN

#endif

#include <Windows.h>



#include <atomic>

#include <string>



namespace x::ipc {

namespace {



std::atomic<uint64_t> gLastAppliedTick{0};

std::atomic<bool> gHaveApplied{false};



std::string PayloadBinDir() {

    HMODULE self = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |

                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,

                            reinterpret_cast<LPCWSTR>(&PayloadBinDir), &self) ||

        !self)

        return {};

    wchar_t path[MAX_PATH]{};

    if (!GetModuleFileNameW(self, path, MAX_PATH)) return {};

    std::wstring s(path);

    const size_t slash = s.find_last_of(L"\\/");

    if (slash == std::wstring::npos) return {};

    s.resize(slash);

    char narrow[MAX_PATH]{};

    if (!WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr))

        return {};

    return std::string(narrow);

}



void ApplyControl(const xcat::PayloadControl& c) {

    x::features::invuln::SetDesired(c.invuln != 0);

    x::features::fly::SetDesired(c.fly != 0);

    x::features::auto_enter::SetDesired(c.autoEnter != 0, c.worldId, c.worldName, c.charSlot);

    gLastAppliedTick.store(c.writeTickMs);

    gHaveApplied.store(true);

}



bool ReadMergePublish(bool setInvuln, bool invulnOn, bool setFly, bool flyOn) {

    const std::string bin = PayloadBinDir();

    if (bin.empty()) return false;

    xcat::PayloadControl c{};

    (void)xcat::ReadPayloadControl(bin.c_str(), c);

    if (setInvuln) c.invuln = invulnOn ? 1u : 0u;

    if (setFly) c.fly = flyOn ? 1u : 0u;

    c.writeTickMs = GetTickCount64();

    if (!xcat::WritePayloadControl(bin.c_str(), c)) return false;

    gLastAppliedTick.store(c.writeTickMs);

    gHaveApplied.store(true);

    return true;

}



}  // namespace



void PayloadControl_Poll() {

    const std::string bin = PayloadBinDir();

    if (bin.empty()) return;

    xcat::PayloadControl c{};

    if (!xcat::ReadPayloadControl(bin.c_str(), c)) return;

    if (gHaveApplied.load() && c.writeTickMs == gLastAppliedTick.load()) return;

    ApplyControl(c);

}



void PayloadControl_PublishInvuln(bool on) {

    x::features::invuln::SetDesired(on);

    (void)ReadMergePublish(true, on, false, false);

}



void PayloadControl_PublishFly(bool on) {

    (void)ReadMergePublish(false, false, true, on);

}



}  // namespace x::ipc


