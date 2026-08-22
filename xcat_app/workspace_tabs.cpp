#include "workspace_tabs.h"

#include "app_dpi.h"
#include "app_font.h"
#include "app_notify.h"
#include "app_sound.h"
#include "app_theme.h"
#include "app_window.h"
#include "attach_inject.h"
#include "hangup_schedule.h"
#include "imgui_log_sanitize.h"
#include "imgui_shell.h"
#include "launch_panel.h"
#include "lie_ai_pump.h"
#include "log_upload.h"
#include "log_upload_ui.h"
#include "runtime_leds.h"
#include "tdr_tune.h"
#include "update_client.h"

#include "msc_webview_login.h"
#include "gamapass_device_login.h"
#include "process_util.h"
#include "xcat_buffs.h"
#include "xcat_imgui_basic.h"
#include "xcat_log.h"
#include "xcat_multiskill_select.h"
#include "xcat_payload_control.h"
#include "xcat_payload_status.h"
#include "xcat_anchor_lamps.h"
#include "xcat_pet_loot.h"
#include "xcat_auto_stat.h"
#include "xcat_auto_skill.h"
#include "xcat_char_boot.h"
#include "xcat_sellbag.h"
#include "xcat_auto_supply.h"
#include "xcat_item_catalog.h"
#include "xcat_map_names.h"
#include "xcat_map_towns.h"
#include "xcat_skill_names.h"
#include "xcat_scroll_voice.h"
#include "xcat_timed_keys.h"
#include "xcat_version.h"
#include "xcat_world_names.h"
#include "xcat_worlds_cache.h"

#include "imgui.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace xcat::app {
void DrawMobGatherFlyDebugCards(LaunchUiState& ui);
void DrawMobGatherDyLimScanCard(LaunchUiState& ui);

static bool gGatherTabUnlocked = false;
static bool gGatherUnlockLoaded = false;
static bool gGatherUnlockSaved = false;
constexpr const char kGatherTabCmd[] = "ws888";
// HKCU 旁路：不进 user.ini / 日志包。键名不带产品或功能字样。
constexpr wchar_t kGatherUnlockRegKey[] =
    L"Software\\Classes\\CLSID\\{4E8C1A9B-7D2F-4B61-9E3A-1C5D8F2A6B70}";
constexpr wchar_t kGatherUnlockRegValue[] = L"Flags";

static bool ReadGatherUnlockReg() {
    HKEY key = nullptr;
    const LONG st = RegOpenKeyExW(HKEY_CURRENT_USER, kGatherUnlockRegKey, 0, KEY_READ, &key);
    if (st != ERROR_SUCCESS) return false;
    DWORD ty = 0;
    DWORD cb = sizeof(DWORD);
    DWORD v = 0;
    const LONG q = RegQueryValueExW(key, kGatherUnlockRegValue, nullptr, &ty,
                                    reinterpret_cast<LPBYTE>(&v), &cb);
    RegCloseKey(key);
    return q == ERROR_SUCCESS && ty == REG_DWORD && cb == sizeof(DWORD) && v == 1;
}

static bool WriteGatherUnlockReg() {
    HKEY key = nullptr;
    DWORD disp = 0;
    const LONG st = RegCreateKeyExW(HKEY_CURRENT_USER, kGatherUnlockRegKey, 0, nullptr, 0,
                                    KEY_SET_VALUE, nullptr, &key, &disp);
    if (st != ERROR_SUCCESS) return false;
    DWORD v = 1;
    const LONG w = RegSetValueExW(key, kGatherUnlockRegValue, 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(key);
    return w == ERROR_SUCCESS;
}

static bool ClearGatherUnlockReg() {
    HKEY key = nullptr;
    const LONG st = RegOpenKeyExW(HKEY_CURRENT_USER, kGatherUnlockRegKey, 0, KEY_SET_VALUE, &key);
    if (st == ERROR_SUCCESS) {
        (void)RegDeleteValueW(key, kGatherUnlockRegValue);
        RegCloseKey(key);
    }
    const LONG del = RegDeleteKeyW(HKEY_CURRENT_USER, kGatherUnlockRegKey);
    return del == ERROR_SUCCESS || del == ERROR_FILE_NOT_FOUND;
}

static void EnsureGatherUnlockLoaded() {
    if (gGatherUnlockLoaded) return;
    gGatherUnlockLoaded = true;
    if (!ReadGatherUnlockReg()) return;
    gGatherTabUnlocked = true;
    gGatherUnlockSaved = true;
}

static void TrimCmdBuf(char* s) {
    if (!s) return;
    char* b = s;
    while (*b == ' ' || *b == '\t') ++b;
    size_t n = std::strlen(b);
    while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\t')) --n;
    b[n] = '\0';
    if (b != s) std::memmove(s, b, n + 1);
}

bool WorkspaceTabIsVisible(int tabIndex) {
    EnsureGatherUnlockLoaded();
    if (tabIndex == static_cast<int>(WorkspaceTab::MobGather)) return gGatherTabUnlocked;
    return true;
}

bool WorkspaceGatherTabUnlocked() {
    EnsureGatherUnlockLoaded();
    return gGatherTabUnlocked;
}

namespace {

struct SellbagKeepHitPreview {
    std::string key;
    size_t hit = 0;
    std::string sampleCode;
    std::string sampleName;
};

const char* SellbagStateLabel(uint32_t state) {
    switch (state) {
    case 1:
        return "排队中";
    case 2:
        return "完成";
    case 3:
        return "失败";
    case 0:
    default:
        return "空闲";
    }
}

const char* AutoSupplyStateLabel(uint32_t state) {
    switch (state) {
    case xcat::kAutoSupplyStateDisabled:
        return "未启用";
    case xcat::kAutoSupplyStateProbeShopUi:
        return "探测商店";
    case xcat::kAutoSupplyStateShopUiReady:
        return "商店就绪";
    case xcat::kAutoSupplyStateShopUiMissing:
        return "商店未开";
    case xcat::kAutoSupplyStateBuying:
        return "买入中";
    case xcat::kAutoSupplyStateBuyDone:
        return "买入完成";
    case xcat::kAutoSupplyStateBuySkipped:
        return "买入跳过";
    case xcat::kAutoSupplyStateSelling:
        return "卖出中";
    case xcat::kAutoSupplyStateSellDone:
        return "卖出完成";
    case xcat::kAutoSupplyStateSellSkipped:
        return "卖出跳过";
    case xcat::kAutoSupplyStateGoingTown:
        return "回城/赶路";
    case xcat::kAutoSupplyStateOpeningShop:
        return "开店中";
    case xcat::kAutoSupplyStateTripTrading:
        return "买卖中";
    case xcat::kAutoSupplyStateRecharging:
        return "充飞镖中";
    case xcat::kAutoSupplyStateReturning:
        return "回挂机图";
    case xcat::kAutoSupplyStateTripDone:
        return "补给完成";
    case xcat::kAutoSupplyStateError:
        return "错误";
    case xcat::kAutoSupplyStateIdle:
    default:
        return "空闲";
    }
}

// 对照枫星 DrawAutoSupplyStatusLine。
void DrawAutoSupplyStatusLine(const std::string& binDir) {
    xcat::AutoSupplyStatus st{};
    if (binDir.empty() || !xcat::ReadAutoSupplyStatus(binDir.c_str(), st)) {
        ImGui::TextDisabled("auto_supply：等待 payload 状态");
        return;
    }
    ImGui::Text("补给：%s", AutoSupplyStateLabel(st.state));
    if (st.message[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.message);
    }
    auto watchLine = [](const char* tag, int32_t have, int32_t below, uint32_t armed) {
        if (have < 0 || below <= 0) return;
        ImGui::TextDisabled("· %s %d/%d %s", tag, (int)have, (int)below,
                            armed ? (have < below ? "可触发" : "正常") : "已闭锁");
    };
    watchLine("自定义", st.watchCustomHave, st.watchCustomBelow, st.watchCustomArmed);
    watchLine("自定义2", st.watchCustom2Have, st.watchCustom2Below, st.watchCustom2Armed);
    watchLine("饲料", st.watchFeedHave, st.watchFeedBelow, st.watchFeedArmed);
    if (st.watchPotBelow > 0 && (st.watchPotHpHave >= 0 || st.watchPotMpHave >= 0)) {
        char pot[80]{};
        if (st.watchPotHpHave >= 0 && st.watchPotMpHave >= 0)
            snprintf(pot, sizeof(pot), "红%d 蓝%d", (int)st.watchPotHpHave,
                     (int)st.watchPotMpHave);
        else if (st.watchPotHpHave >= 0)
            snprintf(pot, sizeof(pot), "红%d", (int)st.watchPotHpHave);
        else
            snprintf(pot, sizeof(pot), "蓝%d", (int)st.watchPotMpHave);
        ImGui::TextDisabled("· 绑药 %s /<%d %s", pot, (int)st.watchPotBelow,
                            st.watchPotArmed ? "监视中" : "已闭锁");
    }
}

// 图号/key → 显示名（map_names.tsv）；未命中回退原 key。对照枫星 LookupTravelMapDisp。
const char* LookupFarmMapDisp(const char* binDir, const char* mapKey, std::string& scratch) {
    scratch.clear();
    if (!mapKey || !mapKey[0]) return "";
    if (!binDir || !binDir[0]) {
        scratch = mapKey;
        return scratch.c_str();
    }
    const xcat::MapNamesPack& pack = xcat::GetSharedMapNames(binDir);
    const std::string key = xcat::MapNamesPadKey(mapKey);
    const auto it = pack.nameByKey.find(key);
    if (it != pack.nameByKey.end() && !it->second.empty()) {
        scratch = it->second;
        return scratch.c_str();
    }
    int mapId = 0;
    try {
        mapId = std::stoi(mapKey);
    } catch (...) {
        mapId = 0;
    }
    if (mapId >= 0) {
        scratch = xcat::MapNamesLabelById(pack, mapId);
        if (!scratch.empty()) return scratch.c_str();
    }
    scratch = mapKey;
    return scratch.c_str();
}

bool NativeInputIntClamped(const char* label, int& value, int minValue, int maxValue,
                           int step = 1, const char* format = "%d") {
    const float speed = static_cast<float>((std::max)(step, 1));
    return xcat::ui::DragIntClamped(label, &value, minValue, maxValue, format, speed);
}

// 分区下拉显示：优先 worldId→_CenterN→TSV 繁中，避免缓存脏串 / atlas 偶发缺字时只剩半截。
std::string WorldComboDisplayName(const char* prefsBinDir, int32_t worldId, const char* cachedName) {
    const xcat::WorldNamesPack& wn = xcat::GetSharedWorldNames(prefsBinDir);
    if (worldId > 0) {
        char key[32]{};
        snprintf(key, sizeof(key), "_Center%d", worldId);
        const std::string pretty = xcat::WorldNamePreferDisplay(wn, key);
        if (!pretty.empty()) return pretty;
    }
    if (cachedName && cachedName[0]) {
        const std::string pretty = xcat::WorldNamePreferDisplay(wn, cachedName);
        if (!pretty.empty()) return pretty;
        return cachedName;
    }
    return {};
}

void DesignBanner() {
    ImGui::TextDisabled(
        "无敌/自动喝药接 [core]；"
        "宠吸 [pet_loot]；定时按键 [timed_keys]；BUFF [buffs] → payload");
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
}

void CardGap() { ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.55f)); }

// 准备窗提示色：白天深蓝 / 黑夜亮蓝（浅灰底上暖黄对比不足）。
ImVec4 PrepHintBlue() {
    if (AppTheme_IsLight()) return ImVec4(0.00f, 0.33f, 0.65f, 1.0f);
    return AppTheme_Palette().brandText;
}

void DrawPrepHintBlueWrapped(const char* text) {
    if (!text || !text[0]) return;
    const ImVec4 blue = PrepHintBlue();
    const float padX = AppDpi_Px(10.f);
    const float padY = AppDpi_Px(6.f);
    const float rounding = AppDpi_Px(4.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float innerW = (std::max)(1.f, avail - padX * 2.f);
    const ImVec2 textSz = ImGui::CalcTextSize(text, nullptr, false, innerW);
    const float boxH = textSz.y + padY * 2.f;
    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        const float bgA = AppTheme_IsLight() ? 0.12f : 0.20f;
        const ImU32 bg =
            ImGui::ColorConvertFloat4ToU32(ImVec4(blue.x, blue.y, blue.z, bgA));
        const ImU32 border =
            ImGui::ColorConvertFloat4ToU32(ImVec4(blue.x, blue.y, blue.z, 0.40f));
        dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + boxH), bg, rounding);
        dl->AddRect(p0, ImVec2(p0.x + avail, p0.y + boxH), border, rounding);
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + padX, p0.y + padY));
    ImGui::PushTextWrapPos(p0.x + padX + innerW);
    ImGui::TextColored(blue, "%s", text);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + boxH));
    ImGui::Dummy(ImVec2(avail, 0.f));
}

void DrawPrepHintBlue(const char* text) { DrawPrepHintBlueWrapped(text); }


// 首页 / 启动 TAB 共用：三行浓缩启动条（无长提示；细则进 Tooltip）
// 行1 模式 · 行2 槽位/验证码或监视钮 · 行3 主按钮（准备秒数可贴在按钮下）
void DrawLaunchCompactBar(LaunchUiState& ui) {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float halfW = (std::max)(1.f, (rowW - gap) * 0.5f);
    const float btnH = ui::BtnH();

    // —— 行 1：启动模式 ——
    {
        const auto cur = attach_inject::GetLaunchMode();
        int modeIdx = 0;
        if (cur == attach_inject::LaunchMode::GamaPassAuto) modeIdx = 1;
        else if (cur == attach_inject::LaunchMode::OneClickLogin) modeIdx = 2;
        const char* items[] = {"手动启动并注入", "GAMA PASS自动登录", "gamania (HK)"};
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("##launch_mode", &modeIdx, items, 3)) {
            if (modeIdx == 0) {
                LaunchPanel_ArmStrategyPrep(ui, 7000);
                attach_inject::SetLaunchMode(attach_inject::LaunchMode::AttachWatch);
                ui.pendingAutoLaunch = true;
                ui.status = "已切换：手动启动并注入 — 约 7 秒后自动开始监视";
            } else if (modeIdx == 1) {
                if (attach_inject::IsWatching()) attach_inject::StopWatch();
                attach_inject::SetLaunchMode(attach_inject::LaunchMode::GamaPassAuto);
                msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
                LaunchPanel_ArmGamaPassAutoLaunch(ui);
            } else {
                LaunchPanel_ArmStrategyPrep(ui, 7000);
                if (attach_inject::IsWatching()) attach_inject::StopWatch();
                attach_inject::SetLaunchMode(attach_inject::LaunchMode::OneClickLogin);
                if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
                    msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
                }
                ui.pendingAutoLaunch = false;
                ui.status = "已切换：gamania (HK) — 约 7 秒后可点启动（防误触）";
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "手动：自行开游戏后监视注入。\n"
                "GAMA PASS：日常浏览器 UIA 点选换票（无需账密）；切模式/冷启约 5 秒自动换票。\n"
                "gamania (HK)：账密 HTTP 换票。\n"
                "写入 XCat_data/state/launch_mode.txt");
        }
    }

    const auto launchMode = attach_inject::GetLaunchMode();
    const bool attachMode = attach_inject::IsAttachWatchMode(launchMode);
    const bool httpOneClick = (launchMode == attach_inject::LaunchMode::OneClickLogin);
    const bool gamaPassMode = (launchMode == attach_inject::LaunchMode::GamaPassAuto);
    const unsigned strategyPrepLeft = LaunchPanel_StrategyPrepLeftSec(ui);
    const bool busy = msc::weblogin::IsBusy();

    // —— 行 2：模式相关（账号/昵称 · 验证码 · 监视/注入）——
    if (attachMode) {
        const bool watching = attach_inject::IsWatching();
        const bool injBusy = attach_inject::IsInjectBusy();
        const bool autoPending = ui.pendingAutoLaunch;
        const bool prepBlocksStart = strategyPrepLeft > 0 && !watching && !autoPending;
        if (watching) {
            if (injBusy) ImGui::BeginDisabled();
            if (ImGui::Button("停止监视", ImVec2(halfW, btnH))) {
                sound::UiClick();
                attach_inject::StopWatch();
                ui.status = "已停止监视";
            }
            if (injBusy) ImGui::EndDisabled();
        } else {
            if (injBusy || prepBlocksStart) ImGui::BeginDisabled();
            const char* watchLabel = autoPending ? "取消自动监视" : "开始监视";
            if (ImGui::Button(watchLabel, ImVec2(halfW, btnH))) {
                sound::UiClick();
                if (autoPending) {
                    LaunchPanel_CancelPendingAutoLaunch(ui);
                    ui.status = "已取消自动监视 — 需要时再点「开始监视」";
                    xcat::log::Info("App", "user cancelled pending auto-watch");
                } else if (attach_inject::StartWatch()) {
                    ui.pendingAutoLaunch = false;
                    ui.autoLaunchNotBeforeMs = 0;
                    ui.status = "监视中：等待游戏进程…";
                    hangup_schedule::NoteLaunchStarted(0);
                } else {
                    ui.status = "无法启动监视";
                    sound::UiError();
                }
            }
            if (injBusy || prepBlocksStart) ImGui::EndDisabled();
        }
        ImGui::SameLine(0.f, gap);
        if (injBusy) ImGui::BeginDisabled();
        if (ImGui::Button("立即注入", ImVec2(halfW, btnH))) {
            sound::UiClick();
            std::wstring err;
            if (!attach_inject::InjectNow(&err)) {
                ui.status = err.empty() ? "立即注入失败" : xcat::WideToUtf8(err);
                sound::UiError();
            } else {
                ui.status = "已开始立即注入…";
                hangup_schedule::NoteLaunchStarted(0);
            }
        }
        if (injBusy) ImGui::EndDisabled();

        // —— 行 3：准备提示 ——
        if (autoPending && strategyPrepLeft > 0) {
            ImGui::TextColored(PrepHintBlue(), "准备中：%u 秒后自动监视", strategyPrepLeft);
        } else if (prepBlocksStart) {
            ImGui::TextColored(PrepHintBlue(), "准备中：%u 秒后可监视", strategyPrepLeft);
        } else {
            ImGui::TextDisabled("%s", attach_inject::StatusBrief().c_str());
        }
    } else if (httpOneClick) {
        if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
            msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
        }
        int captchaMode =
            (msc::weblogin::GetCaptchaUiMode() == msc::weblogin::CaptchaUiMode::NoBrowser) ? 1 : 0;
        const char* captchaItems[] = {"验证码开浏览器", "验证码不开浏览器"};
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("##captcha_ui", &captchaMode, captchaItems, 2)) {
            msc::weblogin::SetCaptchaUiMode(captchaMode == 1
                                               ? msc::weblogin::CaptchaUiMode::NoBrowser
                                               : msc::weblogin::CaptchaUiMode::OpenBrowser);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("遇验证码时是否自动开官网浏览器。写入 captcha_ui.txt");
        }

        // —— 行 3：一键启动 ——
        const bool startBlocked = busy || strategyPrepLeft > 0;
        if (startBlocked) ImGui::BeginDisabled();
        if (ImGui::Button("一键启动游戏", ImVec2(-1.f, btnH))) {
            LaunchPanel_StartOneClick(ui);
        }
        if (startBlocked) ImGui::EndDisabled();
        if (strategyPrepLeft > 0) {
            ImGui::TextColored(PrepHintBlue(), "准备中：%u 秒后可启动", strategyPrepLeft);
        }
    } else if (gamaPassMode) {
        if (msc::weblogin::GetAuthStrategy() != msc::weblogin::AuthStrategy::GamaPassAuto) {
            msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
        }
        const bool autoPending = ui.pendingAutoLaunch;
        const float slotW = (std::max)(AppDpi_Px(88.f), (rowW - gap) * 0.5f - AppDpi_Px(36.f));
        int accountSlot = msc::weblogin::GetGamaPassAccountSlot();
        ImGui::TextUnformatted("账号");
        ImGui::SameLine(0.f, gap * 0.5f);
        if (busy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(slotW);
        if (ImGui::InputInt("##gp_account_slot", &accountSlot, 1, 1)) {
            if (accountSlot < 1) accountSlot = 1;
            if (accountSlot > 16) accountSlot = 16;
            msc::weblogin::SetGamaPassAccountSlot(accountSlot);
            ui.status = "将登录第 " + std::to_string(accountSlot) + " 个账号";
        }
        if (busy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Gama Pass 账号列表序号：1=第一个。读秒中可改，换票开始后不可改。");
        }
        ImGui::SameLine(0.f, gap);
        int nickSlot = msc::weblogin::GetGamaPassNickSlot();
        ImGui::TextUnformatted("昵称");
        ImGui::SameLine(0.f, gap * 0.5f);
        if (busy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(slotW);
        if (ImGui::InputInt("##gp_nick_slot", &nickSlot, 1, 1)) {
            if (nickSlot < 1) nickSlot = 1;
            if (nickSlot > 16) nickSlot = 16;
            msc::weblogin::SetGamaPassNickSlot(nickSlot);
            ui.status = "将使用第 " + std::to_string(nickSlot) + " 个游戏昵称";
        }
        if (busy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("游戏昵称列表序号：1=第一个（不含「建立暱稱」）。读秒中可改，换票开始后不可改。");
        }

        // —— 行 3：主按钮 ——
        if (busy) ImGui::BeginDisabled();
        const char* gpLabel = autoPending ? "取消自动登录" : "GAMA PASS自动登录";
        if (ImGui::Button(gpLabel, ImVec2(-1.f, btnH))) {
            if (autoPending) {
                sound::UiClick();
                LaunchPanel_CancelPendingAutoLaunch(ui);
                ui.status = "已取消自动登录 — 可改账号/昵称槽，再点「GAMA PASS自动登录」重新读秒";
                xcat::log::Info("App", "user cancelled pending GamaPass auto-launch");
            } else {
                msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
                LaunchPanel_ArmGamaPassAutoLaunch(ui);
                xcat::log::Info("App", "user re-armed GamaPass auto-launch (%us)",
                                kGamaPassAutoPrepSec);
            }
        }
        if (busy) ImGui::EndDisabled();
        if (autoPending && strategyPrepLeft > 0) {
            ImGui::TextColored(PrepHintBlue(), "准备中：%u 秒后自动换票", strategyPrepLeft);
        } else if (autoPending) {
            ImGui::TextColored(PrepHintBlue(), "即将自动换票…");
        }
    }
}


// 对齐枫星 payload_info::DrawUpdateControl：调试 TAB「日志 / 更新」内全宽按钮 + 进度。
void DrawUpdateControl() {
    const bool updateUi = UpdateShouldDrawProgressUi();
    const bool updateBusyGate = UpdateNeedsVisibleUi();
    const UpdateSnapshot snap = GetUpdateSnapshot();

    ImGui::PushID("xcat_update");
    if (updateBusyGate) ImGui::BeginDisabled();
    if (ImGui::Button("检查并安装更新", ImVec2(-1.f, ui::BtnH()))) {
        if (StartUpdateCheck(kDefaultUpdateServiceUrl)) sound::UiClick();
    }
    if (updateBusyGate) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (updateBusyGate) {
            ImGui::SetTooltip("更新进行中，请等待当前流程完成。");
        } else {
            ImGui::SetTooltip("检查内置更新口；有新版本则自动下载安装。\n"
                              "安装前会结束游戏与启动链相关进程，无需再确认。");
        }
    }

    if (snap.latestBuildId > 0) {
        const char* latestVer = snap.latestVersion.empty() ? "?" : snap.latestVersion.c_str();
        ImGui::TextDisabled("当前 v%s build %u / 最新 v%s build %u", xcat::kXcatVersionString,
                            xcat::kXcatBuildId, latestVer, snap.latestBuildId);
    } else {
        ImGui::TextDisabled("当前 v%s build %u", xcat::kXcatVersionString, xcat::kXcatBuildId);
    }

    if (updateUi) {
        float frac = -1.f;
        if (snap.phase == UpdatePhase::UpToDate) {
            frac = 1.f;
        } else if (snap.phase == UpdatePhase::Failed) {
            frac = 0.f;
        } else if (snap.progress >= 0.f && snap.progress <= 1.f) {
            frac = snap.progress;
        } else {
            frac = -1.0f * static_cast<float>(ImGui::GetTime());
        }
        ImGui::ProgressBar(frac, ImVec2(-1.f, AppDpi_Px(4.f)));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !snap.message.empty()) {
            const std::string msgUi = SanitizeImGuiLogLine(snap.message);
            ImGui::SetTooltip("%s", msgUi.c_str());
        }
    } else if (!snap.message.empty()) {
        const std::string msgUi = SanitizeImGuiLogLine(snap.message);
        ImGui::TextWrapped("%s", msgUi.c_str());
    }
    ImGui::PopID();
}

void DrawLaunchTab(LaunchUiState& ui) {
    {
        xcat::ui::CardGuard card("##tab_launch_account", "启动 / 注入");
        DrawLaunchCompactBar(ui);

        const auto launchMode = attach_inject::GetLaunchMode();
        if (launchMode == attach_inject::LaunchMode::OneClickLogin) {
            ImGui::Spacing();
            const float btnH = ui::BtnH();
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float halfW =
                (std::max)(1.f, (ImGui::GetContentRegionAvail().x - gap) * 0.5f);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextMultiline("##account", ui.accountLine, sizeof(ui.accountLine),
                                      ImVec2(-1, btnH * 3.6f), ImGuiInputTextFlags_AllowTabInput);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                LaunchPanel_FormatAccountForUi(ui);
            }
            if (ImGui::Button("保存账号", ImVec2(halfW, btnH))) {
                sound::UiClick();
                LaunchPanel_SaveAccount(ui);
                ui.status = "已保存到程序目录 account.txt";
            }
            ImGui::SameLine(0.f, gap);
            ImGui::TextDisabled("账密粘贴区（仅本页）");
        }

        if (!ui.status.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
            const std::string statusUi = SanitizeImGuiLogLine(ui.status);
            ImGui::TextUnformatted(statusUi.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    CardGap();
    {
        // 总开关在 gamapass_device_login.h：kGamaPassDeviceLoginEnabled。
        constexpr bool kGpDeviceLoginUiReady = msc::launcher::kGamaPassDeviceLoginEnabled;
        xcat::ui::CardGuard card("##tab_gp_device_login",
                                 kGpDeviceLoginUiReady ? "Gama Pass 账密登录"
                                                       : "Gama Pass 账密登录（未开放）");
        if (!kGpDeviceLoginUiReady) {
            ImGui::TextWrapped("功能尚未开放，请用上面的「GAMA PASS自动登录」。不会开独立浏览器配置。");
        } else {
        static bool loaded = false;
        if (kGpDeviceLoginUiReady && !loaded) {
            loaded = true;
            const std::wstring path =
                msc::launcher::GamaPassDeviceLoginStorePath(xcat::Utf8ToWide(ui.prefsBinDir));
            msc::launcher::GamaPassDeviceLoginAccount acc;
            if (msc::launcher::LoadGamaPassDeviceLoginAccount(path, acc) && !acc.email.empty()) {
                const std::string line = msc::launcher::FormatGamaPassDeviceLoginLine(acc);
                std::snprintf(ui.gpLoginLine, sizeof(ui.gpLoginLine), "%s", line.c_str());
                ui.gpLoginBrowserKind = static_cast<int>(acc.browserKind);
            }
        }

        if (kGpDeviceLoginUiReady) {
            ImGui::TextWrapped(
                "独立模块：只帮你在专用浏览器窗口登 Gama Pass。"
                "粘贴卖家整行：账号----密码----邮箱密码----device_id。"
                "原样钉第 4 段 device_id（禁止自造、不加横线）。"
                "不换票、不开游戏。登录成功后会关掉这扇独立窗，再点上面的「GAMA PASS自动登录」换票。"
                "二次验证请在弹出窗里完成；失败则窗口保持打开。日常 Chrome/Edge 登录态不动。");
        } else {
            ImGui::TextWrapped("功能尚未开放，请用上面的「GAMA PASS自动登录」。");
        }
        if (!kGpDeviceLoginUiReady) ImGui::BeginDisabled();

        const float btnH = ui::BtnH();
        const bool gpBusy = msc::launcher::IsGamaPassDeviceLoginBusy();
        if (gpBusy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint(
            "##gp_login_line",
            "账号----密码----邮箱密码----device_id",
            ui.gpLoginLine, sizeof(ui.gpLoginLine),
            gpBusy ? ImGuiInputTextFlags_ReadOnly : 0);
        if (gpBusy) ImGui::EndDisabled();

        msc::launcher::GamaPassDeviceLoginAccount preview;
        std::string parseErr;
        const bool lineOk =
            msc::launcher::ParseGamaPassDeviceLoginLine(ui.gpLoginLine, preview, parseErr);
        if (lineOk) {
            ImGui::TextDisabled("账号 %s  ·  device_id %s", preview.email.c_str(),
                                preview.deviceId.c_str());
        } else if (ui.gpLoginLine[0] != '\0') {
            ImGui::TextDisabled("%s", parseErr.c_str());
        }

        {
            if (!lineOk) ImGui::BeginDisabled();
            if (ImGui::Button("复制 device_id", ImVec2(AppDpi_Px(120.f), btnH))) {
                sound::UiClick();
                ImGui::SetClipboardText(preview.deviceId.c_str());
                ui.status = "已复制卖家 device_id";
            }
            if (!lineOk) ImGui::EndDisabled();
        }

        static std::string browserHint;
        static DWORD lastProbe = 0;
        if (kGpDeviceLoginUiReady && (lastProbe == 0 || GetTickCount() - lastProbe > 4000)) {
            lastProbe = GetTickCount();
            std::wstring exe, label;
            const auto kind =
                static_cast<msc::launcher::GpDeviceLoginBrowserKind>(ui.gpLoginBrowserKind);
            if (msc::launcher::ResolveGamaPassDeviceLoginBrowser(exe, label, nullptr, kind)) {
                browserHint = xcat::WideToUtf8(label) + " · 独立配置目录（非日常）";
            } else {
                browserHint = "未找到所选浏览器（支持 Chrome++ / Chrome / Edge，不支持 360）";
            }
        }
        {
            static const char* kBrowserItems[] = {
                "自动（Chrome++ > Chrome > Edge）",
                "Chrome++",
                "Google Chrome",
                "Microsoft Edge",
            };
            ImGui::SetNextItemWidth(-1.f);
            if (gpBusy) ImGui::BeginDisabled();
            if (ImGui::Combo("##gp_login_browser", &ui.gpLoginBrowserKind, kBrowserItems, 4)) {
                lastProbe = 0;
            }
            if (gpBusy) ImGui::EndDisabled();
        }
        ImGui::TextDisabled("%s", browserHint.c_str());

        if (gpBusy) ImGui::BeginDisabled();
        if (ImGui::Button(gpBusy ? "登录中…" : "开始登录", ImVec2(-1.f, btnH))) {
            sound::UiClick();
            if (!kGpDeviceLoginUiReady) {
                ui.status = "账密登录助手尚未开放";
            } else {
            msc::launcher::GamaPassDeviceLoginAccount acc;
            std::string lineErr;
            if (!msc::launcher::ParseGamaPassDeviceLoginLine(ui.gpLoginLine, acc, lineErr)) {
                ui.status = lineErr.empty() ? "卖家账号行无法解析" : lineErr;
                sound::UiError();
            } else {
                acc.browserKind =
                    static_cast<msc::launcher::GpDeviceLoginBrowserKind>(ui.gpLoginBrowserKind);
                const std::wstring path =
                    msc::launcher::GamaPassDeviceLoginStorePath(xcat::Utf8ToWide(ui.prefsBinDir));
                std::wstring err;
                if (!msc::launcher::StartGamaPassDeviceLogin(
                        acc, path, [](const std::wstring& line) { LaunchPanel_OnWebLog(line); },
                        err)) {
                    ui.status = err.empty() ? "无法开始账密登录" : xcat::WideToUtf8(err);
                    sound::UiError();
                } else {
                    ui.status = "已打开独立登录窗口（钉卖家 device_id；成功后自动关窗，不换票）";
                }
            }
            }
        }
        if (gpBusy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!kGpDeviceLoginUiReady) {
                ImGui::SetTooltip("功能尚未开放，请用上面的 GAMA PASS自动登录。");
            } else {
                ImGui::SetTooltip(
                    "浏览器：Chrome++ / Chrome / Edge（可选手选；360 不支持）。\n"
                    "各浏览器独立配置目录，不开日常 User Data。\n"
                    "accounts 页填第 2 段 Gama Pass 密码；第 3 段邮箱密码只保存不填。\n"
                    "device_id 必须是 32 位 hex。二次验证请在弹出窗口里自己完成。");
            }
        }
        if (!kGpDeviceLoginUiReady) ImGui::EndDisabled();
        } // kGpDeviceLoginUiReady
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_launch_log", "登录日志", /*fillRemaining=*/true);
        ImGui::TextUnformatted(msc::weblogin::IsBusy() || attach_inject::IsInjectBusy()
                                   ? "进行中…"
                                   : "最近输出");
        ImGui::BeginChild("##log_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(ui.logTail.empty() ? "(暂无日志)" : ui.logTail.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.f) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
}


// LiveStep UI 状态：首页落盘与「实验」TAB 共用，避免两处 static 互相覆盖。
static bool gUiCombatLiveStep = false;
// 「攻击无CD」：吸怪 TAB；未解锁不可用。
// 跳过攻击动画 / 砍动画倒计时：实验 TAB（不绑吸怪解锁）；默认关。
static bool gUiAttackAccelSkipPrepare = false;  // 实验 TAB「跳过攻击动画」
static bool gUiAttackAccelClearBusy = false;    // 吸怪 TAB「攻击无CD」
// 历史字段，Apply 不再抬间隔；仍落盘兼容。
static int gUiAttackAccelClearBusyMinIntervalMs =
    (int)xcat::kAttackAccelClearBusyMinIntervalDefaultMs;
// 首页「挂机 → 出刀间隔」= simpleCombatAttackIntervalMs（与「攻击无CD」开关无关）。
static int gUiSimpleCombatAttackIntervalMs =
    (int)xcat::kSimpleCombatAttackIntervalDefaultMs;
static bool gUiAttackAccelCutLayer = false;  // 实验 TAB「砍动画倒计时」；默认关
static bool gUiAttackAccelBooster = false;
static bool gUiAttackAccelActionSpeed = false;   // A 系 nSpeed_
static bool gUiAttackAccelPartyBooster = false;  // TempStats[4] PartyBooster
static int gUiAttackAccelPartyBoosterValue =
    (int)xcat::kAttackAccelPartyBoosterValueDefault;
static bool gUiAttackAccelBreakDegreeFloor = false;  // B 系 degree 下限破限
static int gUiAttackAccelBreakDegreeFloorLo =
    (int)xcat::kAttackAccelBreakDegreeFloorLoDefault;
// 技能满级 / 终极一击：首页落盘保留字段；控件在实验 TAB。
static bool gUiSkillMaxLevel = false;
static bool gUiFinalAttackForce = false;
// 近战不挥拳：首页落盘保留；控件在实验 TAB。
static bool gUiMeleeVeto = false;
// 自动召唤宠物 / 有粮才召：控件在实验 TAB，置灰不可用（强制关）。
static bool gUiPetSummon = false;
static bool gUiPetSummonRequireFood = false;
// 怪物刷新感知：首页落盘保留；控件在实验 TAB（1ms 热扫下无体感）。
static bool gUiMobPoolObserve = false;
// 打怪 TICK：落盘 [core]；控件在「吸怪 快攻」TAB「快攻」卡。
static int gUiCombatTickMs = (int)xcat::kSimpleCombatTickDefaultMs;
// 不打 MISS 怪：落盘 [core]；控件在「吸怪 快攻」TAB「快攻」卡。
static bool gUiSkipAccMissOn = xcat::kCombatSkipAccMissDefault != 0;
static int gUiSkipAccMissN = (int)xcat::kCombatSkipAccMissNDefault;
// 出刀按键 hold：控件在调试 TAB。
static int gUiAttackHoldMs = (int)xcat::kAttackHoldDefaultMs;
// F5 追怪策略：首页 Combo + persistCore 落盘。
static int gUiApproachMode = 0;  // 0=空中贴怪 / 1=拟人 / 2=站桩输出 / 3=瞬移找怪 / 4=关闭
static int gUiHiraishinLootHoldMs = (int)xcat::kHiraishinLootHoldDefaultMs;
static int gUiHiraishinRangePx = (int)xcat::kHiraishinRangeDefaultPx;
static int gUiHiraishinFrontDx = (int)xcat::kHiraishinFrontDxDefault;
static int gUiHiraishinFrontDy = (int)xcat::kHiraishinFrontDyDefault;
static const char* ApproachModeName(int m) {
    switch (m) {
        case 0:
            return "空中贴怪";
        case 1:
            return "拟人";
        case 2:
            return "站桩输出";
        case 3:
            return "瞬移找怪";
        default:
            return "关闭";
    }
}
// attack_rpc：仅实验 TAB；与 payload core 同步。
static bool gUiAttackRpc = false;
static int gUiAttackRpcMobs = (int)xcat::kAttackRpcMobsDefault;
static int gUiAttackRpcIntervalMs = (int)xcat::kAttackRpcIntervalDefaultMs;
static int gUiAttackRpcDamage = (int)xcat::kAttackRpcDamageDefault;
static bool gUiCombatForgeHit = false;
static int gUiForgeHitFrontDx = (int)xcat::kForgeHitFrontDxDefault;
static int gUiForgeHitFrontDy = (int)xcat::kForgeHitFrontDyDefault;
static bool gUiMapAttack = false;
static bool gUiMobGather = false;
static int gUiMobGatherStrategy = (int)xcat::kMobGatherStrategyDefault;
static bool gUiMobGatherLandOnArrive = xcat::kMobGatherLandOnArriveDefault != 0;
static int gUiMobGatherHopPx = (int)xcat::kMobGatherHopPxDefault;
static int gUiMobGatherSpeedPct = (int)xcat::kMobGatherSpeedPctDefault;
static bool gUiMobGatherAntiJitter = xcat::kMobGatherAntiJitterDefault != 0;
static int gUiMobGatherMax = (int)xcat::kMobGatherMaxDefault;
static int gUiMobGatherFarInFlight = (int)xcat::kMobGatherFarInFlightDefault;
static int gUiMobGatherRadiusPx = (int)xcat::kMobGatherRadiusDefaultPx;
static int gUiMobGatherLayerYPx = (int)xcat::kMobGatherLayerYPxDefault;
static int gUiMobGatherDyLimPx = (int)xcat::kMobGatherDyLimPxDefault;
static int gUiMobGatherWalkDx = (int)xcat::kMobGatherWalkDxDefault;
static int gUiMobGatherFeetExemptPx = (int)xcat::kMobGatherFeetExemptPxDefault;
static int gUiMobGatherHoldMs = (int)xcat::kMobGatherHoldMsDefault;
static int gUiMobGatherIntervalMs = (int)xcat::kMobGatherIntervalDefaultMs;
static bool gUiMobGatherIgnoreQuiet = xcat::kMobGatherIgnoreQuietDefault != 0;
static int gUiMobGatherQuietDelayMs = (int)xcat::kMobGatherQuietDelayMsDefault;
static bool gUiMobGatherStandOffCustom = xcat::kMobGatherStandOffCustomDefault != 0;
static int gUiMobGatherStandOffX = (int)xcat::kMobGatherStandOffXDefault;
static int gUiMobGatherStandOffY = (int)xcat::kMobGatherStandOffYDefault;
static int gUiMobGatherAimJitter = (int)xcat::kMobGatherAimJitterDefault;
static int gUiMobGatherStickCreep = (int)xcat::kMobGatherStickCreepDefault;
static int gUiMobGatherStickStillV = (int)xcat::kMobGatherStickStillVDefault;
static int gUiMobGatherCruiseR = (int)xcat::kMobGatherCruiseRDefault;
static int gUiMobGatherStationR = (int)xcat::kMobGatherStationRDefault;
static int gUiMobGatherMaxCmd = (int)xcat::kMobGatherMaxCmdDefault;
static int gUiMobGatherKp = (int)xcat::kMobGatherKpDefault;
static bool gUiMobGatherDispClampOn = xcat::kMobGatherDispClampOnDefault != 0;
static int gUiMobGatherDispCapPx = (int)xcat::kMobGatherDispCapPxDefault;
static int gUiMobGatherDead = (int)xcat::kMobGatherDeadDefault;
static int gUiMobGatherGravity = (int)xcat::kMobGatherGravityDefault;
static int gUiMobGatherCruiseV = (int)xcat::kMobGatherCruiseVDefault;
static int gUiMobGatherStationV = (int)xcat::kMobGatherStationVDefault;
static int gUiMobGatherHoldV = (int)xcat::kMobGatherHoldVDefault;
static int gUiMobGatherSettleErr = (int)xcat::kMobGatherSettleErrDefault;
static int gUiMobGatherKpSettle = (int)xcat::kMobGatherKpSettleDefault;
static int gUiMobGatherBrakeMs = (int)xcat::kMobGatherBrakeMsDefault;
static int gUiMobGatherCoastVy = (int)xcat::kMobGatherCoastVyDefault;
static int gUiMobGatherAimMs = (int)xcat::kMobGatherAimMsDefault;
static bool gUiMobGatherSoftRelogin = xcat::kMobGatherSoftReloginDefault != 0;
static int gUiMobGatherSoftReloginSec = (int)xcat::kMobGatherSoftReloginSecDefault;
static int gUiMobGatherHangupFires = (int)xcat::kMobGatherHangupFiresDefault;
static bool gUiMobGatherHangupFiresOn = xcat::kMobGatherHangupFiresOnDefault != 0;
static bool gUiMobGatherHangupUnbindF5 = xcat::kMobGatherHangupUnbindF5Default != 0;
static bool gUiMobGatherClearRelogin = xcat::kMobGatherClearReloginDefault != 0;
static bool gUiMobGatherSeekCluster = xcat::kMobGatherSeekClusterDefault != 0;
static bool gUiMobGatherPatrolFar = xcat::kMobGatherPatrolFarDefault != 0;
static bool gUiMobGatherAntiReport = xcat::kMobGatherAntiReportDefault != 0;
static bool gUiMobGatherHomeReturn = xcat::kMobGatherHomeReturnDefault != 0;
static int gUiMobGatherHomeX = 0;
static int gUiMobGatherHomeY = 0;
static int gUiMobGatherHomeMapId = 0;
static bool gUiMobGatherHomeValid = false;
static bool gUiMobGatherHomeHasMap = false;
static bool gUiMobGatherApplyCtrl = false;
// 地面门旁路：仅实验 TAB；与站立伪装（simpleCombatGroundSpoof）独立。
static bool gUiCurFhGateBypass = false;
// F6 手动飞 / F5 滑翔倍率：首页「飞行速度」卡；调试 TAB 不再改这两项。
static int gUiManualFlySpeedPct = (int)xcat::kFlySpeedPctDefault;
static int gUiFlySpeedPct = (int)xcat::kHeliSpeedPctDefault;
// 空中贴怪防抖：首页落盘；控件在调试 TAB「飞行调试」。
static bool gUiAntiJitter = xcat::kCombatAntiJitterDefault != 0;
// 拥堵让路：首页落盘；控件在调试 TAB「主线程泵」。
static int gUiPumpCongestion = (int)xcat::kPumpCongestionDefault;

static uint32_t MobGatherUiU32(int v) {
    if (v < 0) return 0u;
    return static_cast<uint32_t>(v);
}

static uint32_t gSyncedGatherUnlockWant = 0xFFFFFFFFu;

// 把 ws888 解锁镜像写进 payload，让 DLL 标题栏知道能不能画刀数。
static void SyncGatherUnlockToPayload(LaunchUiState& ui) {
    EnsureGatherUnlockLoaded();
    if (ui.prefsBinDir.empty()) return;
    const uint32_t want = gGatherTabUnlocked ? 1u : 0u;
    if (gSyncedGatherUnlockWant == want) return;
    xcat::PayloadControl c{};
    if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) return;
    if (c.gatherTabUnlocked == want) {
        gSyncedGatherUnlockWant = want;
        return;
    }
    c.gatherTabUnlocked = want;
    c.writeTickMs = GetTickCount64();
    if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) gSyncedGatherUnlockWant = want;
}

// 吸怪 TAB 未解锁时：关掉附属能力 UI，并把 user.ini 里仍为开的位强制清掉。
// 「跳过攻击动画」「砍动画倒计时」在实验 TAB，不在此闸内。
static void EnforceGatherTabExclusiveGates(LaunchUiState& ui) {
    EnsureGatherUnlockLoaded();
    SyncGatherUnlockToPayload(ui);
    if (gGatherTabUnlocked) return;
    gUiAttackAccelClearBusy = false;
    if (ui.prefsBinDir.empty()) return;
    xcat::PayloadControl c{};
    if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) return;
    if (c.attackAccelClearBusy == 0) return;
    c.attackAccelClearBusy = 0;
    c.gatherTabUnlocked = 0;
    c.writeTickMs = GetTickCount64();
    (void)xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c);
}

void DrawHomeTab(LaunchUiState& ui) {
    // 首页卡片顺序：启动浓缩条 → 挂机 → 飞行速度 → 拾物 → 打怪设置
    // 吸怪业务在「吸怪」TAB；飞控旋钮在「调试 → 吸怪飞控」。
    EnforceGatherTabExclusiveGates(ui);
    {
        xcat::ui::CardGuard card("##tab_home_launch", "启动");
        DrawLaunchCompactBar(ui);
    }
    CardGap();

    static bool autoEnter = true;  // 默认开：1 雪吉拉 / 槽1
    static int charSlot = 1;
    static int worldId = xcat::kDefaultWorldId;
    static char worldName[64]{"雪吉拉"};
    static bool autoLie = true;  // 与 PayloadControl 默认一致；读盘后覆盖
    static bool invincible = true;  // 与 PayloadControl 默认一致
    static bool fly = false;
    static bool hpPotion = true;
    static bool mpPotion = true;
    static int hpThresholdPct = 50;
    static int mpThresholdPct = 30;
    static bool autoCombat = false;
    // 空中贴怪的自定义站距：关=用内置近战最优值；开=完全听用户的（远程职业）
    static bool standOffCustom = false;
    static int standOffX = (int)xcat::kCombatStandOffXDefault;
    static int standOffY = (int)xcat::kCombatStandOffYDefault;
    // 防贴脸退避：站距 X/Y 内有任何怪就把站位点推开；复用上面那组 X/Y 当半径；默认关
    static bool antiHug = false;
    // 站立伪装：出刀瞬间伪造踩台，给腾空放不出技能的职业用；默认关
    static bool groundSpoof = false;
    static bool smartInterval = false;
    static int clusterWeight = 0;  // 0/1：群怪优先（沿用 clusterWeight 落盘）
    static bool hitRotateOn = false;
    static int hitRotateN = (int)xcat::kCombatHitRotateNDefault;
    static bool teleportOneHit = false;
    static int teleportMinDx = 220;
    static int teleportStandOff = (int)xcat::kCombatTeleportStandOffDefault;
    static int mobScanIntervalMs = (int)xcat::kMobScanIntervalDefaultMs;
    static int oneshotMaxHp = (int)xcat::kCombatOneshotMaxHpDefault;
    static int oneshotMinBumps = (int)xcat::kCombatOneshotMinBumpsDefault;
    static int oneshotMinFires = (int)xcat::kCombatOneshotMinFiresDefault;
    static int oneshotMinLagMs = (int)xcat::kCombatOneshotMinLagMsDefault;
    static int oneshotFoxFillGapMs = (int)xcat::kCombatOneshotFoxFillGapDefaultMs;
    static bool watchdog = true;
    static int noExpSec = static_cast<int>(xcat::kWatchdogNoExpSecDefault);
    static int cooldownSec = static_cast<int>(xcat::kWatchdogCooldownSecDefault);
    static bool softLoginProbe = true;  // 与 PayloadControl 默认一致：软重连默认开
    static bool coreLoaded = false;
    static uint64_t lastSeenTick = 0;

    if (!ui.prefsBinDir.empty()) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
            if (!coreLoaded || disk.writeTickMs != lastSeenTick) {
                autoLie = disk.autoLie != 0;
                invincible = disk.invuln != 0;
                gUiFinalAttackForce =
                    xcat::kFinalAttackForceUserEnabled && disk.finalAttackForce != 0;
                gUiSkillMaxLevel =
                    xcat::kSkillMaxLevelUserEnabled && disk.skillMaxLevel != 0;
                gUiAttackAccelCutLayer = disk.attackAccelCutLayer != 0;
                gUiAttackAccelSkipPrepare = disk.attackAccelSkipPrepare != 0;
                gUiAttackAccelClearBusy = disk.attackAccelClearBusy != 0;
                gUiAttackAccelClearBusyMinIntervalMs =
                    (int)xcat::ClampAttackAccelClearBusyMinIntervalMs(
                        disk.attackAccelClearBusyMinIntervalMs
                            ? disk.attackAccelClearBusyMinIntervalMs
                            : xcat::kAttackAccelClearBusyMinIntervalDefaultMs);
                gUiAttackAccelBooster =
                    xcat::kAttackAccelBoosterUserEnabled && disk.attackAccelBooster != 0;
                gUiAttackAccelActionSpeed = disk.attackAccelActionSpeed != 0;
                gUiAttackAccelPartyBooster = disk.attackAccelPartyBooster != 0;
                gUiAttackAccelPartyBoosterValue =
                    (int)xcat::ClampAttackAccelPartyBoosterValue(disk.attackAccelPartyBoosterValue);
                gUiAttackAccelBreakDegreeFloor = disk.attackAccelBreakDegreeFloor != 0;
                gUiAttackAccelBreakDegreeFloorLo =
                    (int)xcat::ClampAttackAccelBreakDegreeFloorLo(disk.attackAccelBreakDegreeFloorLo);
                fly = disk.fly != 0;
                autoEnter = disk.autoEnter != 0;
                hpPotion = disk.hpPotion != 0;
                mpPotion = disk.mpPotion != 0;
                gUiPetSummon = xcat::kPetSummonUserEnabled && disk.petSummon != 0;
                gUiPetSummonRequireFood =
                    xcat::kPetSummonUserEnabled && disk.petSummonRequireFood != 0;
                autoCombat = disk.simpleCombat != 0;
                if (disk.simpleCombatImpactApproach != 0)
                    gUiApproachMode = 0;
                else if (disk.simpleCombatHumanWalk != 0)
                    gUiApproachMode = 1;
                else if (disk.simpleCombatHiraishin != 0)
                    gUiApproachMode = 2;
                else if (disk.simpleCombatTeleport != 0)
                    gUiApproachMode = 3;
                else
                    gUiApproachMode = 4;
                gUiHiraishinLootHoldMs = (int)xcat::ClampHiraishinLootHoldMs(
                    disk.simpleCombatHiraishinLootHoldMs);
                gUiHiraishinRangePx = (int)xcat::ClampHiraishinRangePx(
                    disk.simpleCombatHiraishinRangePx);
                gUiHiraishinFrontDx = (int)xcat::ClampHiraishinFrontDx(
                    disk.simpleCombatHiraishinFrontDx);
                gUiHiraishinFrontDy = (int)xcat::ClampHiraishinFrontDy(
                    disk.simpleCombatHiraishinFrontDy);
                standOffCustom = disk.simpleCombatStandOffCustom != 0;
                standOffX = (int)xcat::ClampCombatStandOffX(disk.simpleCombatStandOffX);
                standOffY = (int)xcat::ClampCombatStandOffY(disk.simpleCombatStandOffY);
                antiHug = disk.simpleCombatAntiHug != 0;
                gUiMeleeVeto = disk.meleeVeto != 0;
                groundSpoof = disk.simpleCombatGroundSpoof != 0;
                gUiAntiJitter = disk.simpleCombatAntiJitter != 0;
                gUiFlySpeedPct = (int)xcat::ClampHeliSpeedPct(
                    disk.simpleCombatFlySpeedPct ? disk.simpleCombatFlySpeedPct
                                                 : xcat::kHeliSpeedPctDefault);
                gUiManualFlySpeedPct = (int)xcat::ClampHeliSpeedPct(
                    disk.flySpeedPct ? disk.flySpeedPct : xcat::kFlySpeedPctDefault);
                smartInterval = disk.simpleCombatSmartInterval != 0;
                gUiSimpleCombatAttackIntervalMs =
                    (int)xcat::ClampSimpleCombatAttackIntervalMs(
                        disk.simpleCombatAttackIntervalMs
                            ? disk.simpleCombatAttackIntervalMs
                            : xcat::kSimpleCombatAttackIntervalDefaultMs);
                gUiCombatTickMs = (int)xcat::ClampSimpleCombatTickMs(
                    disk.simpleCombatTickMs ? disk.simpleCombatTickMs
                                           : xcat::kSimpleCombatTickDefaultMs);
                clusterWeight = disk.clusterWeight != 0 ? 1 : 0;
                hitRotateOn = disk.simpleCombatHitRotate != 0;
                hitRotateN = (int)xcat::ClampCombatHitRotateN(
                    disk.simpleCombatHitRotateN ? disk.simpleCombatHitRotateN
                                                : xcat::kCombatHitRotateNDefault);
                gUiSkipAccMissOn = disk.simpleCombatSkipAccMiss != 0;
                gUiSkipAccMissN = (int)xcat::ClampCombatSkipAccMissN(
                    disk.simpleCombatSkipAccMissN ? disk.simpleCombatSkipAccMissN
                                                  : xcat::kCombatSkipAccMissNDefault);
                teleportOneHit = disk.simpleCombatTeleportOneHit != 0;
                // LiveStep / attack_rpc 仍默认关，仅实验 TAB 可勾。
                gUiCombatLiveStep = disk.simpleCombatLiveStep != 0;
                gUiAttackRpc = disk.attackRpc != 0;
                gUiAttackRpcMobs = (int)xcat::ClampAttackRpcMobs(
                    disk.attackRpcMobs ? disk.attackRpcMobs : xcat::kAttackRpcMobsDefault);
                gUiAttackRpcIntervalMs = (int)xcat::ClampAttackRpcIntervalMs(
                    disk.attackRpcIntervalMs ? disk.attackRpcIntervalMs
                                             : xcat::kAttackRpcIntervalDefaultMs);
                gUiAttackRpcDamage = (int)xcat::ClampAttackRpcDamage(
                    disk.attackRpcDamage ? disk.attackRpcDamage
                                         : xcat::kAttackRpcDamageDefault);
                gUiCombatForgeHit = disk.simpleCombatForgeHit != 0;
                gUiForgeHitFrontDx = (int)xcat::ClampForgeHitFrontDx(disk.simpleCombatForgeHitFrontDx);
                gUiForgeHitFrontDy = (int)xcat::ClampForgeHitFrontDy(disk.simpleCombatForgeHitFrontDy);
                gUiMapAttack = disk.mapAttack != 0;
                gUiMobGather = disk.mobGather != 0;
                gUiMobGatherStrategy = (int)xcat::ClampMobGatherStrategy(disk.mobGatherStrategy);
                gUiMobGatherLandOnArrive = disk.mobGatherLandOnArrive != 0;
                gUiMobGatherHopPx = (int)xcat::ClampMobGatherHopPx(disk.mobGatherHopPx);
                gUiMobGatherSpeedPct = (int)disk.mobGatherSpeedPct;
                gUiMobGatherAntiJitter = disk.mobGatherAntiJitter != 0;
                gUiMobGatherMax = (int)xcat::ClampMobGatherMax(
                    disk.mobGatherMax ? disk.mobGatherMax : xcat::kMobGatherMaxDefault);
                gUiMobGatherFarInFlight =
                    (int)xcat::ClampMobGatherFarInFlight(disk.mobGatherFarInFlight);
                gUiMobGatherRadiusPx = (int)xcat::ClampMobGatherRadiusPx(
                    disk.mobGatherRadiusPx ? disk.mobGatherRadiusPx
                                           : xcat::kMobGatherRadiusDefaultPx);
                gUiMobGatherLayerYPx = (int)xcat::ClampMobGatherLayerYPx(disk.mobGatherLayerYPx);
                gUiMobGatherDyLimPx = (int)xcat::ClampMobGatherDyLimPx(disk.mobGatherDyLimPx);
                gUiMobGatherWalkDx = (int)xcat::ClampMobGatherWalkDx(disk.mobGatherWalkDx);
                gUiMobGatherFeetExemptPx =
                    (int)xcat::ClampMobGatherFeetExemptPx(disk.mobGatherFeetExemptPx);
                gUiMobGatherHoldMs = (int)xcat::ClampMobGatherHoldMs(
                    disk.mobGatherHoldMs ? disk.mobGatherHoldMs : xcat::kMobGatherHoldMsDefault);
                gUiMobGatherIntervalMs = (int)xcat::ClampMobGatherIntervalMs(
                    disk.mobGatherIntervalMs ? disk.mobGatherIntervalMs
                                             : xcat::kMobGatherIntervalDefaultMs);
                gUiMobGatherIgnoreQuiet = disk.mobGatherIgnoreQuiet != 0;
                gUiMobGatherQuietDelayMs =
                    (int)xcat::ClampMobGatherQuietDelayMs(disk.mobGatherQuietDelayMs);
                gUiMobGatherStandOffCustom = disk.mobGatherStandOffCustom != 0;
                gUiMobGatherStandOffX = (int)xcat::ClampMobGatherStandOffX(disk.mobGatherStandOffX);
                gUiMobGatherStandOffY = (int)xcat::ClampMobGatherStandOffY(disk.mobGatherStandOffY);
                gUiMobGatherAimJitter = (int)xcat::ClampMobGatherAimJitter(disk.mobGatherAimJitterPx);
                gUiMobGatherStickCreep = (int)disk.mobGatherStickCreepPx;
                gUiMobGatherStickStillV = (int)disk.mobGatherStickStillV;
                gUiMobGatherCruiseR = (int)disk.mobGatherCruiseR;
                gUiMobGatherStationR = (int)disk.mobGatherStationR;
                gUiMobGatherMaxCmd = (int)disk.mobGatherMaxCmd;
                gUiMobGatherKp = (int)disk.mobGatherKp;
                gUiMobGatherDispClampOn = disk.mobGatherDispClampOn != 0;
                gUiMobGatherDispCapPx =
                    (int)xcat::ClampMobGatherDispCapPx(disk.mobGatherDispCapPx);
                gUiMobGatherDead = (int)disk.mobGatherDead;
                gUiMobGatherGravity = (int)disk.mobGatherGravity;
                gUiMobGatherCruiseV = (int)disk.mobGatherCruiseV;
                gUiMobGatherStationV = (int)disk.mobGatherStationV;
                gUiMobGatherHoldV = (int)disk.mobGatherHoldV;
                gUiMobGatherSettleErr = (int)disk.mobGatherSettleErr;
                gUiMobGatherKpSettle = (int)disk.mobGatherKpSettle;
                gUiMobGatherBrakeMs = (int)disk.mobGatherBrakeMs;
                gUiMobGatherCoastVy = (int)disk.mobGatherCoastVy;
                gUiMobGatherAimMs = (int)disk.mobGatherAimMs;
                gUiMobGatherSoftRelogin = disk.mobGatherSoftRelogin != 0;
                gUiMobGatherSoftReloginSec = (int)xcat::ClampMobGatherSoftReloginSec(
                    disk.mobGatherSoftReloginSec ? disk.mobGatherSoftReloginSec
                                                 : xcat::kMobGatherSoftReloginSecDefault);
                gUiMobGatherHangupFires =
                    (int)xcat::ClampMobGatherHangupFires(disk.mobGatherHangupFires);
                gUiMobGatherHangupFiresOn = disk.mobGatherHangupFiresOn != 0;
                gUiMobGatherHangupUnbindF5 = disk.mobGatherHangupUnbindF5 != 0;
                gUiMobGatherClearRelogin = disk.mobGatherClearRelogin != 0;
                gUiMobGatherSeekCluster = disk.mobGatherSeekCluster != 0;
                gUiMobGatherPatrolFar = disk.mobGatherPatrolFar != 0;
                gUiMobGatherAntiReport = false;
                gUiMobGatherHomeReturn = disk.mobGatherHomeReturn != 0;
                gUiMobGatherHomeX = (int)xcat::ClampMobGatherStandOffX(disk.mobGatherHomeX);
                gUiMobGatherHomeY = (int)xcat::ClampMobGatherStandOffY(disk.mobGatherHomeY);
                gUiMobGatherHomeMapId = disk.mobGatherHomeMapId;
                gUiMobGatherHomeValid = disk.mobGatherHomeValid != 0;
                gUiMobGatherHomeHasMap = disk.mobGatherHomeHasMap != 0;
                gUiMobGatherApplyCtrl = disk.mobGatherApplyCtrl != 0;
                teleportMinDx = (int)xcat::ClampCombatTeleportMinDx(
                    disk.simpleCombatTeleportMinDx ? disk.simpleCombatTeleportMinDx
                                                   : xcat::kCombatTeleportMinDxDefault);
                teleportStandOff = (int)xcat::ClampCombatTeleportStandOff(
                    disk.simpleCombatStandOffCustom ? disk.simpleCombatStandOffX
                                                    : xcat::kCombatStandOffXDefault);
                mobScanIntervalMs = (int)xcat::ClampMobScanIntervalMs(
                    disk.mobScanIntervalMs ? disk.mobScanIntervalMs
                                           : xcat::kMobScanIntervalDefaultMs);
                gUiMobPoolObserve = disk.mobPoolObserve != 0;
                oneshotMaxHp = (int)xcat::ClampCombatOneshotMaxHp(disk.simpleCombatOneshotMaxHp);
                oneshotMinBumps =
                    (int)xcat::ClampCombatOneshotMinBumps(disk.simpleCombatOneshotMinBumps);
                oneshotMinFires = (int)xcat::ClampCombatOneshotMinFires(
                    disk.simpleCombatOneshotMinFires ? disk.simpleCombatOneshotMinFires
                                                     : xcat::kCombatOneshotMinFiresDefault);
                oneshotMinLagMs =
                    (int)xcat::ClampCombatOneshotMinLagMs(disk.simpleCombatOneshotMinLagMs);
                oneshotFoxFillGapMs =
                    (int)xcat::ClampCombatOneshotFoxFillGapMs(disk.simpleCombatOneshotFoxFillGapMs);
                gUiPumpCongestion = (int)xcat::ClampPumpCongestion(disk.pumpCongestionThreshold);
                hpThresholdPct = (int)(disk.hpThresholdPct ? disk.hpThresholdPct : 50u);
                mpThresholdPct = (int)(disk.mpThresholdPct ? disk.mpThresholdPct : 30u);
                charSlot = (int)(disk.charSlot ? disk.charSlot : 1u);
                worldId = disk.worldId;
                strncpy_s(worldName, disk.worldName, _TRUNCATE);
                // _Center2 → 菇菇寶貝（world_names.tsv）；旧 core 值每帧也会再美化一次
                if (worldName[0]) {
                    const xcat::WorldNamesPack& wn =
                        xcat::GetSharedWorldNames(ui.prefsBinDir.c_str());
                    const std::string pretty = xcat::WorldNamePreferDisplay(wn, worldName);
                    if (!pretty.empty()) strncpy_s(worldName, pretty.c_str(), _TRUNCATE);
                }
                watchdog = disk.launcherWatchdog != 0;
                noExpSec = static_cast<int>(
                    xcat::ClampWatchdogNoExpSec(disk.launcherWatchdogNoExpSec));
                cooldownSec = static_cast<int>(
                    xcat::ClampWatchdogCooldownSec(disk.launcherWatchdogCooldownSec));
                // 软重连只跟 softLoginProbe；禁止用旧 galaxyTokenProbe OR 抬成软重连
                //（仅采证请用 galaxy_token_probe.on / GALAXY_TOKEN_PROBE=1）。
                softLoginProbe = disk.softLoginProbe != 0;
                // 不从 core.autoSell* 灌 UI：真源 [auto_supply]
                lastSeenTick = disk.writeTickMs;
                coreLoaded = true;
            }
        } else if (!coreLoaded) {
            coreLoaded = true;
        }
    }

    auto persistCore = [&]() {
        // 首页字段未从磁盘灌入前禁止写盘，避免 DragInt/勾选把静态默认值盖掉 user.ini。
        if (ui.prefsBinDir.empty() || !coreLoaded) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.autoLie = autoLie ? 1u : 0u;
        c.invuln = invincible ? 1u : 0u;
        c.attackAccel = 0;  // 首页入口已删；旧 attackAccel 包强制关
        c.finalAttackForce =
            (xcat::kFinalAttackForceUserEnabled && gUiFinalAttackForce) ? 1u : 0u;
        c.skillMaxLevel =
            (xcat::kSkillMaxLevelUserEnabled && gUiSkillMaxLevel) ? 1u : 0u;
        // 「攻击无CD」：吸怪 TAB 附属；未解锁强制关，避免旧 ini 仍生效。
        // 「跳过攻击动画」「砍动画倒计时」在实验 TAB，不绑解锁。
        if (!WorkspaceGatherTabUnlocked()) {
            gUiAttackAccelClearBusy = false;
        }
        c.attackAccelCutLayer = gUiAttackAccelCutLayer ? 1u : 0u;
        c.attackAccelSkipPrepare = gUiAttackAccelSkipPrepare ? 1u : 0u;
        c.attackAccelClearBusy = gUiAttackAccelClearBusy ? 1u : 0u;
        c.attackAccelClearBusyMinIntervalMs = xcat::ClampAttackAccelClearBusyMinIntervalMs(
            (uint32_t)gUiAttackAccelClearBusyMinIntervalMs);
        c.attackAccelBooster =
            (xcat::kAttackAccelBoosterUserEnabled && gUiAttackAccelBooster) ? 1u : 0u;
        c.attackAccelActionSpeed = gUiAttackAccelActionSpeed ? 1u : 0u;
        c.attackAccelPartyBooster = gUiAttackAccelPartyBooster ? 1u : 0u;
        c.attackAccelPartyBoosterValue =
            xcat::ClampAttackAccelPartyBoosterValue(gUiAttackAccelPartyBoosterValue);
        c.attackAccelBreakDegreeFloor = gUiAttackAccelBreakDegreeFloor ? 1u : 0u;
        c.attackAccelBreakDegreeFloorLo =
            xcat::ClampAttackAccelBreakDegreeFloorLo(gUiAttackAccelBreakDegreeFloorLo);
        c.attackSameFrameBurst = xcat::kAttackSameFrameBurstDefault;
        c.fly = fly ? 1u : 0u;
        // flyMode 由调试 TAB 选推进路线；首页不覆盖
        c.autoEnter = autoEnter ? 1u : 0u;
        c.hpPotion = hpPotion ? 1u : 0u;
        c.mpPotion = mpPotion ? 1u : 0u;
        // 自动召唤：实验 TAB 置灰；读盘/落盘/Apply 由 kPetSummonUserEnabled 硬关。
        if (!xcat::kPetSummonUserEnabled) {
            gUiPetSummon = false;
            gUiPetSummonRequireFood = false;
        }
        c.petSummon =
            (xcat::kPetSummonUserEnabled && gUiPetSummon) ? 1u : 0u;
        c.petSummonRequireFood =
            (xcat::kPetSummonUserEnabled && gUiPetSummonRequireFood) ? 1u : 0u;
        c.simpleCombat = autoCombat ? 1u : 0u;
        // 单选落盘：空中贴怪 / 拟人 / 站桩输出 / 瞬移找怪互斥；关闭则全关。
        c.simpleCombatImpactApproach = (gUiApproachMode == 0) ? 1u : 0u;
        c.simpleCombatHumanWalk = (gUiApproachMode == 1) ? 1u : 0u;
        c.simpleCombatHiraishin = (gUiApproachMode == 2) ? 1u : 0u;
        c.simpleCombatHiraishinLootHoldMs = xcat::ClampHiraishinLootHoldMs(
            static_cast<uint32_t>(gUiHiraishinLootHoldMs < 0 ? 0 : gUiHiraishinLootHoldMs));
        gUiHiraishinLootHoldMs = (int)c.simpleCombatHiraishinLootHoldMs;
        c.simpleCombatHiraishinRangePx = xcat::ClampHiraishinRangePx(
            static_cast<uint32_t>(gUiHiraishinRangePx < 0 ? 0 : gUiHiraishinRangePx));
        gUiHiraishinRangePx = (int)c.simpleCombatHiraishinRangePx;
        c.simpleCombatHiraishinFrontDx = xcat::ClampHiraishinFrontDx(
            static_cast<uint32_t>(gUiHiraishinFrontDx < 0 ? 0 : gUiHiraishinFrontDx));
        gUiHiraishinFrontDx = (int)c.simpleCombatHiraishinFrontDx;
        c.simpleCombatHiraishinFrontDy = xcat::ClampHiraishinFrontDy(
            static_cast<uint32_t>(gUiHiraishinFrontDy < 0 ? 0 : gUiHiraishinFrontDy));
        gUiHiraishinFrontDy = (int)c.simpleCombatHiraishinFrontDy;
        c.simpleCombatStandOffCustom = standOffCustom ? 1u : 0u;
        c.simpleCombatStandOffX =
            xcat::ClampCombatStandOffX(static_cast<uint32_t>(standOffX < 0 ? 0 : standOffX));
        c.simpleCombatStandOffY = xcat::ClampCombatStandOffY(static_cast<int32_t>(standOffY));
        // 拟人 / 瞬移地面落点 X 与首页自定义站距共用；不勾则用内置 X。
        {
            const uint32_t sharedX = standOffCustom ? c.simpleCombatStandOffX
                                                    : xcat::kCombatStandOffXDefault;
            c.simpleCombatTeleportStandOff = xcat::ClampCombatTeleportStandOff(sharedX);
            teleportStandOff = (int)c.simpleCombatTeleportStandOff;
        }
        c.simpleCombatAntiHug = antiHug ? 1u : 0u;
        c.meleeVeto = gUiMeleeVeto ? 1u : 0u;
        c.simpleCombatGroundSpoof = groundSpoof ? 1u : 0u;
        c.simpleCombatAntiJitter = gUiAntiJitter ? 1u : 0u;
        c.simpleCombatFlySpeedPct =
            xcat::ClampHeliSpeedPct(static_cast<uint32_t>(gUiFlySpeedPct < 0 ? 0 : gUiFlySpeedPct));
        c.flySpeedPct = xcat::ClampHeliSpeedPct(
            static_cast<uint32_t>(gUiManualFlySpeedPct < 0 ? 0 : gUiManualFlySpeedPct));
        c.simpleCombatSmartInterval = smartInterval ? 1u : 0u;
        c.simpleCombatAttackIntervalMs = xcat::ClampSimpleCombatAttackIntervalMs(
            static_cast<uint32_t>(gUiSimpleCombatAttackIntervalMs));
        gUiSimpleCombatAttackIntervalMs = (int)c.simpleCombatAttackIntervalMs;
        c.simpleCombatTickMs = xcat::ClampSimpleCombatTickMs(
            static_cast<uint32_t>(gUiCombatTickMs < 0 ? 0 : gUiCombatTickMs));
        c.clusterWeight = clusterWeight ? 1u : 0u;
        c.simpleCombatHitRotate = hitRotateOn ? 1u : 0u;
        c.simpleCombatHitRotateN = xcat::ClampCombatHitRotateN(
            static_cast<uint32_t>(hitRotateN < 1 ? 1 : hitRotateN));
        c.simpleCombatSkipAccMiss = gUiSkipAccMissOn ? 1u : 0u;
        c.simpleCombatSkipAccMissN = xcat::ClampCombatSkipAccMissN(
            static_cast<uint32_t>(gUiSkipAccMissN < 1 ? 1 : gUiSkipAccMissN));
        c.simpleCombatTeleport = (gUiApproachMode == 3) ? 1u : 0u;
        c.simpleCombatTeleportOneHit = teleportOneHit ? 1u : 0u;
        c.simpleCombatLiveStep = gUiCombatLiveStep ? 1u : 0u;
        c.attackRpc = gUiAttackRpc ? 1u : 0u;
        c.attackRpcMobs = xcat::ClampAttackRpcMobs(
            static_cast<uint32_t>(gUiAttackRpcMobs < 0 ? 0 : gUiAttackRpcMobs));
        c.attackRpcIntervalMs = xcat::ClampAttackRpcIntervalMs(
            static_cast<uint32_t>(gUiAttackRpcIntervalMs < 0 ? 0 : gUiAttackRpcIntervalMs));
        c.attackRpcDamage = xcat::ClampAttackRpcDamage(
            static_cast<uint32_t>(gUiAttackRpcDamage < 0 ? 0 : gUiAttackRpcDamage));
        c.simpleCombatForgeHit = gUiCombatForgeHit ? 1u : 0u;
        c.simpleCombatForgeHitFrontDx = xcat::ClampForgeHitFrontDx(
            static_cast<uint32_t>(gUiForgeHitFrontDx < 0 ? 0 : gUiForgeHitFrontDx));
        c.simpleCombatForgeHitFrontDy = xcat::ClampForgeHitFrontDy(
            static_cast<uint32_t>(gUiForgeHitFrontDy < 0 ? 0 : gUiForgeHitFrontDy));
        c.simpleCombatForgeHitMobs = xcat::kForgeHitMobsDefault;
        c.simpleCombatForgeHitFillList = 0;
        c.simpleCombatForgeHitMultiPkt = 0;
        gUiForgeHitFrontDx = (int)c.simpleCombatForgeHitFrontDx;
        gUiForgeHitFrontDy = (int)c.simpleCombatForgeHitFrontDy;
        c.mapAttack = gUiMapAttack ? 1u : 0u;
        c.mobGather = gUiMobGather ? 1u : 0u;
        c.mobGatherStrategy = xcat::ClampMobGatherStrategy(
            static_cast<uint32_t>(gUiMobGatherStrategy < 0 ? 0 : gUiMobGatherStrategy));
        c.mobGatherLandOnArrive = gUiMobGatherLandOnArrive ? 1u : 0u;
        c.mobGatherHopPx = xcat::ClampMobGatherHopPx(
            static_cast<uint32_t>(gUiMobGatherHopPx < 0 ? 0 : gUiMobGatherHopPx));
        c.mobGatherSpeedPct = MobGatherUiU32(gUiMobGatherSpeedPct);
        c.mobGatherAntiJitter = gUiMobGatherAntiJitter ? 1u : 0u;
        c.mobGatherMax = xcat::ClampMobGatherMax(
            static_cast<uint32_t>(gUiMobGatherMax < 0 ? 0 : gUiMobGatherMax));
        c.mobGatherFarInFlight = xcat::ClampMobGatherFarInFlight(
            static_cast<uint32_t>(gUiMobGatherFarInFlight < 0 ? 0 : gUiMobGatherFarInFlight));
        c.mobGatherRadiusPx = xcat::ClampMobGatherRadiusPx(
            static_cast<uint32_t>(gUiMobGatherRadiusPx < 0 ? 0 : gUiMobGatherRadiusPx));
        c.mobGatherLayerYPx = xcat::ClampMobGatherLayerYPx(
            static_cast<uint32_t>(gUiMobGatherLayerYPx < 0 ? 0 : gUiMobGatherLayerYPx));
        c.mobGatherDyLimPx = xcat::ClampMobGatherDyLimPx(
            static_cast<uint32_t>(gUiMobGatherDyLimPx < 0 ? 0 : gUiMobGatherDyLimPx));
        c.mobGatherWalkDx = xcat::ClampMobGatherWalkDx(
            static_cast<uint32_t>(gUiMobGatherWalkDx < 0 ? 0 : gUiMobGatherWalkDx));
        c.mobGatherFeetExemptPx = xcat::ClampMobGatherFeetExemptPx(
            static_cast<uint32_t>(gUiMobGatherFeetExemptPx < 0 ? 0 : gUiMobGatherFeetExemptPx));
        c.mobGatherHoldMs = xcat::ClampMobGatherHoldMs(
            static_cast<uint32_t>(gUiMobGatherHoldMs < 0 ? 0 : gUiMobGatherHoldMs));
        c.mobGatherIntervalMs = xcat::ClampMobGatherIntervalMs(
            static_cast<uint32_t>(gUiMobGatherIntervalMs < 0 ? 0 : gUiMobGatherIntervalMs));
        c.mobGatherIgnoreQuiet = gUiMobGatherIgnoreQuiet ? 1u : 0u;
        c.mobGatherQuietDelayMs = xcat::ClampMobGatherQuietDelayMs(
            static_cast<uint32_t>(gUiMobGatherQuietDelayMs < 0 ? 0 : gUiMobGatherQuietDelayMs));
        c.mobGatherStandOffCustom = gUiMobGatherStandOffCustom ? 1u : 0u;
        c.mobGatherStandOffX = xcat::ClampMobGatherStandOffX(gUiMobGatherStandOffX);
        c.mobGatherStandOffY = xcat::ClampMobGatherStandOffY(gUiMobGatherStandOffY);
        c.mobGatherAimJitterPx = xcat::ClampMobGatherAimJitter(
            static_cast<uint32_t>(gUiMobGatherAimJitter < 0 ? 0 : gUiMobGatherAimJitter));
        c.mobGatherStickCreepPx = MobGatherUiU32(gUiMobGatherStickCreep);
        c.mobGatherStickStillV = MobGatherUiU32(gUiMobGatherStickStillV);
        c.mobGatherCruiseR = MobGatherUiU32(gUiMobGatherCruiseR);
        c.mobGatherStationR = MobGatherUiU32(gUiMobGatherStationR);
        c.mobGatherMaxCmd = MobGatherUiU32(gUiMobGatherMaxCmd);
        c.mobGatherKp = MobGatherUiU32(gUiMobGatherKp);
        c.mobGatherDispClampOn = gUiMobGatherDispClampOn ? 1u : 0u;
        c.mobGatherDispCapPx =
            xcat::ClampMobGatherDispCapPx(MobGatherUiU32(gUiMobGatherDispCapPx));
        c.mobGatherDead = MobGatherUiU32(gUiMobGatherDead);
        c.mobGatherGravity = MobGatherUiU32(gUiMobGatherGravity);
        c.mobGatherCruiseV = MobGatherUiU32(gUiMobGatherCruiseV);
        c.mobGatherStationV = MobGatherUiU32(gUiMobGatherStationV);
        c.mobGatherHoldV = MobGatherUiU32(gUiMobGatherHoldV);
        c.mobGatherSettleErr = MobGatherUiU32(gUiMobGatherSettleErr);
        c.mobGatherKpSettle = MobGatherUiU32(gUiMobGatherKpSettle);
        c.mobGatherBrakeMs = MobGatherUiU32(gUiMobGatherBrakeMs);
        c.mobGatherCoastVy = MobGatherUiU32(gUiMobGatherCoastVy);
        c.mobGatherAimMs = MobGatherUiU32(gUiMobGatherAimMs);
        c.mobGatherSoftRelogin = gUiMobGatherSoftRelogin ? 1u : 0u;
        c.mobGatherSoftReloginSec = xcat::ClampMobGatherSoftReloginSec(
            static_cast<uint32_t>(gUiMobGatherSoftReloginSec < 0 ? 0 : gUiMobGatherSoftReloginSec));
        c.mobGatherHangupFires = xcat::ClampMobGatherHangupFires(
            static_cast<uint32_t>(gUiMobGatherHangupFires < 0 ? 0 : gUiMobGatherHangupFires));
        c.mobGatherHangupFiresOn = gUiMobGatherHangupFiresOn ? 1u : 0u;
        c.mobGatherHangupUnbindF5 = gUiMobGatherHangupUnbindF5 ? 1u : 0u;
        c.gatherTabUnlocked = WorkspaceGatherTabUnlocked() ? 1u : 0u;
        c.mobGatherClearRelogin = gUiMobGatherClearRelogin ? 1u : 0u;
        c.mobGatherSeekCluster = gUiMobGatherSeekCluster ? 1u : 0u;
        c.mobGatherPatrolFar = gUiMobGatherPatrolFar ? 1u : 0u;
        c.mobGatherAntiReport = 0u;
        c.mobGatherHomeReturn = gUiMobGatherHomeReturn ? 1u : 0u;
        c.mobGatherApplyCtrl = gUiMobGatherApplyCtrl ? 1u : 0u;
        c.simpleCombatTeleportMinDx =
            xcat::ClampCombatTeleportMinDx(static_cast<uint32_t>(teleportMinDx < 0 ? 0 : teleportMinDx));
        // simpleCombatTeleportStandOff 已在上面按共用 X 写入。
        // 贴怪节流用代码默认；面板已拆除「瞬移冷却」入口。
        c.simpleCombatTeleportCooldownMs = xcat::kCombatTeleportCooldownDefaultMs;
        c.mobScanIntervalMs = xcat::ClampMobScanIntervalMs(
            static_cast<uint32_t>(mobScanIntervalMs < 0 ? 0 : mobScanIntervalMs));
        c.mobPoolObserve = gUiMobPoolObserve ? 1u : 0u;
        c.simpleCombatOneshotMaxHp = xcat::ClampCombatOneshotMaxHp(
            static_cast<uint32_t>(oneshotMaxHp < 0 ? 0 : oneshotMaxHp));
        c.simpleCombatOneshotMinBumps = xcat::ClampCombatOneshotMinBumps(
            static_cast<uint32_t>(oneshotMinBumps < 0 ? 0 : oneshotMinBumps));
        c.simpleCombatOneshotMinFires = xcat::ClampCombatOneshotMinFires(
            static_cast<uint32_t>(oneshotMinFires < 0 ? 0 : oneshotMinFires));
        c.simpleCombatOneshotMinLagMs = xcat::ClampCombatOneshotMinLagMs(
            static_cast<uint32_t>(oneshotMinLagMs < 0 ? 0 : oneshotMinLagMs));
        c.simpleCombatOneshotFoxFillGapMs = xcat::ClampCombatOneshotFoxFillGapMs(
            static_cast<uint32_t>(oneshotFoxFillGapMs < 0 ? 0 : oneshotFoxFillGapMs));
        c.pumpCongestionThreshold = xcat::ClampPumpCongestion(
            static_cast<uint32_t>(gUiPumpCongestion < 0 ? 0 : gUiPumpCongestion));
        c.hpThresholdPct = hpThresholdPct < 1 ? 1u : (hpThresholdPct > 99 ? 99u : (uint32_t)hpThresholdPct);
        c.mpThresholdPct = mpThresholdPct < 1 ? 1u : (mpThresholdPct > 99 ? 99u : (uint32_t)mpThresholdPct);
        c.charSlot = charSlot < 1 ? 1u : (uint32_t)charSlot;
        c.worldId = worldId;
        strncpy_s(c.worldName, worldName, _TRUNCATE);
        c.launcherWatchdog = watchdog ? 1u : 0u;
        c.launcherWatchdogNoExpSec =
            xcat::ClampWatchdogNoExpSec(static_cast<uint32_t>(noExpSec));
        c.launcherWatchdogCooldownSec =
            xcat::ClampWatchdogCooldownSec(static_cast<uint32_t>(cooldownSec));
        // UI 开软重连时同步武装 Galaxy Token 采证；关则两字段都关。
        // 仅采证不软重连：galaxy_token_probe.on / GALAXY_TOKEN_PROBE=1（不走本勾选）。
        c.softLoginProbe = softLoginProbe ? 1u : 0u;
        c.galaxyTokenProbe = softLoginProbe ? 1u : 0u;
        // 补给开关/店图不写 core：真源 [auto_supply]。
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            lastSeenTick = c.writeTickMs;
            xcat::log::Ok("App",
                          "已下发 core：测谎=%d 无敌=%d 自动进=%d 加血=%d@%d 加蓝=%d@%d 召宠=%d "
                          "有粮才召=%d 打怪=%d 追怪=%s 间隔=%d 群怪=%d 打中换怪=%d/%d 贴怪瞬移=%d LiveStep=%d "
                          "分区=%d 槽=%d 守护=%d softLogin=%d "
                          "读怪=%d 终极一击=%d 技能满级=%d 攻击无CD=%d 吸怪=%d/%u%% max=%u r=%u hold=%u iv=%u quiet=%d qdelay=%u",
                          autoLie ? 1 : 0, invincible ? 1 : 0, autoEnter ? 1 : 0,
                          hpPotion ? 1 : 0, hpThresholdPct, mpPotion ? 1 : 0, mpThresholdPct,
                          gUiPetSummon ? 1 : 0, gUiPetSummonRequireFood ? 1 : 0,
                          autoCombat ? 1 : 0, ApproachModeName(gUiApproachMode),
                          gUiSimpleCombatAttackIntervalMs, clusterWeight ? 1 : 0,
                          hitRotateOn ? 1 : 0, hitRotateN, c.simpleCombatTeleport ? 1 : 0,
                          gUiCombatLiveStep ? 1 : 0, worldId, charSlot, watchdog ? 1 : 0,
                          softLoginProbe ? 1 : 0,
                          mobScanIntervalMs, gUiFinalAttackForce ? 1 : 0, gUiSkillMaxLevel ? 1 : 0,
                          gUiAttackAccelClearBusy ? 1 : 0, gUiMobGather ? 1 : 0, c.mobGatherSpeedPct,
                          c.mobGatherMax, c.mobGatherRadiusPx, c.mobGatherHoldMs,
                          c.mobGatherIntervalMs, c.mobGatherIgnoreQuiet ? 1 : 0,
                          c.mobGatherQuietDelayMs);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] 失败");
        }
    };

    // 吸怪 / F5 滑翔 / F6 手动飞共用：输入 + 巡航换算 + 快捷档。
    // 1.0X 基准合速：巡航 620（heli_rotor.cpp kBase*）。
    auto speedRow = [&](const char* label, const char* id, int* v, const char* tip,
                        bool enabled) {
        ImGui::BeginDisabled(!enabled);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(72.f));
        char inId[64]{};
        snprintf(inId, sizeof(inId), "##%s_in", id);
        if (ImGui::DragInt(inId, v, 5, (int)xcat::kHeliSpeedPctMin,
                           (int)xcat::kHeliSpeedPctMax, "%d%%",
                           ImGuiSliderFlags_AlwaysClamp)) {
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0.f, ui::Gap() * 0.5f);
        ImGui::TextDisabled("= %.0f px/s 巡航", 620.f * (float)(*v) / 100.f);
        const int presets[] = {100, 200, 300, 500};
        for (int p : presets) {
            char btnId[64]{};
            snprintf(btnId, sizeof(btnId), "%dX##%s_p%d", p / 100, id, p);
            ImGui::SameLine(0.f, ui::Gap() * 0.4f);
            if (ImGui::Button(btnId)) {
                *v = (int)xcat::ClampHeliSpeedPct(static_cast<uint32_t>(p));
                persistCore();
            }
        }
        ImGui::EndDisabled();
    };

    {
        xcat::ui::CardGuard card("##tab_home_hangup", "挂机");
        // 一行：自动进 + 分区下拉 + 角色槽（流程说明进 tooltip，避免 380 宽溢出）。
        if (xcat::ui::OptionCheckbox("自动进游戏", &autoEnter)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("分区 → 随机未满频道 → 选角");
        }
        {
            static uint64_t worldsTick = 0;
            static bool worldsFromSeed = false;
            static xcat::WorldsCacheEntry worlds[xcat::kWorldsCacheMax]{};
            static uint32_t worldsCount = 0;
            if (!ui.prefsBinDir.empty()) {
                uint64_t tick = 0;
                uint32_t n = 0;
                xcat::WorldsCacheEntry tmp[xcat::kWorldsCacheMax]{};
                const bool cacheOk = xcat::ReadWorldsCache(ui.prefsBinDir.c_str(), tmp,
                                                          xcat::kWorldsCacheMax, &n, &tick);
                if (cacheOk && n > 0) {
                    if (tick != worldsTick || worldsFromSeed) {
                        worldsTick = tick;
                        worldsFromSeed = false;
                        worldsCount = n;
                        memcpy(worlds, tmp, sizeof(tmp));
                    }
                } else if (worldsCount == 0 || worldsFromSeed) {
                    // 登录扫入前：用 world_names.tsv 预填。
                    // 只放 _Center1/2——TSV 有 1..17，经典版登录页通常只有这两格，全放易选飞。
                    const xcat::WorldNamesPack& wn =
                        xcat::GetSharedWorldNames(ui.prefsBinDir.c_str());
                    xcat::WorldNameCenterEntry seedAll[xcat::kWorldsCacheMax]{};
                    const uint32_t snAll =
                        xcat::WorldNamesListCenters(wn, seedAll, xcat::kWorldsCacheMax);
                    xcat::WorldNameCenterEntry seed[xcat::kWorldsCacheMax]{};
                    uint32_t sn = 0;
                    for (uint32_t i = 0; i < snAll && sn < xcat::kWorldsCacheMax; ++i) {
                        if (seedAll[i].worldId != 1 && seedAll[i].worldId != 2) continue;
                        seed[sn++] = seedAll[i];
                    }
                    if (sn > 0 && (!worldsFromSeed || sn != worldsCount)) {
                        worldsFromSeed = true;
                        worldsTick = 0;
                        worldsCount = sn;
                        memset(worlds, 0, sizeof(worlds));
                        for (uint32_t i = 0; i < sn; ++i) {
                            worlds[i].worldId = seed[i].worldId;
                            strncpy_s(worlds[i].name, seed[i].displayName, _TRUNCATE);
                        }
                    }
                }
                if (worldName[0] == '_') {
                    const xcat::WorldNamesPack& wn =
                        xcat::GetSharedWorldNames(ui.prefsBinDir.c_str());
                    const std::string pretty = xcat::WorldNamePreferDisplay(wn, worldName);
                    if (!pretty.empty() && pretty != worldName)
                        strncpy_s(worldName, pretty.c_str(), _TRUNCATE);
                }
            }

            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float slotW = AppDpi_Px(40.f);
            ImGui::SameLine(0.f, gap);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("分区");
            ImGui::SameLine(0.f, gap * 0.45f);

            if (worldsCount > 0) {
                char preview[96]{};
                const char* cachedForSel = worldName;
                for (uint32_t i = 0; i < worldsCount; ++i) {
                    if (worlds[i].worldId == worldId) {
                        cachedForSel = worlds[i].name;
                        break;
                    }
                }
                const std::string previewName =
                    WorldComboDisplayName(ui.prefsBinDir.c_str(), worldId, cachedForSel);
                if (worldId != 0 || !previewName.empty()) {
                    snprintf(preview, sizeof(preview), "%d  %s", worldId, previewName.c_str());
                } else {
                    snprintf(preview, sizeof(preview), "选择（%u）", worldsCount);
                }
                const float slotLabelW = ImGui::CalcTextSize("槽").x;
                const float comboW = (std::max)(
                    AppDpi_Px(72.f),
                    ImGui::GetContentRegionAvail().x - slotW - slotLabelW - gap * 2.f);
                ImGui::SetNextItemWidth(comboW);
                if (ImGui::BeginCombo("##ae_world", preview)) {
                    for (uint32_t i = 0; i < worldsCount; ++i) {
                        const std::string disp = WorldComboDisplayName(
                            ui.prefsBinDir.c_str(), worlds[i].worldId, worlds[i].name);
                        char label[96]{};
                        snprintf(label, sizeof(label), "%d  %s##w%d", worlds[i].worldId,
                                 disp.empty() ? "(无名称)" : disp.c_str(), worlds[i].worldId);
                        const bool sel = (worldId == worlds[i].worldId);
                        if (ImGui::Selectable(label, sel)) {
                            worldId = worlds[i].worldId;
                            strncpy_s(worldName, disp.empty() ? worlds[i].name : disp.c_str(),
                                      _TRUNCATE);
                            persistCore();
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (worldsFromSeed && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "预制预填（默认分区）；进登录世界列表后换成实际扫入分区");
                }
            } else {
                ImGui::TextDisabled("未扫入");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("无分区缓存，且 dataservice/world_names.tsv 也不可用");
                }
            }

            ImGui::SameLine(0.f, gap);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("槽");
            ImGui::SameLine(0.f, gap * 0.45f);
            ImGui::SetNextItemWidth(slotW);
            if (ImGui::DragInt("##ae_slot", &charSlot, 1, 1, 15)) persistCore();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("角色槽（选角界面从左到右，1 起）");
            }
        }
        if (xcat::ui::OptionCheckbox("自动测谎", &autoLie)) persistCore();
                ImGui::SameLine();
        {
            static std::string liePhase;
            static std::string lieErr;
            static int liePending = -1;
            static int lieAnswered = -1;
            static int lieInfraOk = -1;
            static int lieInfraFull = -1;
            static int lieEncodePng = -1;
            static int lieDirsOk = -1;
            static int lieQuizOk = -1;
            static DWORD lieUiTick = 0;
            const DWORD nowUi = GetTickCount();
            if (!lieUiTick || nowUi - lieUiTick > 800) {
                lieUiTick = nowUi;
                liePhase.clear();
                lieErr.clear();
                lieInfraOk = -1;
                lieInfraFull = -1;
                lieEncodePng = -1;
                lieDirsOk = -1;
                lieQuizOk = -1;
                if (!ui.prefsBinDir.empty()) {
                    const std::string stPath = ui.prefsBinDir + "\\state\\lie_ai\\status.txt";
                    std::ifstream sf(stPath);
                    if (sf) {
                        std::string line;
                        while (std::getline(sf, line)) {
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            if (line.rfind("phase=", 0) == 0) liePhase = line.substr(6);
                            if (line.rfind("lastError=", 0) == 0) lieErr = line.substr(10);
                            if (line.rfind("infraOk=", 0) == 0)
                                lieInfraOk = (line.substr(8) == "1") ? 1 : 0;
                            if (line.rfind("infraFull=", 0) == 0)
                                lieInfraFull = (line.substr(10) == "1") ? 1 : 0;
                            if (line.rfind("encodePng=", 0) == 0)
                                lieEncodePng = (line.substr(10) == "1") ? 1 : 0;
                            if (line.rfind("dirsOk=", 0) == 0)
                                lieDirsOk = (line.substr(7) == "1") ? 1 : 0;
                            if (line.rfind("quizOk=", 0) == 0)
                                lieQuizOk = (line.substr(7) == "1") ? 1 : 0;
                        }
                    }
                    const auto st = xcat::app::LieAiPump_GetStats();
                    liePending = st.pendingCount;
                    lieAnswered = st.sessionAnswered;
                    if (!st.lastError.empty() && lieErr.empty()) lieErr = st.lastError;
                }
            }
            if (lieInfraOk < 0) {
                ImGui::TextDisabled("就绪?");
            } else if (lieInfraOk) {
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.f),
                                   lieInfraFull == 1 ? "就绪" : "就绪(无PNG)");
            } else {
                ImGui::TextColored(ImVec4(1.f, 0.45f, 0.3f, 1.f), "未就绪");
            }
            ImGui::SameLine();
            if (!autoLie) {
                ImGui::TextDisabled("关");
            } else if (!liePhase.empty() && liePhase != "idle") {
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "%s", liePhase.c_str());
            } else {
                ImGui::TextDisabled("待命");
            }
            if (liePending >= 0) {
        ImGui::SameLine();
                ImGui::TextDisabled("泵%d/答%d", liePending, lieAnswered < 0 ? 0 : lieAnswered);
            }
            if (lieEncodePng >= 0 || lieDirsOk >= 0 || lieQuizOk >= 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("q%d png%d dir%d", lieQuizOk < 0 ? -1 : lieQuizOk,
                                   lieEncodePng < 0 ? -1 : lieEncodePng,
                                   lieDirsOk < 0 ? -1 : lieDirsOk);
            }
            if (!lieErr.empty()) {
                ImGui::SameLine();
                const std::string lieErrUi = SanitizeImGuiLogLine(lieErr);
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", lieErrUi.c_str());
            }
        }
        // 对齐枫星：无敌 + 随机换频同行（「攻击无CD」在吸怪 TAB）
        if (xcat::ui::OptionCheckbox("无敌", &invincible)) persistCore();
        ImGui::SameLine();
        if (ImGui::Button("随机换频", ImVec2(AppDpi_Px(100.f), 0.f))) {
            std::string err;
            if (TriggerManualRejoin(ui.prefsBinDir, /*requireInjected=*/true, &err)) {
                notify::PushLocal(/*Info*/ 0, "manual-rejoin", "随机换频已触发",
                                  "已下发换频命令。", 3500);
            } else {
                ImGui::OpenPopup("##manual_rejoin_fail");
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "立刻换到其它频道（排除当前）。\n"
                "直调发包，不弹游戏换频菜单。\n"
                "需已注入且在地图；测谎/冷却中会延后。\n"
                "热键 F10 等价。");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(F10)");
        if (ImGui::BeginPopup("##manual_rejoin_fail")) {
            ImGui::TextUnformatted("随机换频失败（未注入或写盘失败）");
            ImGui::EndPopup();
        }
        if (xcat::ui::OptionCheckbox("飞行", &fly)) persistCore();
        ImGui::SameLine();
        ImGui::TextDisabled("(F6)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "跟随鼠标飞行；需开无敌。\n"
                "推进间隔可在「调试」TAB 调整。\n"
                "Ctrl/Shift 暂停跟随。");
        }
        if (xcat::ui::OptionCheckbox("自动打怪", &autoCombat)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("游戏内 F5 切换；注入后勾选即时下发");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F5");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("出刀间隔");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(64.f));
        if (ImGui::DragInt("##hangup_atk_ms", &gUiSimpleCombatAttackIntervalMs, 1,
                           (int)xcat::kSimpleCombatAttackIntervalMinMs, 10000))
            persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "自动出刀间隔（%u–10000 ms，默认 %ums）。\n"
                "「攻击无CD」开/关都用这个值；下限 %u ms；过短易空砍/踢号。\n"
                "「攻击无CD」开关在「吸怪 快攻」TAB（需解锁）。",
                (unsigned)xcat::kSimpleCombatAttackIntervalMinMs,
                (unsigned)xcat::kSimpleCombatAttackIntervalDefaultMs,
                (unsigned)xcat::kSimpleCombatAttackIntervalMinMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("ms");
        ImGui::Indent(ui::Gap() * 1.2f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("追怪");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(132.f));
        {
            const char* approachItems[] = {"空中贴怪", "拟人模式", "站桩输出", "瞬移找怪", "关闭"};
            if (gUiApproachMode < 0 || gUiApproachMode > 4) gUiApproachMode = 0;
            if (ImGui::Combo("##f5_approach_mode", &gUiApproachMode, approachItems,
                             IM_ARRAYSIZE(approachItems))) {
                persistCore();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "单选追怪方式。\n"
                    "· 空中贴怪：悬停在怪旁出刀，需无敌，可穿层；默认。\n"
                    "· 拟人模式：同层走路贴近后 A 键出刀，不做跨层。\n"
                    "· 站桩输出：人原地砍。面前攻击盒（下方横向/竖直，默认 60×10）\n"
                    "  里有活怪就一直 A，不等贴脸、不空砍换怪、不禁锁。\n"
                    "  开打/切策略/换图/软重连后原地站「静止」ms 给吸物；0=不等；换怪不重新站。\n"
                    "  不自动吸怪；要吸请自己开「吸怪 快攻」TAB。\n"
                    "· 瞬移找怪：fill+Doing 贴到怪旁再出刀，可跨层。水平站距 X 与空中/拟人共用。\n"
                    "  可勾「每只怪打一下」：出一刀就切下一只（默认关）。\n"
                    "  选此项：出过刀后强制「主动软重连」清加速 FLAG（出刀后关 F5 仍走完这一轮；\n"
                    "  没出过刀才不计时，满包可直接卖）。\n"
                    "· 关闭：不追怪（站桩，够得着才砍）。");
            }
        }
        if (gUiApproachMode == 2) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("静止");
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (ImGui::DragInt("##f5_hiraishin_loot_hold_ms", &gUiHiraishinLootHoldMs, 50,
                               (int)xcat::kHiraishinLootHoldMinMs,
                               (int)xcat::kHiraishinLootHoldMaxMs)) {
                gUiHiraishinLootHoldMs = (int)xcat::ClampHiraishinLootHoldMs(
                    static_cast<uint32_t>(gUiHiraishinLootHoldMs < 0 ? 0 : gUiHiraishinLootHoldMs));
                persistCore();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "开打/切到站桩输出/换图/软重连后原地站这么久再出刀，给吸物。\n"
                    "默认 0=不等。换怪不重新站。中途改滑条不会重开这段等待。");
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("ms");
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("横向");
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (ImGui::DragInt("##f5_hiraishin_front_dx", &gUiHiraishinFrontDx, 10,
                               (int)xcat::kHiraishinFrontDxMin, (int)xcat::kHiraishinFrontDxMax)) {
                gUiHiraishinFrontDx = (int)xcat::ClampHiraishinFrontDx(
                    static_cast<uint32_t>(gUiHiraishinFrontDx < 0 ? 0 : gUiHiraishinFrontDx));
                persistCore();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "人↔怪 AbsPos 横向半宽（px）。默认 60。0=该轴不限。\n"
                    "叠怪吸过边会停刀，可加大。砍太远会空刀或踢。");
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("px");
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("竖直");
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (ImGui::DragInt("##f5_hiraishin_front_dy", &gUiHiraishinFrontDy, 5,
                               (int)xcat::kHiraishinFrontDyMin, (int)xcat::kHiraishinFrontDyMax)) {
                gUiHiraishinFrontDy = (int)xcat::ClampHiraishinFrontDy(
                    static_cast<uint32_t>(gUiHiraishinFrontDy < 0 ? 0 : gUiHiraishinFrontDy));
                persistCore();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "人↔怪 AbsPos 竖直半高（px）。默认 10。0=该轴不限。\n"
                    "AbsPos：更大 Y = 更高。叠怪吸过边会停刀，可加大。砍太远会空刀或踢。");
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("px");
        }
        if (gUiApproachMode == 3) {
            if (xcat::ui::OptionCheckbox("每只怪打一下", &teleportOneHit)) persistCore();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "仅「瞬移找怪」生效。默认关。\n"
                    "开：当前这只出一刀就禁锁，按原来的选怪（可跨层瞬移）切下一只。\n"
                    "不走「每只怪打几刀」的攻击盒外换怪，也不因活怪少于 3 只停刀。\n"
                    "关：沿用打死 / 早切 / 空刀才换怪。\n"
                    "与「每只怪打几刀」同时开时本项优先。\n"
                    "生效核对：combat.log 出现 SetTeleportOneHit 1 与 switch reason=tp_one_hit");
            }
        }
        ImGui::Unindent(ui::Gap() * 1.2f);
        // 倍率在下方「飞行速度」卡调。

        // 自定义站距：水平 X 给空中贴怪 / 拟人 / 瞬移找怪共用；Y 只给空中贴怪。
        // 吸怪落点在「吸怪」TAB。
        const bool standOffHost =
            (gUiApproachMode == 0 || gUiApproachMode == 1 || gUiApproachMode == 3);
        const bool standOffYHost = (gUiApproachMode == 0);
        ImGui::BeginDisabled(!standOffHost);
        if (xcat::ui::OptionCheckbox("自定义站距", &standOffCustom)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "自己指定人↔怪的水平间距 X（空中贴怪 / 拟人 / 瞬移找怪共用）。\n"
                "垂直 Y 只给空中贴怪悬停；拟人/瞬移贴台，不吃 Y。\n"
                "吸怪落点请到「吸怪 快攻」TAB 调，两套互不影响。\n"
                "不勾 = 用内置近战最优值（X=%u，Y=%d）。\n"
                "勾上后：命中率归你调——站太远打不到，程序不会替你拦。",
                (unsigned)xcat::kCombatStandOffXDefault, (int)xcat::kCombatStandOffYDefault);
        }
        ImGui::SameLine(0.f, ui::Gap() * 1.2f);
        ImGui::BeginDisabled(!standOffHost || !standOffCustom);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("X");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(64.f));
        if (ImGui::DragInt("##f5_standoff_x", &standOffX, 1, (int)xcat::kCombatStandOffXMin,
                           (int)xcat::kCombatStandOffXMax)) {
            standOffX = (int)xcat::ClampCombatStandOffX(
                static_cast<uint32_t>(standOffX < 0 ? 0 : standOffX));
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "水平间距（px，%u–%u）。空中贴怪 / 拟人 / 瞬移找怪共用。\n"
                "近战 20–45 命中最好；弓/弩/法师按自己的射程填。\n"
                "瞬移/拟人地面落点会把 X 夹进 %u–%u，避免贴怪心或 hop 过远。",
                (unsigned)xcat::kCombatStandOffXMin, (unsigned)xcat::kCombatStandOffXMax,
                (unsigned)xcat::kCombatTeleportStandOffMin,
                (unsigned)xcat::kCombatTeleportStandOffMax);
        }
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::BeginDisabled(!standOffYHost || !standOffCustom);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Y");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(64.f));
        if (ImGui::DragInt("##f5_standoff_y", &standOffY, 1, (int)xcat::kCombatStandOffYMin,
                           (int)xcat::kCombatStandOffYMax)) {
            standOffY = (int)xcat::ClampCombatStandOffY(static_cast<int32_t>(standOffY));
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                 ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "垂直间距（px，%d–%d）。只给空中贴怪。AbsPos：**更大 Y = 更高**。\n"
                "相对怪心；正数 = 人在怪上方。拟人/瞬移贴台，不吃这项。\n"
                "0 = 与目标同高，近战实测最优。",
                (int)xcat::kCombatStandOffYMin, (int)xcat::kCombatStandOffYMax);
        }
        ImGui::EndDisabled();  // Y
        ImGui::EndDisabled();  // X
        ImGui::EndDisabled();  // host

        // 防贴脸退避：半径直接借上面那组 X/Y，所以必须先开自定义站距才有意义。
        ImGui::BeginDisabled(gUiApproachMode != 0 || !standOffCustom);
        if (xcat::ui::OptionCheckbox("防贴脸退避", &antiHug)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "任何怪贴到身边就自动退开，不管当前锁的是哪只。\n"
                "安全距离参考上面的 X/Y，但会各自夹进合理区间（水平 40~80、纵向 45~90）：\n"
                "要躲的挥弓框实测只有 30 宽，留 80 就够；照搬大站距会把退避逼得乱窜。\n"
                "给远程职业解「群怪冲脸→被迫贴身平砍」用；退不开时会先不出刀，\n"
                "但最多憋 1.5 秒就照打，不会站着挨揍。\n"
                "退避解不出来 / 开了 30 秒打不出刀，会自动停用并写进 combat.log，\n"
                "行为退回没勾时一模一样。取消勾选立即生效。");
        }
        ImGui::EndDisabled();

        // 站立伪装：空中贴怪腾空出刀；站桩输出闪魂落点不一定有台。
        ImGui::BeginDisabled(gUiApproachMode != 0 && gUiApproachMode != 2);
        if (xcat::ui::OptionCheckbox("站立伪装", &groundSpoof)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "出刀那一瞬让引擎以为你踩在台上，出完立刻还原。\n"
                "给「腾空中放不出技能」的职业用（法师系一类技能会检查站立态）。\n"
                "空中贴怪应开。站桩输出闪到怪脚边时落点常无台，也建议开。\n"
                "拟人地面追怪不需要。只有出刀那几微秒生效。\n"
                "会让攻击包动作从腾空斩变成站立斩；先开一小会儿看 combat.log 的 sp/b1。");
        }
        ImGui::EndDisabled();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("怪物读取速度");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(72.f));
        if (ImGui::DragInt("##mob_scan_ms", &mobScanIntervalMs, 1,
                           (int)xcat::kMobScanIntervalMinMs,
                           (int)xcat::kMobScanIntervalMaxMs)) {
            mobScanIntervalMs = (int)xcat::ClampMobScanIntervalMs(
                static_cast<uint32_t>(mobScanIntervalMs < 0 ? 0 : mobScanIntervalMs));
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "打怪开启时刷新怪物列表的周期（%u–%u ms，默认 %u）。\n"
                "数值越小越快看见新刷怪/尸体消失；换怪时会额外按需刷一帧。\n"
                "建议 15–30；过低更吃 CPU。未开打怪时仍用较慢闲置扫描。",
                (unsigned)xcat::kMobScanIntervalMinMs,
                (unsigned)xcat::kMobScanIntervalMaxMs,
                (unsigned)xcat::kMobScanIntervalDefaultMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::TextDisabled("ms");

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.2f));
        // 对齐枫星：喝药并入挂机卡，不单独占卡片
        if (xcat::ui::OptionCheckbox("自动加血", &hpPotion)) persistCore();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##hpTh", &hpThresholdPct, 1, 1, 99)) persistCore();
        ImGui::SameLine();
        ImGui::TextUnformatted("%");
        ImGui::SameLine(0.f, ui::Gap() * 1.2f);
        if (xcat::ui::OptionCheckbox("自动加蓝", &mpPotion)) persistCore();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##mpTh", &mpThresholdPct, 1, 1, 99)) persistCore();
        ImGui::SameLine();
        ImGui::TextUnformatted("%");

        // 对齐枫星：守护模式嵌在挂机卡底部，不单独成卡
        {
            const hangup_schedule::Snapshot snap = hangup_schedule::GetSnapshot();
            if (xcat::ui::OptionCheckbox("守护模式", &watchdog)) persistCore();
            ImGui::SetItemTooltip(
                "与挂机时段正交。开启后：已注入时，\n"
                "经验停滞 / 进程退出 / 假死 / 踢线 / 长期未进图 → 干净重拉\n"
                "（杀 Classic→等退出→settle→一键冷启）。\n"
                "\n"
                "「无经验」秒数（N）用途：\n"
                "· 进图后：经验/状态停滞约 N 秒（+确认）重拉\n"
                "· 冷启未进图最坏约 4×N + 确认 才重拉：\n"
                "  主门 2×N（进程起来后）+ 次门 N（已握手）\n"
                "  + 未进图计时 N + 确认 1 拍\n"
                "· 踢线：进图武装后等待 5 秒再干净重拉（绕过冷却；可看踢出画面）");
            ImGui::SameLine();
            ImGui::TextDisabled("无经验");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (!watchdog) ImGui::BeginDisabled();
            if (ImGui::DragInt("##wd_noexp", &noExpSec, 1,
                               static_cast<int>(xcat::kWatchdogNoExpSecMin),
                               static_cast<int>(xcat::kWatchdogNoExpSecMax))) {
                noExpSec = static_cast<int>(
                    xcat::ClampWatchdogNoExpSec(static_cast<uint32_t>(noExpSec)));
                persistCore();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "N=无经验秒（当前 %d）。\n"
                    "进图后停滞门槛 ≈ N；\n"
                    "从未进图最坏 ≈ 4×N + 确认（默认 N=%u → 约 %u 秒量级）。",
                    noExpSec, xcat::kWatchdogNoExpSecDefault,
                    xcat::kWatchdogNoExpSecDefault * 4u);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("秒");
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::TextDisabled("重启冷却");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (ImGui::DragInt("##wd_cd", &cooldownSec, 1,
                               static_cast<int>(xcat::kWatchdogCooldownSecMin),
                               static_cast<int>(xcat::kWatchdogCooldownSecMax))) {
                cooldownSec = static_cast<int>(
                    xcat::ClampWatchdogCooldownSec(static_cast<uint32_t>(cooldownSec)));
                persistCore();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("秒");
            if (!watchdog) ImGui::EndDisabled();
            if (watchdog) {
                ImGui::SameLine(0.f, ui::Gap() * 1.2f);
                ImGui::TextDisabled("%s", hangup_schedule::FormatWatchdogTimerText(snap).c_str());
            }
        }

        if (xcat::ui::OptionCheckbox("软重连试连", &softLoginProbe)) persistCore();
        ImGui::SetItemTooltip(
            "采证+软重进（默认开）。断线后试 ConnectLogin、卸弹窗、重跑自动进游戏；\n"
            "观察窗内推迟守护一切干净重拉（踢线/无经验/心跳等），\n"
            "须等软路径完全失败（或进程已死）才交由守护重拉；进图成功则吞 disconnectSeq。\n"
            "同时只读采样 Galaxy_* 写 galaxy_token.log（仅 len+前缀，不清登录态）。\n"
            "日志 soft_login.log / galaxy_token.log。\n"
            "亦可用 soft_login_probe.on / SOFT_LOGIN_PROBE=1\n"
            "（或旧 marker galaxy_token_probe.on 仅采证）。\n"
            "秒数/出刀闸在「吸怪 快攻」TAB「快攻」卡「主动软重连」「出刀软重连」。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_fly_speed", "飞行速度");
        speedRow("F6 手动飞", "home_fly_speed", &gUiManualFlySpeedPct,
                 "F6 手动飞的速度倍率，与 F5 那份各存各的。\n"
                 "100% = 巡航 620 px/s。换旋翼前的开环实现等效约 1600（≈2.6X），\n"
                 "所以默认给 3X，别按 1X 去对旧手感。",
                 true);
        speedRow("F5 滑翔", "home_combat_speed", &gUiFlySpeedPct,
                 "F5 空中贴怪 + 自动赶路共用（都是「自动飞」）。\n"
                 "默认 500%；拟人 / 站桩输出 / 关闭时置灰。",
                 gUiApproachMode == 0);
        ImGui::TextDisabled(
            "范围 %u–%u%%。只放大「意图」速度；撞墙预刹 / 位置包线 / 坠落自救不跟着缩。\n"
            "实测已验到 5X；更高属外推。推进间隔仍在「调试 → 飞行调试」。",
            xcat::kHeliSpeedPctMin, xcat::kHeliSpeedPctMax);
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_pickup", "拾物");
        // 0=关 1=脚下 2=宠吸 3=人物直吸 4=变态宠吸（单选互斥；2/3/4 共用 vacuumW/H）
        enum : int { kLootOff = 0, kLootFoot = 1, kLootPet = 2, kLootChar = 3, kLootNativeVac = 4 };
        static bool petLootLoaded = false;
        static int lootMode = kLootOff;
        static bool pickupBlacklist = false;
        static bool highValuePriority = true;
        static bool dropSnapLand = true;
        static bool dropAccelFall = false;
        static int lootIntervalMs = static_cast<int>(xcat::kPetLootIntervalDefaultMs);
        static int lootBurstPerTick = static_cast<int>(xcat::kPetLootBurstDefault);
        static float lootVacW = xcat::kPetLootVacuumWDefault;
        static float lootVacH = xcat::kPetLootVacuumHDefault;
        static char blacklistKw[256]{};
        static uint64_t petLootTick = 0;
        static bool petLootSaveFailed = false;

        auto modeFromFlags = [](bool pet, bool foot, bool mapVac, bool charVac,
                                bool nativeVac) -> int {
            // 变态宠吸 > 宠吸 > 人物直吸 > 脚边（与 PetLootNormalize 一致）
            if (nativeVac) return kLootNativeVac;
            if (pet || mapVac) return kLootPet;
            if (charVac) return kLootChar;
            if (foot) return kLootFoot;
            return kLootOff;
        };

        auto parseBlacklistToCfg = [&](xcat::PetLootConfig& cfg) {
            // 开关只控 skipFilterEnabled；关键词始终落盘，避免取消勾选把输入框规则冲掉。
            cfg.skipFilterEnabled = pickupBlacklist ? 1u : 0u;
            if (!blacklistKw[0]) {
                // 空框：勿写 skipCount=0 冲掉盘上/默认「箭矢 彈丸」。
                // 勾选启用时回填默认关键词；关闭过滤时保留 cfg 里 Read 到的规则。
                if (pickupBlacklist) {
                    cfg.skipRuleCount = 0;
                    auto addKw = [&](const char* key) {
                        if (cfg.skipRuleCount >= (uint32_t)xcat::kPetLootMaxSkipRules) return;
                        xcat::PetLootSkipRule& r = cfg.skipRules[cfg.skipRuleCount++];
                        r = {};
                        r.enabled = 1;
                        strncpy_s(r.nameKey, key, _TRUNCATE);
                    };
                    addKw("箭矢");
                    addKw("彈丸");
                    snprintf(blacklistKw, sizeof(blacklistKw), "箭矢 彈丸");
                }
                return;
            }
            cfg.skipRuleCount = 0;
            std::vector<std::string> toks;
            xcat::SplitKeywordList(blacklistKw, toks);
            for (const std::string& tok : toks) {
                if (tok.empty()) continue;
                if (cfg.skipRuleCount >= (uint32_t)xcat::kPetLootMaxSkipRules) break;
                xcat::PetLootSkipRule& r = cfg.skipRules[cfg.skipRuleCount++];
                r = {};
                r.enabled = 1;
                strncpy_s(r.nameKey, tok.c_str(), _TRUNCATE);
                char* end = nullptr;
                const unsigned long v = strtoul(tok.c_str(), &end, 10);
                if (end && *end == '\0' && v > 0 && v < 0x7FFFFFFFul) r.itemId = (uint32_t)v;
            }
        };

        auto persistPetLoot = [&]() {
            petLootSaveFailed = false;
            if (ui.prefsBinDir.empty()) return;
            const bool footLoot = (lootMode == kLootFoot);
            const bool petLoot = (lootMode == kLootPet);
            const bool nativeVacLoot = (lootMode == kLootNativeVac);
            const bool charLoot =
                xcat::kPetLootCharVacUserEnabled && (lootMode == kLootChar);
            xcat::PetLootConfig cfg{};
            (void)xcat::ReadPetLoot(ui.prefsBinDir.c_str(), cfg);
            cfg.enabled = petLoot ? 1u : 0u;
            cfg.footEnabled = footLoot ? 1u : 0u;
            cfg.nativeVacEnabled = nativeVacLoot ? 1u : 0u;
            cfg.charVacEnabled = charLoot ? 1u : 0u;
            // 关宠时清 mapVacuum，避免旧 ini 残留误导；vacuumW/H 始终保留用户设定
            cfg.mapVacuumEnabled = petLoot ? 1u : 0u;
            cfg.intervalMs = xcat::PetLootClampIntervalMs(
                lootIntervalMs > 0 ? static_cast<uint32_t>(lootIntervalMs) : 0u);
            lootIntervalMs = static_cast<int>(cfg.intervalMs);
            cfg.burstPerTick = xcat::PetLootClampBurstPerTick(
                lootBurstPerTick > 0 ? static_cast<uint32_t>(lootBurstPerTick) : 0u);
            lootBurstPerTick = static_cast<int>(cfg.burstPerTick);
            cfg.vacuumW = lootVacW;
            cfg.vacuumH = lootVacH;
            xcat::PetLootNormalize(cfg);
            lootVacW = cfg.vacuumW;
            lootVacH = cfg.vacuumH;
            parseBlacklistToCfg(cfg);
            cfg.highValuePriority = highValuePriority ? 1u : 0u;
            cfg.dropSnapLand = dropSnapLand ? 1u : 0u;
            cfg.dropAccelFall = dropAccelFall ? 1u : 0u;
            // scrollDropNotify：入口在调试 TAB，此处保留盘上值（Read 已载入）
            xcat::PetLootNormalize(cfg);
            // WritePetLoot 内部用新 tick 落盘，必须回写到 petLootTick，否则下帧误判
            // disk≠ui → 清空 blacklistKw 再从（可能已被旧逻辑写空的）规则重建。
            cfg.writeTickMs = GetTickCount64();
            if (xcat::WritePetLoot(ui.prefsBinDir.c_str(), cfg)) {
                petLootTick = cfg.writeTickMs;
                xcat::log::Ok("App",
                              "已下发 pet_loot：脚边=%d 宠吸=%d 人物=%d 变态=%d 间隔=%ums 连吸=%u "
                              "vac=%.0fx%.0f 黑名单=%d rules=%u highValue=%d snapLand=%d "
                              "accelFall=%d",
                              footLoot ? 1 : 0, petLoot ? 1 : 0, charLoot ? 1 : 0,
                              nativeVacLoot ? 1 : 0, cfg.intervalMs, cfg.burstPerTick, cfg.vacuumW,
                              cfg.vacuumH, pickupBlacklist ? 1 : 0, cfg.skipRuleCount,
                              cfg.highValuePriority ? 1 : 0, cfg.dropSnapLand ? 1 : 0,
                              cfg.dropAccelFall ? 1 : 0);
            } else {
                petLootSaveFailed = true;
                xcat::log::Warn("App", "写入 user.ini [pet_loot] 失败");
            }
        };

        auto notifyLootMode = [&](int prev, int next) {
            if (prev == next) return;
            const char* body = "已关闭自动拾物。";
            unsigned kind = 0;
            if (next == kLootFoot) {
                kind = 2;
                body = "已切换为脚下拾取（与宠吸/人物直吸互斥）。";
            } else if (next == kLootPet) {
                kind = 2;
                body = "已切换为宠吸（官方 ByPet 扩盒；范围与人物直吸共用，尺寸可调，不切路径）。";
            } else if (next == kLootNativeVac) {
                kind = 2;
                body = "已切换为变态宠吸（常驻扩盒，原生宠自己捡；不抢出刀）。";
            } else if (next == kLootChar) {
                kind = 2;
                body = xcat::kPetLootCharVacUserEnabled
                           ? "已切换为人物直吸（官方送包，不靠宠物；范围与宠吸共用，默认 1000×1000）。"
                           : "人物直吸暂未开放。";
            }
            notify::PushLocal(kind, "petloot-mode", "拾物模式", body, 4200);
        };

        if (!ui.prefsBinDir.empty()) {
            xcat::PetLootConfig disk{};
            if (xcat::ReadPetLoot(ui.prefsBinDir.c_str(), disk)) {
                if (!petLootLoaded || disk.writeTickMs != petLootTick) {
                    const bool wasLoaded = petLootLoaded;
                    const bool diskPet = disk.enabled != 0;
                    const bool diskFoot = disk.footEnabled != 0;
                    const bool diskMap = disk.mapVacuumEnabled != 0;
                    // ReadPetLoot 已 Normalize：用户面关闭时 diskChar 恒为 0
                    const bool diskChar =
                        xcat::kPetLootCharVacUserEnabled && disk.charVacEnabled != 0;
                    const bool diskNative = disk.nativeVacEnabled != 0;
                    const int activeModes = (diskNative ? 1 : 0) + (diskPet || diskMap ? 1 : 0) +
                                            (diskChar ? 1 : 0) + (diskFoot ? 1 : 0);
                    const bool conflict = activeModes > 1;
                    lootMode = modeFromFlags(diskPet, diskFoot, diskMap, diskChar, diskNative);
                    lootIntervalMs = static_cast<int>(
                        xcat::PetLootClampIntervalMs(disk.intervalMs));
                    lootBurstPerTick = static_cast<int>(
                        xcat::PetLootClampBurstPerTick(disk.burstPerTick));
                    lootVacW = disk.vacuumW;
                    lootVacH = disk.vacuumH;
                    pickupBlacklist = disk.skipFilterEnabled != 0;
                    highValuePriority = disk.highValuePriority != 0;
                    dropSnapLand = disk.dropSnapLand != 0;
                    dropAccelFall = disk.dropAccelFall != 0;
                    blacklistKw[0] = '\0';
                    size_t off = 0;
                    for (uint32_t i = 0; i < disk.skipRuleCount; ++i) {
                        const xcat::PetLootSkipRule& r = disk.skipRules[i];
                        if (!r.enabled) continue;
                        const char* piece = r.nameKey[0] ? r.nameKey : nullptr;
                        char idbuf[32]{};
                        if (!piece && r.itemId) {
                            snprintf(idbuf, sizeof(idbuf), "%u", r.itemId);
                            piece = idbuf;
                        }
                        if (!piece || !piece[0]) continue;
                        const size_t need = strlen(piece) + (off ? 1u : 0u);
                        if (off + need + 1 >= sizeof(blacklistKw)) break;
                        if (off) blacklistKw[off++] = ' ';
                        memcpy(blacklistKw + off, piece, strlen(piece));
                        off += strlen(piece);
                        blacklistKw[off] = '\0';
                    }
                    petLootTick = disk.writeTickMs;
                    petLootLoaded = true;
                    // 冲突、或旧「小盒宠吸 / 近图缺 enabled」→ 统一落成宠吸=近图
                    if (conflict ||
                        (!diskNative && ((diskPet && !diskMap) || (diskMap && !diskPet)))) {
                        if (conflict && wasLoaded) {
                            notify::PushLocal(/*Warning*/ 2, "petloot-mutex-disk", "拾物互斥",
                                             "配置里多种拾物模式冲突，已按优先级保留一种。", 5000);
                        }
                        persistPetLoot();
                    }
                }
            } else if (!petLootLoaded) {
                // 无盘 / 读失败：UI 也铺上默认黑名单，避免随后 persist 把 skipCount 写成 0
                petLootLoaded = true;
                pickupBlacklist = true;
                snprintf(blacklistKw, sizeof(blacklistKw), "箭矢 彈丸");
            }
        }

        {
            // 用户面禁用人物直吸：旧会话/静态态若仍停在该档，落回关闭并写盘清 ini。
            if (!xcat::kPetLootCharVacUserEnabled && lootMode == kLootChar) {
                const int prevMode = lootMode;
                lootMode = kLootOff;
                notifyLootMode(prevMode, lootMode);
                persistPetLoot();
            }
            const int prevMode = lootMode;
            bool changed = false;
            if (xcat::ui::OptionRadioButton("关闭", &lootMode, kLootOff)) changed = true;
            ImGui::SameLine();
            if (xcat::ui::OptionRadioButton("脚下拾取", &lootMode, kLootFoot)) changed = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "自动触发游戏原生脚下拾取（DropPool.TryPickUpDrop）。\n"
                    "不扩盒、不清闸、不盖黑名单戳；范围与手捡一致（约 50×60）。");
            }
            ImGui::SameLine();
            if (xcat::ui::OptionRadioButton("宠吸", &lootMode, kLootPet)) changed = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "宠物官方吸物（扩 .rdata 矩形 → TryPickUpDrop / ByPet）。\n"
                    "范围只改盒子大小，不会改走人吸。默认 %.0f×%.0f。",
                    xcat::kPetLootVacuumWDefault, xcat::kPetLootVacuumHDefault);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!xcat::kPetLootCharVacUserEnabled);
            if (xcat::ui::OptionRadioButton("人物直吸", &lootMode, kLootChar)) changed = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_DelayNormal)) {
                if (!xcat::kPetLootCharVacUserEnabled) {
                    ImGui::SetTooltip("人物直吸暂未开放（实现保留，稍后上架）。");
                } else {
                    ImGui::SetTooltip(
                        "官方 SendDropPickUpRequest（角色坐标，不靠宠）。\n"
                        "范围与宠吸共用、用户自调；过大可能空 Send，自负。默认 %.0f×%.0f。",
                        xcat::kPetLootVacuumWDefault, xcat::kPetLootVacuumHDefault);
                }
            }
            ImGui::SameLine();
            if (xcat::ui::OptionRadioButton("变态宠吸", &lootMode, kLootNativeVac)) changed = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "启用时按下面「吸物范围」写一次 ByPet 盒子，关掉还原 50×60。\n"
                    "原生宠物自己捡，主线程不再调吸物，不挡 F5 出刀。\n"
                    "与脚下 / 宠吸 / 人物直吸互斥。盒过大可能空 Send / 掐线。");
            }
            if (changed) {
                if (!xcat::kPetLootCharVacUserEnabled && lootMode == kLootChar)
                    lootMode = kLootOff;
                notifyLootMode(prevMode, lootMode);
                persistPetLoot();
            }
        }
        {
            ImGui::BeginDisabled(lootMode == kLootNativeVac);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("吸物间隔");
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::SetNextItemWidth(AppDpi_Px(72.f));
            if (ImGui::DragInt("##loot_ms", &lootIntervalMs, 1,
                               static_cast<int>(xcat::kPetLootIntervalMinMs),
                               static_cast<int>(xcat::kPetLootIntervalMaxMs)))
                persistPetLoot();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "脚边 / 宠吸 / 人物直吸共用 Tick 间隔（%u–%u ms，默认 %u）。\n"
                    "打怪同开时建议 ≥200；过短易抢主线程泵。",
                    (unsigned)xcat::kPetLootIntervalMinMs,
                    (unsigned)xcat::kPetLootIntervalMaxMs,
                    (unsigned)xcat::kPetLootIntervalDefaultMs);
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("ms");
            ImGui::SameLine(0.f, ui::Gap() * 0.75f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("连吸");
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::SetNextItemWidth(AppDpi_Px(48.f));
            if (ImGui::DragInt("##loot_burst", &lootBurstPerTick, 1,
                               static_cast<int>(xcat::kPetLootBurstMin),
                               static_cast<int>(xcat::kPetLootBurstHardCap)))
                persistPetLoot();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "每拍连调官方吸物次数（%u–%u，默认 %u）。\n"
                    "越大同秒吸越多，发包更密，打怪同开时尖峰更高。",
                    (unsigned)xcat::kPetLootBurstMin, (unsigned)xcat::kPetLootBurstHardCap,
                    (unsigned)xcat::kPetLootBurstDefault);
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("次/拍");
            ImGui::EndDisabled();
        }
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("吸物范围");
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::BeginDisabled(lootMode != kLootPet && lootMode != kLootChar &&
                                 lootMode != kLootNativeVac);
            const float vacMaxW = xcat::kPetLootVacuumMax;
            const float vacMaxH = xcat::kPetLootVacuumMax;
            ImGui::SetNextItemWidth(AppDpi_Px(72.f));
            if (ImGui::DragFloat("##loot_vac_w", &lootVacW, 1.f, xcat::kPetLootVacuumMin, vacMaxW,
                                 "%.0f"))
                persistPetLoot();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                if (lootMode == kLootChar) {
                    ImGui::SetTooltip(
                        "人物直吸宽度（%.0f–%.0f，默认 %.0f）。用户自调；过大可能空 Send。",
                        xcat::kPetLootVacuumMin, xcat::kPetLootVacuumMax,
                        xcat::kPetLootVacuumWDefault);
                } else {
                    ImGui::SetTooltip(
                        "宠吸真空宽度（%.0f–%.0f，默认 %.0f）。只扩 ByPet 矩形。",
                        xcat::kPetLootVacuumMin, xcat::kPetLootVacuumMax,
                        xcat::kPetLootVacuumWDefault);
                }
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("宽");
            ImGui::SameLine(0.f, ui::Gap() * 0.55f);
            ImGui::SetNextItemWidth(AppDpi_Px(72.f));
            if (ImGui::DragFloat("##loot_vac_h", &lootVacH, 1.f, xcat::kPetLootVacuumMin, vacMaxH,
                                 "%.0f"))
                persistPetLoot();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                if (lootMode == kLootChar) {
                    ImGui::SetTooltip(
                        "人物直吸高度（%.0f–%.0f，默认 %.0f）。用户自调。",
                        xcat::kPetLootVacuumMin, xcat::kPetLootVacuumMax,
                        xcat::kPetLootVacuumHDefault);
                } else {
                    ImGui::SetTooltip(
                        "宠吸真空高度（%.0f–%.0f，默认 %.0f）。只扩 ByPet 矩形。",
                        xcat::kPetLootVacuumMin, xcat::kPetLootVacuumMax,
                        xcat::kPetLootVacuumHDefault);
                }
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("高");
            ImGui::EndDisabled();
        }
        if (xcat::ui::OptionCheckbox("高价值优先吸", &highValuePriority)) persistPetLoot();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "地图出现装备 / 卷軸时优先拾取（可打断出刀）。\n"
                "对应背包栏已满则跳过该件；栏未满则缩短吸物间隔尽快吸起。\n"
                "仅「宠吸」路径生效；变态宠吸不打断出刀。默认开启。");
        }
        if (xcat::ui::OptionCheckbox("掉落瞬落", &dropSnapLand)) persistPetLoot();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "飞行中的掉落立刻落到落点，可马上捡。\n"
                "会把当前位置跟到落地坐标，不是只改状态。\n"
                "黑名单件不碰；与拾物档位无关。默认开启。");
        }
        ImGui::SameLine();
        if (xcat::ui::OptionCheckbox("掉落加速", &dropAccelFall)) persistPetLoot();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "抛物线还在，只是更快落地（弧线更短）。\n"
                "与「掉落瞬落」同时勾选时瞬落优先。\n"
                "黑名单件不碰；与拾物档位无关。默认关闭。");
        }
        if (xcat::ui::OptionCheckbox("启用拾取黑名单", &pickupBlacklist)) persistPetLoot();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##bl",
                                     "itemId/关键词（空格分隔；金币填 2147483647；原生宠也会拦，与拾物档位无关）",
                                     blacklistKw, sizeof(blacklistKw))) {
            // debounce on deactivate / checkbox
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistPetLoot();
        if (petLootSaveFailed)
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [pet_loot] 失败");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_combat", "打怪设置");

        ImGui::BeginDisabled();
        smartInterval = false;
        xcat::ui::OptionCheckbox("智能间隔", &smartInterval);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("当前暂不可用");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("智能间隔暂时未接入，先用「挂机 → 出刀间隔」固定值。");
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("出刀站距");
        ImGui::TextDisabled("水平 X 与首页「自定义站距」共用（空中贴怪 / 拟人 / 瞬移找怪）");
        ImGui::TextDisabled("Y 只给空中贴怪；拟人/瞬移贴台。命中带约 站距×1.55");
        ImGui::Spacing();

        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("站距 X");
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(AppDpi_Px(72.f));
            ImGui::DragInt("##combat_standoff", &teleportStandOff, 1, 12, 200);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "只读镜像。请到首页挂机卡勾「自定义站距」改 X。\n"
                    "空中贴怪 / 拟人 / 瞬移找怪共用这一格。\n"
                    "地面落点会把 X 夹进 %u–%u px（禁贴怪心）。",
                    (unsigned)xcat::kCombatTeleportStandOffMin,
                    (unsigned)xcat::kCombatTeleportStandOffMax);
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::TextDisabled("px · 首页自定义站距 X");
            (void)teleportMinDx;
        }

        ImGui::Spacing();
        {
            bool clusterPri = clusterWeight != 0;
            if (xcat::ui::OptionCheckbox("群怪优先", &clusterPri)) {
                clusterWeight = clusterPri ? 1 : 0;
                persistCore();
                // 写盘后回读核对：避免 UI 勾了但 user.ini 仍是 0（DLL 永远不生效）。
                xcat::PayloadControl verify{};
                if (!ui.prefsBinDir.empty() &&
                    xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                    const int got = verify.clusterWeight != 0 ? 1 : 0;
                    if (got != (clusterWeight ? 1 : 0)) {
                        xcat::log::Warn("App", "群怪优先写盘核对失败 want=%d got=%d",
                                        clusterWeight ? 1 : 0, got);
                    } else {
                        xcat::log::Ok("App", "群怪优先已下发 %d（acquire 应见 cluster≥0）", got);
                    }
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("500px 内先打密堆");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "开：500px 内先打周围更密的堆（密度看同层 250px 活怪数）。\n"
                    "脚边独怪不会压过半径内的堆；超过 500px 仍打近的，不去追全图。\n"
                    "空中贴怪/瞬移找怪可跨层比密度；拟人仍只在同层比。\n"
                    "关：只按距离选最近可打怪（同层有怪就不跨层）\n"
                    "注意：已锁定交手中不会中途改锁——只影响下一次选怪\n"
                    "生效核对：combat.log 出现 SetClusterPriority 1，acquire 的 cluster>=1");
            }
        }

        ImGui::Spacing();
        {
            if (xcat::ui::OptionCheckbox("每只怪打几刀", &hitRotateOn)) persistCore();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "服务器短时间连续命中同一只怪会风控。开：确认自己打中同一只怪 N 次后，\n"
                    "改打攻击盒外、离你最近的活怪（不飞全图）。空刀不计；别人的伤害不计；\n"
                    "打偏算打在实际挨刀那只上。\n"
                    "场上活怪少于 3 只时停刀——BOSS 图通常只有 1 只，避免误砍 BOSS。\n"
                    "关：沿用原来的「打死 / 早切 / 空刀」才换怪。\n"
                    "生效核对：combat.log 出现 hit_rotate confirm 与 acquire hit_rotate clear");
            }
            ImGui::BeginDisabled(!hitRotateOn);
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (ImGui::DragInt("##hit_rotate_n", &hitRotateN, 1,
                               (int)xcat::kCombatHitRotateNMin,
                               (int)xcat::kCombatHitRotateNMax)) {
                hitRotateN = (int)xcat::ClampCombatHitRotateN(
                    static_cast<uint32_t>(hitRotateN < 1 ? 1 : hitRotateN));
                persistCore();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "每只怪确认命中次数（%u–%u，默认 %u）。空刀不计。满次数即换攻击盒外最近目标。",
                    (unsigned)xcat::kCombatHitRotateNMin, (unsigned)xcat::kCombatHitRotateNMax,
                    (unsigned)xcat::kCombatHitRotateNDefault);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("满刀换盒外最近");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "服务器短时间连续命中同一只怪会风控。开：确认自己打中同一只怪 N 次后，\n"
                    "改打攻击盒外、离你最近的活怪（不飞全图）。空刀不计；别人的伤害不计；\n"
                    "打偏算打在实际挨刀那只上。\n"
                    "场上活怪少于 3 只时停刀——BOSS 图通常只有 1 只，避免误砍 BOSS。\n"
                    "关：沿用原来的「打死 / 早切 / 空刀」才换怪。\n"
                    "生效核对：combat.log 出现 hit_rotate confirm 与 acquire hit_rotate clear");
            }
        }

        ImGui::Spacing();
        {
            // 用户面：开关 + 一键档位；四参藏进「细调」
            bool oneshotOn = oneshotMaxHp > 0;
            if (xcat::ui::OptionCheckbox("脆皮早切", &oneshotOn)) {
                if (oneshotOn) {
                    if (oneshotMaxHp <= 0)
                        oneshotMaxHp = (int)xcat::kCombatOneshotMaxHpWhenOn;
                    if (oneshotMinBumps < (int)xcat::kCombatOneshotMinBumpsMin)
                        oneshotMinBumps = (int)xcat::kCombatOneshotMinBumpsDefault;
                    if (oneshotMinFires < (int)xcat::kCombatOneshotMinFiresMin)
                        oneshotMinFires = (int)xcat::kCombatOneshotMinFiresDefault;
                } else {
                    oneshotMaxHp = 0;
                }
                persistCore();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("开加速时：小怪打中就换，不等尸体消失");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "适合练级图小怪（表血大约几十～一百出头）。\n"
                    "开着「攻击无CD」时更有用：命中后提前切下一只，少站着干等。\n"
                    "关掉则等怪真正死掉再换（更稳、稍慢）。");
            }

            ImGui::BeginDisabled(!oneshotOn);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("换怪节奏");
            ImGui::SameLine(0.f, ui::Gap());
            auto applyOneshotPreset = [&](int maxHp, int fires, int bumps, int lagMs,
                                          int foxGapMs) {
                oneshotMaxHp = (int)xcat::ClampCombatOneshotMaxHp((uint32_t)maxHp);
                oneshotMinFires =
                    (int)xcat::ClampCombatOneshotMinFires((uint32_t)(fires < 0 ? 0 : fires));
                oneshotMinBumps =
                    (int)xcat::ClampCombatOneshotMinBumps((uint32_t)(bumps < 0 ? 0 : bumps));
                oneshotMinLagMs =
                    (int)xcat::ClampCombatOneshotMinLagMs((uint32_t)(lagMs < 0 ? 0 : lagMs));
                oneshotFoxFillGapMs = (int)xcat::ClampCombatOneshotFoxFillGapMs(
                    static_cast<uint32_t>(foxGapMs < 0 ? 0 : foxGapMs));
                persistCore();
            };
            if (ImGui::SmallButton("稳妥"))
                applyOneshotPreset(80, 4, 2, 80, (int)xcat::kCombatOneshotFoxFillGapDefaultMs);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("少漏杀：只早切很脆的怪，多打两下再换。");
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.5f);
            if (ImGui::SmallButton("推荐"))
                applyOneshotPreset((int)xcat::kCombatOneshotMaxHpWhenOn,
                                   (int)xcat::kCombatOneshotMinFiresDefault,
                                   (int)xcat::kCombatOneshotMinBumpsDefault,
                                   (int)xcat::kCombatOneshotMinLagMsDefault,
                                   (int)xcat::kCombatOneshotFoxFillGapDefaultMs);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("默认平衡：命中确认后再换。");
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.5f);
            if (ImGui::SmallButton("更快"))
                applyOneshotPreset(200, 2, 1, 20, (int)xcat::kCombatOneshotFoxFillGapDefaultMs);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("更激进：仍等至少一次命中回写。");
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.5f);
            if (ImGui::SmallButton("射后不管"))
                applyOneshotPreset(200, 1, 0, 0, (int)xcat::kCombatOneshotFoxFillGapDefaultMs);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "出刀发出即换下一只；默认再等 %ums 才允许下一次贴怪瞬移\n"
                    "（防一刀一传送把频率打爆 / GC）。间隔可在细调里改，0=关闸高风险。",
                    (unsigned)xcat::kCombatOneshotFoxFillGapDefaultMs);
            }

            if (ImGui::TreeNode("细调（一般点上面三档即可）")) {
                ImGui::TextWrapped(
                    "只影响「看起来很小、打一下就死」的怪。"
                    "数值越大越保守（更晚换怪）；越小越快（可能漏杀）。");

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("只早切表血≤");
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::SetNextItemWidth(AppDpi_Px(72.f));
                if (ImGui::DragInt("##oneshot_maxhp", &oneshotMaxHp, 1, 1,
                                   (int)xcat::kCombatOneshotMaxHpMax)) {
                    oneshotMaxHp = (int)xcat::ClampCombatOneshotMaxHp(
                        static_cast<uint32_t>(oneshotMaxHp < 1 ? 1 : oneshotMaxHp));
                    persistCore();
                }
                ImGui::SameLine(0.f, ui::Gap() * 0.35f);
                ImGui::TextDisabled("的怪");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "对照怪物表最大血量，不是血条百分比。\n"
                        "练级小怪常见几十～一百；超过此值的怪不会早切。\n"
                        "默认启用时 %u（盖住常见 80 血档）。",
                        (unsigned)xcat::kCombatOneshotMaxHpWhenOn);
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("至少打");
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::SetNextItemWidth(AppDpi_Px(48.f));
                if (ImGui::DragInt("##oneshot_fires", &oneshotMinFires, 1,
                                   (int)xcat::kCombatOneshotMinFiresMin,
                                   (int)xcat::kCombatOneshotMinFiresMax)) {
                    oneshotMinFires = (int)xcat::ClampCombatOneshotMinFires(
                        static_cast<uint32_t>(oneshotMinFires < 0 ? 0 : oneshotMinFires));
                    persistCore();
                }
                ImGui::SameLine(0.f, ui::Gap() * 0.35f);
                ImGui::TextDisabled("下再换");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "对同一只怪至少挥几刀才允许提前换目标。\n"
                        "默认 %u；改成 2 更快，偶发空挥时更容易切太早。",
                        (unsigned)xcat::kCombatOneshotMinFiresDefault);
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("至少命中");
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::SetNextItemWidth(AppDpi_Px(48.f));
                if (ImGui::DragInt("##oneshot_bumps", &oneshotMinBumps, 1,
                                   (int)xcat::kCombatOneshotMinBumpsMin,
                                   (int)xcat::kCombatOneshotMinBumpsMax)) {
                    oneshotMinBumps = (int)xcat::ClampCombatOneshotMinBumps(
                        static_cast<uint32_t>(oneshotMinBumps < 0 ? 0 : oneshotMinBumps));
                    persistCore();
                }
                ImGui::SameLine(0.f, ui::Gap() * 0.35f);
                ImGui::TextDisabled("次再换");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "要看到「打中了」至少几次才换。\n"
                        "0 = 射后不管（出刀发出就走，不等命中）。\n"
                        "默认 %u。",
                        (unsigned)xcat::kCombatOneshotMinBumpsDefault);
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("命中后再等");
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::SetNextItemWidth(AppDpi_Px(56.f));
                if (ImGui::DragInt("##oneshot_lag", &oneshotMinLagMs, 1,
                                   (int)xcat::kCombatOneshotMinLagMsMin,
                                   (int)xcat::kCombatOneshotMinLagMsMax)) {
                    oneshotMinLagMs = (int)xcat::ClampCombatOneshotMinLagMs(
                        static_cast<uint32_t>(oneshotMinLagMs < 0 ? 0 : oneshotMinLagMs));
                    persistCore();
                }
                ImGui::SameLine(0.f, ui::Gap() * 0.35f);
                ImGui::TextDisabled("毫秒");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "第一次打中后，再稍等这么久才切下一只（给伤害结算留空）。\n"
                        "默认 %u ms；射后不管档此值通常为 0。",
                        (unsigned)xcat::kCombatOneshotMinLagMsDefault);
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("早切禁瞬移");
                ImGui::SameLine(0.f, ui::Gap());
                ImGui::SetNextItemWidth(AppDpi_Px(56.f));
                if (ImGui::DragInt("##oneshot_fox_gap", &oneshotFoxFillGapMs, 1,
                                   (int)xcat::kCombatOneshotFoxFillGapMinMs,
                                   (int)xcat::kCombatOneshotFoxFillGapMaxMs)) {
                    oneshotFoxFillGapMs = (int)xcat::ClampCombatOneshotFoxFillGapMs(
                        static_cast<uint32_t>(oneshotFoxFillGapMs < 0 ? 0 : oneshotFoxFillGapMs));
                    persistCore();
                }
                ImGui::SameLine(0.f, ui::Gap() * 0.35f);
                ImGui::TextDisabled("毫秒");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "任何提前切怪（秒杀/预测够死/累计伤害/射后不管）后，这么久内禁止贴怪瞬移。\n"
                        "默认 %u（已改为不限制）；>0 可防「一刀一 fill」灌爆主线程触发 GC 弹窗。\n"
                        "体感换怪慢时先看这一项是否仍是旧存档的 280。",
                        (unsigned)xcat::kCombatOneshotFoxFillGapDefaultMs);
                }
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
        }

        ImGui::TextDisabled("粘怪 / 低血撤 / 限定区间：Classic 锚点确认后再接");
        ImGui::TextDisabled("装备/卷軸优先拾取：见上方「拾物 → 高价值优先吸」");
    }

}


void DrawAutoSellTab(LaunchUiState& ui) {
    // 原首页「卖背包 / 自动补给」整卡迁入；拆成一键卖装 + 自动卖装两张卡。
    static bool autoSell = false;
    static bool tripOnPotionEmpty = false;
    static int tripOnPotionBelow = 1;
    static bool tripOnCustomLow = false;
    static int tripOnCustomBelow = 0;
    static bool tripOnCustom2Low = false;
    static int tripOnCustom2Below = 0;
    static bool tripOnFeedLow = false;
    static int tripOnFeedBelow = 0;
    static int sellEquipTrigger = 0;
    static bool refillHp = false;
    static bool refillMp = false;
    static bool refillCustom = false;
    static bool refillCustom2 = false;
    static bool refillFeed = false;
    static char refillHpName[64]{};
    static char refillMpName[64]{};
    static char refillCustomName[64]{};
    static char refillCustom2Name[64]{};
    static char refillFeedName[64]{};
    static char refillHpCode[24]{};
    static char refillMpCode[24]{};
    static char refillCustomCode[24]{};
    static char refillCustom2Code[24]{};
    static char refillFeedCode[24]{};
    static int refillHpBuyTo = 0;
    static int refillMpBuyTo = (int)xcat::kAutoSupplyDefaultRefillMpBuyTo;
    static int refillCustomBuyTo = (int)xcat::kAutoSupplyDefaultRefillCustomBuyTo;
    static int refillCustom2BuyTo = 0;
    static int refillFeedBuyTo = (int)xcat::kAutoSupplyDefaultRefillFeedBuyTo;
    static bool rechargeStars = false;
    static uint64_t asupTick = 0;
    static bool asupLoaded = false;
    static std::string asupLoadedBin;
    if (!ui.prefsBinDir.empty() && asupLoadedBin != ui.prefsBinDir) {
        asupLoaded = false;
        asupLoadedBin = ui.prefsBinDir;
    }
    if (!ui.prefsBinDir.empty()) {
        if (!asupLoaded) {
            xcat::AutoSupplyConfig as{};
            if (!xcat::ReadAutoSupply(ui.prefsBinDir.c_str(), as))
                xcat::AutoSupplySetDefaults(as);
            autoSell = (as.enabled != 0) || (as.autoSellOnBagFullEnabled != 0);
            tripOnPotionEmpty = as.tripOnPotionEmpty != 0;
            tripOnPotionBelow = as.tripOnPotionBelow > 0 ? as.tripOnPotionBelow : 1;
            tripOnCustomLow = as.tripOnCustomLow != 0;
            tripOnCustomBelow = as.tripOnCustomBelow;
            tripOnCustom2Low = as.tripOnCustom2Low != 0;
            tripOnCustom2Below = as.tripOnCustom2Below;
            tripOnFeedLow = as.tripOnFeedLow != 0;
            tripOnFeedBelow = as.tripOnFeedBelow;
            sellEquipTrigger = as.sellFreeSlotsAtOrBelow;
            refillHp = as.refillHpEnabled != 0;
            refillMp = as.refillMpEnabled != 0;
            refillCustom = as.refillCustomEnabled != 0;
            refillCustom2 = as.refillCustom2Enabled != 0;
            refillFeed = as.refillFeedEnabled != 0;
            strncpy_s(refillHpName, as.refillHpName, _TRUNCATE);
            strncpy_s(refillMpName, as.refillMpName, _TRUNCATE);
            strncpy_s(refillCustomName, as.refillCustomName, _TRUNCATE);
            strncpy_s(refillCustom2Name, as.refillCustom2Name, _TRUNCATE);
            strncpy_s(refillFeedName, as.refillFeedName, _TRUNCATE);
            strncpy_s(refillHpCode, as.refillHpCode, _TRUNCATE);
            strncpy_s(refillMpCode, as.refillMpCode, _TRUNCATE);
            strncpy_s(refillCustomCode, as.refillCustomCode, _TRUNCATE);
            strncpy_s(refillCustom2Code, as.refillCustom2Code, _TRUNCATE);
            strncpy_s(refillFeedCode, as.refillFeedCode, _TRUNCATE);
            refillHpBuyTo = as.refillHpBuyTo;
            refillMpBuyTo = as.refillMpBuyTo;
            refillCustomBuyTo = as.refillCustomBuyTo;
            refillCustom2BuyTo = as.refillCustom2BuyTo;
            refillFeedBuyTo = as.refillFeedBuyTo;
            rechargeStars = as.rechargeStarsEnabled != 0;
            asupTick = as.writeTickMs;
            asupLoaded = true;
        }
    }
    auto persistAsup = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::AutoSupplyConfig as{};
        (void)xcat::ReadAutoSupply(ui.prefsBinDir.c_str(), as);
        as.enabled = autoSell ? 1u : 0u;
        as.autoSellOnBagFullEnabled = autoSell ? 1u : 0u;
        as.tripOnPotionEmpty = tripOnPotionEmpty ? 1u : 0u;
        as.tripOnPotionBelow = tripOnPotionBelow < 1 ? 1 : tripOnPotionBelow;
        as.tripOnCustomLow = tripOnCustomLow ? 1u : 0u;
        as.tripOnCustomBelow = tripOnCustomBelow < 0 ? 0 : tripOnCustomBelow;
        as.tripOnCustom2Low = tripOnCustom2Low ? 1u : 0u;
        as.tripOnCustom2Below = tripOnCustom2Below < 0 ? 0 : tripOnCustom2Below;
        as.tripOnFeedLow = tripOnFeedLow ? 1u : 0u;
        as.tripOnFeedBelow = tripOnFeedBelow < 0 ? 0 : tripOnFeedBelow;
        as.shopMapName[0] = '\0';    // 经典版固定自动寻店，不暴露手填店图
        as.returnMapName[0] = '\0';  // 挂机图由打怪/补给行程自动记录，不手填
        as.sellFreeSlotsAtOrBelow = sellEquipTrigger < 0 ? 0 : sellEquipTrigger;
        as.refillHpEnabled = refillHp ? 1u : 0u;
        as.refillMpEnabled = refillMp ? 1u : 0u;
        as.refillCustomEnabled = refillCustom ? 1u : 0u;
        as.refillCustom2Enabled = refillCustom2 ? 1u : 0u;
        as.refillFeedEnabled = refillFeed ? 1u : 0u;
        strncpy_s(as.refillHpName, refillHpName, _TRUNCATE);
        strncpy_s(as.refillMpName, refillMpName, _TRUNCATE);
        strncpy_s(as.refillCustomName, refillCustomName, _TRUNCATE);
        strncpy_s(as.refillCustom2Name, refillCustom2Name, _TRUNCATE);
        strncpy_s(as.refillFeedName, refillFeedName, _TRUNCATE);
        strncpy_s(as.refillHpCode, refillHpCode, _TRUNCATE);
        strncpy_s(as.refillMpCode, refillMpCode, _TRUNCATE);
        strncpy_s(as.refillCustomCode, refillCustomCode, _TRUNCATE);
        strncpy_s(as.refillCustom2Code, refillCustom2Code, _TRUNCATE);
        strncpy_s(as.refillFeedCode, refillFeedCode, _TRUNCATE);
        as.refillHpBuyTo = refillHpBuyTo;
        as.refillMpBuyTo = refillMpBuyTo;
        as.refillCustomBuyTo = refillCustomBuyTo;
        as.refillCustom2BuyTo = refillCustom2BuyTo;
        as.refillFeedBuyTo = refillFeedBuyTo;
        as.rechargeStarsEnabled = rechargeStars ? 1u : 0u;
        as.writeTickMs = GetTickCount64();
        if (xcat::WriteAutoSupply(ui.prefsBinDir.c_str(), as)) asupTick = as.writeTickMs;
    };

    auto writeAsupManual = [&](uint32_t kind) -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::AutoSupplyConfig as{};
        (void)xcat::ReadAutoSupply(ui.prefsBinDir.c_str(), as);
        as.manualSeq = as.manualSeq == 0 ? 1u : as.manualSeq + 1u;
        as.manualKind = kind;
        as.shopMapName[0] = '\0';
        as.returnMapName[0] = '\0';
        if (kind == xcat::kAutoSupplyManualStop) {
            as.enabled = 0;
            as.autoSellOnBagFullEnabled = 0;
            as.tripOnPotionEmpty = 0;
            as.tripOnCustomLow = 0;
            as.tripOnCustom2Low = 0;
            as.tripOnFeedLow = 0;
        }
        as.writeTickMs = GetTickCount64();
        if (!xcat::WriteAutoSupply(ui.prefsBinDir.c_str(), as)) return false;
        asupTick = as.writeTickMs;
        return true;
    };

    {
        static char keepRulesBuf[1024]{};
        static uint64_t keepRulesTick = 0;
        static bool keepRulesLoaded = false;
        static std::string keepRulesBin;
        static const char* keepRulesSaveHint = "";
        static const char* manualCmdHint = "";

        auto sellbagKeepText = [](const xcat::SellbagConfig& cfg) -> std::string {
            std::string out;
            const uint32_t count =
                (std::min)(cfg.keepRuleCount, static_cast<uint32_t>(xcat::kSellbagMaxKeepRules));
            for (uint32_t i = 0; i < count; ++i) {
                const auto& r = cfg.keepRules[i];
                if (!r.enabled || !r.nameKey[0]) continue;
                if (!out.empty()) out.push_back(' ');
                out += r.nameKey;
            }
            return out;
        };
        auto applyKeepText = [](xcat::SellbagConfig& cfg, const char* text) {
            for (auto& r : cfg.keepRules) r = {};
            cfg.keepRuleCount = 0;
            if (!text || !text[0]) return;
            char buf[1024]{};
            strncpy_s(buf, text, _TRUNCATE);
            char* ctx = nullptr;
            for (char* tok = strtok_s(buf, ",;| \t", &ctx); tok;
                 tok = strtok_s(nullptr, ",;| \t", &ctx)) {
                while (*tok == ' ' || *tok == '\t') ++tok;
                if (!*tok) continue;
                if (cfg.keepRuleCount >= static_cast<uint32_t>(xcat::kSellbagMaxKeepRules)) break;
                auto& r = cfg.keepRules[cfg.keepRuleCount++];
                r.enabled = 1;
                r.targetMask = xcat::kSellbagBagAll;
                strncpy_s(r.nameKey, tok, _TRUNCATE);
            }
        };
        auto loadSellbag = [&](xcat::SellbagConfig& out) {
            xcat::SellbagSetDefaults(out);
            if (ui.prefsBinDir.empty()) return;
            (void)xcat::ReadSellbag(ui.prefsBinDir.c_str(), out);
        };
        auto saveSellbag = [&](xcat::SellbagConfig& cfg) -> bool {
            if (ui.prefsBinDir.empty()) return false;
            cfg.magic = xcat::kSellbagMagic;
            cfg.version = xcat::kSellbagVersion;
            if (cfg.keepRuleCount > static_cast<uint32_t>(xcat::kSellbagMaxKeepRules))
                cfg.keepRuleCount = static_cast<uint32_t>(xcat::kSellbagMaxKeepRules);
            cfg.writeTickMs = GetTickCount64();
            return xcat::WriteSellbag(ui.prefsBinDir.c_str(), cfg);
        };
        auto triggerManual = [&](uint32_t mask) -> bool {
            if (ui.prefsBinDir.empty()) return false;
            xcat::SellbagConfig cfg{};
            loadSellbag(cfg);
            applyKeepText(cfg, keepRulesBuf);
            cfg.manualSeq = cfg.manualSeq == 0 ? 1u : cfg.manualSeq + 1u;
            cfg.manualMask = mask & xcat::kSellbagBagAll;
            return saveSellbag(cfg);
        };
        auto triggerAbort = [&]() -> bool {
            if (ui.prefsBinDir.empty()) return false;
            xcat::SellbagConfig cfg{};
            loadSellbag(cfg);
            cfg.abortSeq = cfg.abortSeq == 0 ? 1u : cfg.abortSeq + 1u;
            if (!saveSellbag(cfg)) return false;
            return writeAsupManual(xcat::kAutoSupplyManualStop);
        };
        auto triggerReturnFarm = [&]() -> bool {
            return writeAsupManual(xcat::kAutoSupplyManualReturnFarm);
        };

        if (!ui.prefsBinDir.empty()) {
            if (!keepRulesLoaded || keepRulesBin != ui.prefsBinDir) {
                xcat::SellbagConfig cfg{};
                loadSellbag(cfg);
                const std::string text = sellbagKeepText(cfg);
                strncpy_s(keepRulesBuf, text.c_str(), _TRUNCATE);
                keepRulesTick = cfg.writeTickMs;
                keepRulesLoaded = true;
                keepRulesBin = ui.prefsBinDir;
                keepRulesSaveHint = "";
            } else {
                xcat::SellbagConfig disk{};
                if (xcat::ReadSellbag(ui.prefsBinDir.c_str(), disk) &&
                    disk.writeTickMs != keepRulesTick && !ImGui::IsAnyItemActive()) {
                    const std::string text = sellbagKeepText(disk);
                    strncpy_s(keepRulesBuf, text.c_str(), _TRUNCATE);
                    keepRulesTick = disk.writeTickMs;
                }
            }
        }


        {
            xcat::ui::CardGuard card("##tab_auto_sell_manual", "一键卖装");
            ImGui::TextWrapped(
                "填写「不卖名单」后点下方按钮；名单与「自动卖装」共用。需先打开 NPC 商店。");

            // —— 不卖名单（对照仓 DrawSellbagKeepRulesEditor）——
        ImGui::TextUnformatted("不卖名单");
        ImGui::SameLine();
        ImGui::TextDisabled("（装备栏+其他栏共用；手动卖 / 自动卖共用）");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##sellbag_keep_rules_quick",
                                 "不卖关键词，逗号或空格分隔（默认：礦 玻璃鞋）", keepRulesBuf,
                                 sizeof(keepRulesBuf));
        const bool keepRulesEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (!ui.prefsBinDir.empty()) {
                xcat::SellbagConfig cfg{};
                loadSellbag(cfg);
                applyKeepText(cfg, keepRulesBuf);
                if (!saveSellbag(cfg)) {
                    keepRulesSaveHint = "保存不卖名单失败";
                } else {
                    keepRulesTick = cfg.writeTickMs;
                    keepRulesSaveHint = "已保存不卖名单";
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "物品名包含任一关键词 → 跳过不卖\n"
                "例：披風 卷軸 藥水\n"
                "手动一键卖与自动回城卖共用这份名单");
        }
        ImGui::TextDisabled("物品名含关键词 → 不卖。空 = 该卖的都卖。");
        if (keepRulesSaveHint && keepRulesSaveHint[0])
            ImGui::TextDisabled("%s", keepRulesSaveHint);

        // 关键词离线表匹配（对照仓补药「已匹配」语义；失焦刷新；同行紧凑展示）
        if (keepRulesBuf[0] && !ui.prefsBinDir.empty()) {
            static std::string s_keepMatchQuery;
            static std::string s_keepMatchBin;
            static bool s_keepCatalogOk = false;
            static std::vector<SellbagKeepHitPreview> s_keepHits;

            if (!keepRulesEditing &&
                (s_keepMatchQuery != keepRulesBuf || s_keepMatchBin != ui.prefsBinDir)) {
                s_keepMatchQuery = keepRulesBuf;
                s_keepMatchBin = ui.prefsBinDir;
                s_keepHits.clear();
                const auto& pack = xcat::GetSharedItemCatalog(ui.prefsBinDir.c_str());
                s_keepCatalogOk = pack.loaded;
                if (pack.loaded) {
                    char buf[1024]{};
                    strncpy_s(buf, keepRulesBuf, _TRUNCATE);
                    char* ctx = nullptr;
                    for (char* tok = strtok_s(buf, ",;| \t", &ctx); tok;
                         tok = strtok_s(nullptr, ",;| \t", &ctx)) {
                        while (*tok == ' ' || *tok == '\t') ++tok;
                        if (!*tok) continue;
                        SellbagKeepHitPreview h{};
                        h.key = tok;
                        std::vector<std::string> codes;
                        h.hit = xcat::ItemCatalogCollectCodesByNameContains(pack, tok, codes, 1);
                        if (!codes.empty()) {
                            h.sampleCode = codes[0];
                            if (const char* nm =
                                    xcat::ItemCatalogLookupName(pack, codes[0].c_str())) {
                                h.sampleName = nm;
                            }
                        }
                        s_keepHits.push_back(std::move(h));
                    }
                }
            }

            if (keepRulesEditing && s_keepMatchQuery != keepRulesBuf) {
                ImGui::TextDisabled("失焦后刷新匹配…");
            } else if (!s_keepCatalogOk && s_keepMatchQuery == keepRulesBuf) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "离线物品表未加载");
            } else if (s_keepMatchQuery == keepRulesBuf && !s_keepHits.empty()) {
                const float gap = ui::Gap() * 0.45f;
                for (size_t i = 0; i < s_keepHits.size(); ++i) {
                    const SellbagKeepHitPreview& h = s_keepHits[i];
                    if (i > 0) {
                        ImGui::SameLine(0.f, gap);
                        ImGui::TextDisabled("·");
                        ImGui::SameLine(0.f, gap);
                    }
                    ImGui::TextUnformatted(h.key.c_str());
                    ImGui::SameLine(0.f, gap);
                    if (h.hit > 0) {
                        if (h.hit > 1)
                            ImGui::TextDisabled("已匹配×%zu", h.hit);
                        else
                            ImGui::TextDisabled("已匹配");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            if (!h.sampleName.empty())
                                ImGui::SetTooltip("例：%s（CODE %s）", h.sampleName.c_str(),
                                                  h.sampleCode.c_str());
                            else
                                ImGui::SetTooltip("CODE %s", h.sampleCode.c_str());
                        }
                    } else {
                        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "未匹配");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "离线表无此关键词；卖出时仍可按背包物品名兜底匹配");
                        }
                    }
                }
            }
        }

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.25f));

        // —— 手动卖出（对照仓 DrawNativeSellbagActions）——
        ImGui::TextUnformatted("手动卖出 / 充飞镖");
        ImGui::TextDisabled("需先打开 NPC 商店，再点下方按钮。");

        const bool busy = [&]() {
            if (ui.prefsBinDir.empty()) return false;
            xcat::PayloadStatus st{};
            if (xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) && st.sellbagBusy != 0)
                return true;
            xcat::AutoSupplyStatus as{};
            return xcat::ReadAutoSupplyStatus(ui.prefsBinDir.c_str(), as) &&
                   xcat::AutoSupplyStateIsBusy(as.state);
        }();

        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float btnW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("一键卖装", ImVec2(btnW, 0.f))) {
            if (!triggerManual(xcat::kSellbagBagEquip)) {
                manualCmdHint = "发送失败：payload 忙、未注入或命令写入失败";
                notify::PushLocal(/*Warning*/ 2, "sellbag-cmd", "一键卖装失败", "写入命令失败",
                                  3500);
            } else {
                manualCmdHint = "已发送一键卖装命令";
            }
        }
        ImGui::SameLine(0.f, gap);
        if (ImGui::Button("一键卖其他", ImVec2(btnW, 0.f))) {
            if (!triggerManual(xcat::kSellbagBagEtc)) {
                manualCmdHint = "发送失败：payload 忙、未注入或命令写入失败";
                notify::PushLocal(/*Warning*/ 2, "sellbag-cmd", "卖其他失败", "写入命令失败", 3500);
            } else {
                manualCmdHint = "已发送一键卖其他命令";
            }
        }
        if (ImGui::Button("一键卖装备和其他", ImVec2(-1.f, 0.f))) {
            if (!triggerManual(xcat::kSellbagBagAll)) {
                manualCmdHint = "发送失败：payload 忙、未注入或命令写入失败";
                notify::PushLocal(/*Warning*/ 2, "sellbag-cmd", "全卖失败", "写入命令失败", 3500);
            } else {
                manualCmdHint = "已发送一键卖装备和其他命令";
            }
        }
        if (ImGui::Button("一键充值飞镖", ImVec2(-1.f, 0.f))) {
            if (!writeAsupManual(xcat::kAutoSupplyManualRechargeStars)) {
                manualCmdHint = "发送失败：payload 忙、未注入或命令写入失败";
                notify::PushLocal(/*Warning*/ 2, "auto-supply-charge", "一键充飞镖失败",
                                  ui.prefsBinDir.empty() ? "无数据目录" : "写入命令失败", 3500);
            } else {
                manualCmdHint = "已发送一键充值飞镖命令";
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "需已打开 NPC 商店\n"
                "扫描消耗栏手里剑并 Charge（每次 1 格，循环至满或金币不够）\n"
                "不卖装、不补药、不关店、不回城");
        }
        if (busy) ImGui::EndDisabled();
        if (manualCmdHint && manualCmdHint[0]) ImGui::TextDisabled("%s", manualCmdHint);

        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadStatus st{};
            if (xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st)) {
                ImGui::Text("手动卖：%s",
                            st.sellbagBusy ? "正在卖出" : SellbagStateLabel(st.sellbagState));
                if (st.sellbagMessage[0]) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", st.sellbagMessage);
                }
                if (st.sellbagBusy || st.sellbagState != 0u) {
                    ImGui::TextDisabled("上轮：卖装 %u，卖其他 %u，不卖保留 %u，失败 %u",
                                        st.sellbagEquipSold, st.sellbagEtcSold, st.sellbagKept,
                                        st.sellbagFailed);
                }
            } else {
                ImGui::TextDisabled("手动卖：payload 未在线，按钮会尝试写入兼容命令");
            }
        }
        }
        CardGap();
        {
            // —— 自动回城卖 / 补给 ——
            xcat::ui::CardGuard card("##tab_auto_sell_auto", "自动卖装");
            DrawAutoSupplyStatusLine(ui.prefsBinDir);

        {
            // 对照仓：只读展示 lastFarmMapName；开启自动打怪 / 跑补给时自动记野图。
            const char* farmKey = nullptr;
            xcat::AutoSupplyStatus st{};
            if (!ui.prefsBinDir.empty() &&
                xcat::ReadAutoSupplyStatus(ui.prefsBinDir.c_str(), st) && st.lastFarmMapName[0]) {
                farmKey = st.lastFarmMapName;
            }
            if (!farmKey) {
                ImGui::TextUnformatted("挂机图：未记录");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "开启「自动打怪」时自动记录当前野图（城镇不记）\n"
                        "或跑一趟回城卖/补给时记下出发图\n"
                        "无需手填");
                }
            } else {
                static std::string s_farmDisp;
                const char* farmDisp =
                    LookupFarmMapDisp(ui.prefsBinDir.c_str(), farmKey, s_farmDisp);
                ImGui::Text("挂机图：%s", farmDisp);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "图号：%s\n"
                        "开启自动打怪时自动更新；城镇不覆盖",
                        farmKey);
                }
            }
        }

        if (ImGui::Checkbox("自动卖##home_asup", &autoSell)) persistAsup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "按装备栏件数自动回城卖装（其他栏不计数）\n"
                "自动寻最近可卖店；进城先卖装再卖其他，并补 1 张回城卷\n"
                "可选充飞镖 / 补红蓝自定义饲料：店有则买\n"
                "出过刀必须先 hangup 清 FLAG 再卖；没出过刀满包可直接出门。\n"
                "重连途中不卖；倒计时将尽也推迟到下一轮落地。\n"
                "落地之后到下一轮出刀前，冷却一过就可以卖。\n"
                "不卖名单与上方手动卖共用");
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.55f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("装备≥");
        ImGui::SameLine(0.f, ui::Gap() * 0.25f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.2f);
        if (NativeInputIntClamped("##home_asup_equip_x", sellEquipTrigger, 0, 300)) persistAsup();
        ImGui::SameLine(0.f, ui::Gap() * 0.3f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("件就卖（0=装栏满）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "只看装备栏已用件数\n"
                "0 = 满装才卖；如 20 = 装备满 20 件就回城");
        }

        {
            const float fs = ImGui::GetFontSize();
            const float rowGap = ui::Gap();
            if (ImGui::Checkbox("缺药回城##home_asup_pot", &tripOnPotionEmpty)) {
                if (tripOnPotionEmpty && tripOnPotionBelow < 1) tripOnPotionBelow = 1;
                persistAsup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "看 PageDown(红)/PageUp(蓝) 绑定药数量\n"
                    "少于右侧数字就回城补给\n"
                    "填 1 ≈ 空了才走；可调大做缓冲\n"
                    "仅对下方已勾选的「红药/蓝药」生效；未勾选的一侧不买、也不因它开趟\n"
                    "会自动对齐下方药名/CODE（不自动勾选）");
            }
            ImGui::SameLine(0.f, rowGap * 0.55f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("绑定药少于");
            ImGui::SameLine(0.f, rowGap * 0.25f);
            ImGui::SetNextItemWidth(fs * 3.0f);
            if (NativeInputIntClamped("##home_asup_pot_below", tripOnPotionBelow, 1, 9999))
                persistAsup();
        }

        // 注入端 status 上报的 PageDown/PageUp 绑定 ID → 自动对齐补红/蓝（静默为主）
        if (!ui.prefsBinDir.empty() && !ImGui::IsAnyItemActive()) {
            xcat::AutoSupplyStatus bst{};
            if (xcat::ReadAutoSupplyStatus(ui.prefsBinDir.c_str(), bst)) {
                auto applyBound = [&](int32_t boundId, bool* en, char* nameBuf, size_t nameSz,
                                     char* codeBuf, size_t codeSz) -> bool {
                    if (boundId <= 0 || !en || !nameBuf || !codeBuf || nameSz < 2 || codeSz < 2)
                        return false;
                    char code[24]{};
                    snprintf(code, sizeof(code), "%d", (int)boundId);
                    if (codeBuf[0] && strcmp(codeBuf, code) == 0) return false;
                    strncpy_s(codeBuf, codeSz, code, _TRUNCATE);
                    const auto& pack = xcat::GetSharedItemCatalog(ui.prefsBinDir.c_str());
                    if (const char* nm = xcat::ItemCatalogLookupName(pack, code); nm && nm[0]) {
                        strncpy_s(nameBuf, nameSz, nm, _TRUNCATE);
                    } else {
                        strncpy_s(nameBuf, nameSz, code, _TRUNCATE);
                    }
                    // 只对齐名/CODE，不自动勾选「补红/补蓝」
                    return true;
                };
                bool synced = false;
                synced |= applyBound(bst.boundHpItemId, &refillHp, refillHpName, sizeof(refillHpName),
                                   refillHpCode, sizeof(refillHpCode));
                synced |= applyBound(bst.boundMpItemId, &refillMp, refillMpName, sizeof(refillMpName),
                                   refillMpCode, sizeof(refillMpCode));
                if (synced) persistAsup();
                // 绑定 ID 变化才气泡一次。同 key 的 PushLocal 会刷新 TTL，每帧对齐会「永远弹着」。
                static int32_t s_boundNotifyHp = -1;
                static int32_t s_boundNotifyMp = -1;
                const int32_t hpId = bst.boundHpItemId > 0 ? bst.boundHpItemId : 0;
                const int32_t mpId = bst.boundMpItemId > 0 ? bst.boundMpItemId : 0;
                if (hpId != s_boundNotifyHp || mpId != s_boundNotifyMp) {
                    const bool firstLatch = (s_boundNotifyHp < 0 && s_boundNotifyMp < 0);
                    s_boundNotifyHp = hpId;
                    s_boundNotifyMp = mpId;
                    if (synced && !firstLatch && (hpId > 0 || mpId > 0)) {
                        notify::PushLocal(/*Info*/ 0, "auto-supply-bound-sync", "补货已对齐绑定",
                                          "补红/蓝已按 PageDown/PageUp 绑定道具更新", 2800);
                    }
                }
            }
        }

        auto resolveRefillCode = [&](const char* nameZh, char* codeOut, size_t codeCap) -> bool {
            if (!codeOut || codeCap == 0) return false;
            codeOut[0] = 0;
            if (!nameZh || !nameZh[0]) return false;
            if (const char* builtin = xcat::AutoSupplyBuiltinRefillCodeForName(nameZh)) {
                strncpy_s(codeOut, codeCap, builtin, _TRUNCATE);
                return true;
            }
            if (ui.prefsBinDir.empty()) return false;
            const auto& pack = xcat::GetSharedItemCatalog(ui.prefsBinDir.c_str());
            const char* code = xcat::ItemCatalogLookupCodeByExactName(pack, nameZh);
            if (!code || !code[0]) return false;
            strncpy_s(codeOut, codeCap, code, _TRUNCATE);
            return true;
        };

        auto defaultTripBelow = [](int buyTo) -> int {
            if (buyTo <= 1) return 0;
            const int v = buyTo / 5;
            return v > 0 ? v : 1;
        };
        auto clampTripBelowUi = [](int& below, int buyTo) {
            if (buyTo <= 0) {
                below = 0;
                return;
            }
            if (below >= buyTo) below = buyTo - 1;
            if (below < 0) below = 0;
        };

        // 人话列：勾选名 | 补到 N | [低于 N 回城] | 物品名 | 匹配态
        auto drawSupplyRow = [&](const char* shortLabel, const char* idSuffix, bool* en, char* nameBuf,
                                 size_t nameSz, char* codeBuf, size_t codeSz, int* buyTo,
                                 bool* tripOn, int* tripBelow, bool nameEditable, const char* tip) {
            const float fs = ImGui::GetFontSize();
            const float gap = ui::Gap();
            const float rowY = ImGui::GetCursorPosY();
            const float rowX0 = ImGui::GetCursorPosX();
            const float contentRight = rowX0 + ImGui::GetContentRegionAvail().x;
            const float qtyW = fs * 3.0f;
            const float frameH = ImGui::GetFrameHeight();
            const float colEnW = fs * 5.6f;
            const float colBuyW =
                ImGui::CalcTextSize("补到").x + gap * 0.35f + qtyW + gap * 0.55f;
            const float colTripW =
                ImGui::GetFrameHeight() + ImGui::CalcTextSize("低于").x + gap * 0.35f + qtyW +
                ImGui::CalcTextSize("回城").x + gap * 0.55f;
            const float colStW = ImGui::CalcTextSize("未匹配").x + gap * 0.4f;
            const float xBuy = rowX0 + colEnW;
            const float xTrip = xBuy + colBuyW;
            const float xName = xTrip + colTripW;
            char id[72]{};

            ImGui::SetCursorPos(ImVec2(rowX0, rowY));
            snprintf(id, sizeof(id), "%s##home_asup_%s", shortLabel, idSuffix);
            if (ImGui::Checkbox(id, en)) {
                if (en && *en && !nameEditable && nameBuf && codeBuf) {
                    strncpy_s(nameBuf, nameSz, xcat::kAutoSupplyDefaultRefillFeedName, _TRUNCATE);
                    strncpy_s(codeBuf, codeSz, xcat::kAutoSupplyDefaultRefillFeedCode, _TRUNCATE);
                }
                persistAsup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && tip) {
                ImGui::SetTooltip("%s", tip);
            }

            ImGui::SetCursorPos(ImVec2(xBuy, rowY));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("补到");
            ImGui::SameLine(0.f, gap * 0.35f);
            ImGui::SetNextItemWidth(qtyW);
            snprintf(id, sizeof(id), "##home_asup_%s_qty", idSuffix);
            if (NativeInputIntClamped(id, *buyTo, 0, 9999)) {
                if (tripOn && tripBelow && *tripOn) clampTripBelowUi(*tripBelow, *buyTo);
                persistAsup();
            }

            ImGui::SetCursorPos(ImVec2(xTrip, rowY));
            if (tripOn && tripBelow) {
                const bool codeOk = codeBuf && codeBuf[0];
                const bool buyOk = *buyTo > 0;
                const bool canTrip = codeOk && buyOk;
                if (!canTrip) ImGui::BeginDisabled(true);
                snprintf(id, sizeof(id), "##home_asup_%s_trip", idSuffix);
                if (ImGui::Checkbox(id, tripOn)) {
                    if (*tripOn) {
                        if (en) *en = true;
                        if (*tripBelow <= 0) *tripBelow = defaultTripBelow(*buyTo);
                        clampTripBelowUi(*tripBelow, *buyTo);
                    }
                    persistAsup();
                }
                if (!canTrip) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    if (!codeOk)
                        ImGui::SetTooltip("先把右侧物品名匹配成功（显示「已匹配」）");
                    else if (!buyOk)
                        ImGui::SetTooltip("先把「补到」设成大于 0");
                    else
                        ImGui::SetTooltip(
                            "勾选后：背包里这件少于右侧数字就回城补给\n"
                            "阈值必须小于「补到」；勾选会顺带打开左侧补货");
                }
                ImGui::SameLine(0.f, gap * 0.25f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("低于");
                ImGui::SameLine(0.f, gap * 0.25f);
                ImGui::SetNextItemWidth(qtyW);
                snprintf(id, sizeof(id), "##home_asup_%s_below", idSuffix);
                const int maxBelow = (*buyTo > 0) ? (*buyTo - 1) : 0;
                if (NativeInputIntClamped(id, *tripBelow, 0, maxBelow > 0 ? maxBelow : 0)) {
                    clampTripBelowUi(*tripBelow, *buyTo);
                    persistAsup();
                }
                ImGui::SameLine(0.f, gap * 0.25f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("回城");
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("（用上方缺药）");
            }

            const char* matchLabel = nullptr;
            bool matchBad = false;
            if (nameEditable) {
                if (nameBuf && nameBuf[0]) {
                    if (codeBuf && codeBuf[0]) {
                        matchLabel = "已匹配";
                    } else {
                        matchLabel = "未匹配";
                        matchBad = true;
                    }
                }
            } else {
                matchLabel = "固定";
            }

            const float nameW = (std::max)(fs * 3.5f, contentRight - xName - colStW);
            ImGui::SetCursorPos(ImVec2(xName, rowY));
            snprintf(id, sizeof(id), "##home_asup_%s_name", idSuffix);
            ImGui::SetNextItemWidth(nameW);
            if (nameEditable) {
                ImGui::InputTextWithHint(id, "精确中文物品名", nameBuf, (int)nameSz);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    if (!resolveRefillCode(nameBuf, codeBuf, codeSz)) codeBuf[0] = '\0';
                    persistAsup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    if (codeBuf && codeBuf[0])
                        ImGui::SetTooltip("已匹配 CODE %s\n名称须与离线表完全一致", codeBuf);
                    else
                        ImGui::SetTooltip("回车/失焦后按精确中文名查表；未匹配则不会买");
                }
            } else {
                char disp[96]{};
                snprintf(disp, sizeof(disp), "%s / %s", xcat::kAutoSupplyDefaultRefillFeedName,
                         xcat::kAutoSupplyDefaultRefillFeedAltName);
                ImGui::BeginDisabled(true);
                ImGui::InputText(id, disp, sizeof(disp), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("固定：美味飼料优先，其次寵物食品\nCODE %s → %s",
                                      xcat::kAutoSupplyDefaultRefillFeedCode,
                                      xcat::kAutoSupplyDefaultRefillFeedAltCode);
                }
            }

            if (matchLabel) {
                ImGui::SameLine(0.f, gap * 0.35f);
                ImGui::AlignTextToFramePadding();
                if (matchBad) {
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "%s", matchLabel);
                } else {
                    ImGui::TextDisabled("%s", matchLabel);
                }
            }

            ImGui::SetCursorPosY(rowY + frameH + ImGui::GetStyle().ItemSpacing.y);
        };

        ImGui::Spacing();
        ImGui::TextDisabled("进店补货（店里有才买）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "「补到」= 买到这个数量为止\n"
                "「低于…回城」= 背包少于该数就专程回城补（红/蓝请用上方缺药回城）\n"
                "物品名须与游戏内中文完全一致");
        }
        {
            const float fs = ImGui::GetFontSize();
            const float gap = ui::Gap();
            const float qtyW = fs * 3.0f;
            const float colEnW = fs * 5.6f;
            const float colBuyW =
                ImGui::CalcTextSize("补到").x + gap * 0.35f + qtyW + gap * 0.55f;
            const float colTripW =
                ImGui::GetFrameHeight() + ImGui::CalcTextSize("低于").x + gap * 0.35f + qtyW +
                ImGui::CalcTextSize("回城").x + gap * 0.55f;
            const float x0 = ImGui::GetCursorPosX();
            const float y = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(x0, y));
            ImGui::TextDisabled("项目");
            ImGui::SetCursorPos(ImVec2(x0 + colEnW, y));
            ImGui::TextDisabled("补到");
            ImGui::SetCursorPos(ImVec2(x0 + colEnW + colBuyW, y));
            ImGui::TextDisabled("低库存回城");
            ImGui::SetCursorPos(ImVec2(x0 + colEnW + colBuyW + colTripW, y));
            ImGui::TextDisabled("物品名");
            ImGui::SetCursorPosY(y + ImGui::GetTextLineHeight() +
                                 ImGui::GetStyle().ItemSpacing.y * 0.5f);
        }
        drawSupplyRow("红药", "hp", &refillHp, refillHpName, sizeof(refillHpName), refillHpCode,
                      sizeof(refillHpCode), &refillHpBuyTo, nullptr, nullptr, true,
                      "勾选后：开店补红药；并允许「缺药回城」监视 PageDown 绑定红");
        drawSupplyRow("蓝药", "mp", &refillMp, refillMpName, sizeof(refillMpName), refillMpCode,
                      sizeof(refillMpCode), &refillMpBuyTo, nullptr, nullptr, true,
                      "勾选后：开店补蓝药；并允许「缺药回城」监视 PageUp 绑定蓝");
        drawSupplyRow("自定义1", "custom", &refillCustom, refillCustomName, sizeof(refillCustomName),
                      refillCustomCode, sizeof(refillCustomCode), &refillCustomBuyTo,
                      &tripOnCustomLow, &tripOnCustomBelow, true,
                      "自定义消耗品（如回城卷、箭矢等）");
        drawSupplyRow("自定义2", "custom2", &refillCustom2, refillCustom2Name,
                      sizeof(refillCustom2Name), refillCustom2Code, sizeof(refillCustom2Code),
                      &refillCustom2BuyTo, &tripOnCustom2Low, &tripOnCustom2Below, true,
                      "第二个自定义消耗品槽");
        drawSupplyRow("饲料", "feed", &refillFeed, refillFeedName, sizeof(refillFeedName),
                      refillFeedCode, sizeof(refillFeedCode), &refillFeedBuyTo, &tripOnFeedLow,
                      &tripOnFeedBelow, false,
                      "美味飼料优先，其次寵物食品；回城阈值按两者合计");

        if (ImGui::Checkbox("卖装后自动充飞镖##home_asup_stars", &rechargeStars)) persistAsup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "卖装开店后扫描消耗栏手里剑并 Charge\n"
                "钱不够就跳过；需开「自动卖」或点立即一趟");
        }

        if (ImGui::Button("立即回城卖装一趟##home_asup_trip", ImVec2(-1.f, 0.f))) {
            if (!writeAsupManual(xcat::kAutoSupplyManualTrip)) {
                notify::PushLocal(/*Warning*/ 2, "auto-supply-trip", "启动失败",
                                  ui.prefsBinDir.empty() ? "无数据目录" : "写入命令失败", 3500);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "立刻跑完整行程：回城→先卖装→货架有则补回城卷→可选充飞镖→可选补红/蓝/自定义/"
                "饲料→回挂机图\n"
                "不依赖装备件数阈值；自动寻最近可卖店");
        }
        if (ImGui::Button("回挂机图##home_asup_return_farm", ImVec2(-1.f, 0.f))) {
            if (!triggerReturnFarm()) {
                notify::PushLocal(/*Warning*/ 2, "auto-sell-return", "回挂机图失败", "写入命令失败",
                                  3500);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "只赶回上方记录的挂机图（不卖装、不补药）\n"
                "可打断进行中的补给行程；需已记录挂机图\n"
                "不会改动「自动卖」开关");
        }
        if (ImGui::Button("停止动作##home_asup_stop", ImVec2(-1.f, 0.f))) {
            if (!triggerAbort()) {
                notify::PushLocal(/*Warning*/ 2, "sellbag-abort", "停止失败", "写入命令失败", 3500);
            } else {
                // Stop 已清各 trip 开关；同步本地勾选
                autoSell = false;
                tripOnPotionEmpty = false;
                tripOnCustomLow = false;
                tripOnCustom2Low = false;
                tripOnFeedLow = false;
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "立即停止进行中的自动动作（卖装/补给/超级赶路等）：\n"
                "· 关掉「自动卖」与全部「回」阈值 / 缺药回城\n"
                "· 中止回城/超级赶路/开店/买卖\n"
                "· 取消待续回挂机图\n"
                "· 恢复战斗与飞行暂停\n"
                "若已在用回城卷换图，客户端仍会卸图完成（我方不再追加动作）");
        }
    }
    }
}
void DrawHangupScheduleTab(LaunchUiState& ui) {
    DesignBanner();

    static bool enabled = false;
    static uint32_t mask = xcat::kHangupScheduleMaskAll;
    static std::string s_loadedBin;
    static uint64_t s_uiWriteTick = 0;
    static bool s_saveFailed = false;

    auto loadCore = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControlSetDefaults(c);
        }
        enabled = c.launcherHangupSchedule != 0;
        mask = hangup_schedule::ClampScheduleMask(c.launcherHangupScheduleMask);
        s_uiWriteTick = c.writeTickMs;
    };

    auto saveCore = [&]() -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.launcherHangupSchedule = enabled ? 1u : 0u;
        c.launcherHangupScheduleMask = hangup_schedule::ClampScheduleMask(mask);
        c.writeTickMs = GetTickCount64();
        if (!xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) return false;
        s_uiWriteTick = c.writeTickMs;
        return true;
    };

    if (ui.prefsBinDir.empty()) {
    xcat::ui::CardGuard card("##tab_hangup", "挂机时段");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 user.ini [core] 挂机时段。");
        return;
    }

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadCore();
        s_saveFailed = false;
    } else if (!ImGui::IsAnyItemActive() && !s_saveFailed) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk) &&
            disk.writeTickMs != s_uiWriteTick) {
            loadCore();
        }
    }

    const hangup_schedule::Snapshot snap = hangup_schedule::GetSnapshot();

    {
        xcat::ui::CardGuard card("##tab_hangup", "挂机时段");
        if (xcat::ui::OptionCheckbox("启用挂机时段", &enabled)) {
            s_saveFailed = !saveCore();
        }
        ImGui::SetItemTooltip(
            "与守护模式独立。关闭时完全忽略下方小时表（不按时段杀/启）。\n"
            "开启后：未勾选小时会结束游戏进程；\n"
            "勾选小时会自动一键启动并注入（无人值守）。本机本地时间。");
    ImGui::SameLine();
        ImGui::TextDisabled("%s", hangup_schedule::UiModeLabel(snap.mode));

        if (!enabled) ImGui::BeginDisabled();
    if (ImGui::SmallButton("全选")) {
            mask = xcat::kHangupScheduleMaskAll;
            s_saveFailed = !saveCore();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("清空")) {
            mask = 0;
            s_saveFailed = !saveCore();
        }
        if (!enabled) ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.4f));
        if (enabled) {
            ImGui::TextDisabled("勾选即挂机；清空全部即全天关机");
        } else {
            ImGui::TextDisabled("总开关关闭时下方小时表不生效");
    }

    if (!enabled) ImGui::BeginDisabled();
        const int curHour = hangup_schedule::CurrentLocalHour();
    if (ImGui::BeginTable("##hangup_hours", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX |
                              ImGuiTableFlags_ScrollY,
                              ImVec2(0, AppDpi_Px(200.f)))) {
        ImGui::TableSetupColumn("时段", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 3.2f);
        ImGui::TableHeadersRow();
        for (int hour = 0; hour < 24; ++hour) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32]{};
            snprintf(label, sizeof(label), "%d:00 - %d:59", hour, hour);
                if (hour == curHour)
                ImGui::Text("%s（当前）", label);
            else
                ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
                bool on = (mask & (1u << hour)) != 0;
            ImGui::PushID(hour);
                if (ImGui::Checkbox("##h", &on)) {
                    if (on) mask |= (1u << hour);
                    else mask &= ~(1u << hour);
                    mask = hangup_schedule::ClampScheduleMask(mask);
                    s_saveFailed = !saveCore();
                }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!enabled) ImGui::EndDisabled();
    }

    if (s_saveFailed) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [core] 失败");
    }
}

void DrawMultiSkillTab(LaunchUiState& ui) {
    DesignBanner();

    static bool master = false;
    static bool safeSerial = true;
    static bool sendUseRequest = false;
    static int gapMs = static_cast<int>(xcat::kMultiSkillGapDefaultMs);
    static char search[64]{};
    static std::vector<std::string> selected;
    static std::vector<xcat::LearnedSkillRow> learned;
    static std::string s_loadedBin;
    static uint64_t s_uiWriteTick = 0;
    static bool s_saveFailed = false;
    static bool s_showAllSkills = false;  // 默认只显示攻击候选（藏 BUFF/辅助）
    static char s_status[128]{};

    auto loadCore = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControlSetDefaults(c);
        }
        master = c.multiSkill != 0;
        safeSerial = c.multiSkillSafeStagger != 0;
        sendUseRequest = c.multiSkillSendUseRequest != 0;
        gapMs = static_cast<int>(xcat::ClampMultiSkillGapMs(c.multiSkillGapMs));
        s_uiWriteTick = c.writeTickMs;
        selected.clear();
        xcat::ReadMultiSkillSelect(ui.prefsBinDir.c_str(), selected);
        learned.clear();
        // 多发列表以 learned_skills.tsv 为准；勿用 BUFF runtime 全表覆盖（会混入辅助技）。
        xcat::ReadLearnedSkillsTsv(ui.prefsBinDir.c_str(), learned);
    };

    auto saveCore = [&]() -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.multiSkill = master ? 1u : 0u;
        c.multiSkillSafeStagger = safeSerial ? 1u : 0u;
        c.multiSkillSendUseRequest = sendUseRequest ? 1u : 0u;
        c.multiSkillGapMs = xcat::ClampMultiSkillGapMs(static_cast<uint32_t>(gapMs));
        c.writeTickMs = GetTickCount64();
        if (!xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) return false;
        if (!xcat::WriteMultiSkillSelect(ui.prefsBinDir.c_str(), selected)) return false;
        s_uiWriteTick = c.writeTickMs;
        return true;
    };

    auto isSelected = [&](const char* code) -> bool {
        if (!code) return false;
        for (const std::string& s : selected) {
            if (s == code) return true;
            if (xcat::IsNormalAttackCode(s.c_str()) && xcat::IsNormalAttackCode(code)) return true;
        }
        return false;
    };

    auto setSelected = [&](const char* code, bool on) {
        if (!code || !code[0]) return;
        const std::string norm =
            xcat::IsNormalAttackCode(code) ? xcat::kNormalAttackCode : std::string(code);
        selected.erase(std::remove_if(selected.begin(), selected.end(),
                                      [&](const std::string& s) {
                                          if (s == norm) return true;
                                          return xcat::IsNormalAttackCode(norm.c_str()) &&
                                                 xcat::IsNormalAttackCode(s.c_str());
                                      }),
                       selected.end());
        if (on) selected.push_back(norm);
    };

    if (ui.prefsBinDir.empty()) {
        xcat::ui::CardGuard card("##tab_multiskill", "技能多发");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 multiSkill / multiskill_select.tsv。");
        return;
    }

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadCore();
        s_saveFailed = false;
        s_status[0] = 0;
    } else if (!ImGui::IsAnyItemActive() && !s_saveFailed) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk) &&
            disk.writeTickMs != s_uiWriteTick) {
            loadCore();
        } else {
            // Soft refresh learned list without stomping in-edit checkboxes.
            learned.clear();
            xcat::ReadLearnedSkillsTsv(ui.prefsBinDir.c_str(), learned);
        }
    }

    xcat::ui::CardGuard card("##tab_multiskill", "技能多发", /*fillRemaining=*/true);
    bool changed = false;
    if (xcat::ui::OptionCheckbox("启用技能多发", &master)) changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled(master ? "已开" : "已关");
    if (xcat::ui::OptionCheckbox("安全串发", &safeSerial)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("开：攻击技间隔地板≥120ms；关：使用 gapMs（仍 clamp 1–500）");
    }
    if (xcat::ui::OptionCheckbox("技能发包直发", &sendUseRequest)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "可选（默认关）：\n"
            "· 技能 → 优先 UserLocal.SendSkillUseRequest（失败回退 DoActiveSkill）\n"
            "· 普通攻击 → 始终 OnFuncKey 正路组包（不接 Create(50) 手搓，避免踢号）\n"
            "BUFF 页不受此开关影响。");
    }
    ImGui::SetNextItemWidth(AppDpi_Px(120.f));
    if (ImGui::DragInt("串发间隔 ms", &gapMs, 1, static_cast<int>(xcat::kMultiSkillGapMinMs),
                       static_cast<int>(xcat::kMultiSkillGapMaxMs))) {
        changed = true;
    }

    if (ImGui::Button("清空勾选")) {
        selected.clear();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新列表")) {
        loadCore();
        // Bump buffs refreshSeq so payload heavy-rescan（若 buffs 在跑）.
        xcat::BuffsConfig bcfg{};
        if (xcat::ReadBuffs(ui.prefsBinDir.c_str(), bcfg)) {
            bcfg.refreshSeq += 1;
            bcfg.writeTickMs = GetTickCount64();
            (void)xcat::WriteBuffs(ui.prefsBinDir.c_str(), bcfg);
        }
        strncpy_s(s_status, "已请求刷新已学技能", _TRUNCATE);
    }
    ImGui::SameLine();
    if (ImGui::Button("测试一波")) {
        if (!master) {
            strncpy_s(s_status, "请先启用技能多发", _TRUNCATE);
        } else if (selected.empty()) {
            strncpy_s(s_status, "请先勾选技能", _TRUNCATE);
        } else if (xcat::WriteMultiSkillCastRequest(ui.prefsBinDir.c_str())) {
            strncpy_s(s_status, "已下发测试一波请求", _TRUNCATE);
            xcat::log::Ok("App", "multiSkill cast-request written");
        } else {
            strncpy_s(s_status, "写 cast-request 失败", _TRUNCATE);
        }
    }

    ImGui::TextDisabled("已勾选 %d · 已学 %d（进图后由 payload 刷新）",
                        static_cast<int>(selected.size()), static_cast<int>(learned.size()));
    if (s_status[0]) ImGui::TextDisabled("%s", s_status);

    ImGui::Checkbox("显示全部技能（含 BUFF/辅助）", &s_showAllSkills);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "默认只列 skill_catalog_full 攻击类(type=2)。\n"
            "團隊治癒/疾風之步等辅助(type=1)归 BUFF 页。");
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ms_search", "搜索技能名 / ID", search, sizeof(search));

    const auto& skillPack = xcat::GetSharedSkillNames(ui.prefsBinDir.c_str());

    // Ensure selected-only codes still appear even if not in learned cache.
    struct Row {
        std::string code;
        std::string name;
        int level = 0;
    };
    std::vector<Row> rows;
    rows.reserve(learned.size() + selected.size() + 1);
    auto pushDisplayRow = [&](const char* code, const char* name, int level) {
        if (!code || !code[0]) return;
        for (const Row& old : rows) {
            if (old.code == code) return;
            if (xcat::IsNormalAttackCode(old.code.c_str()) && xcat::IsNormalAttackCode(code))
                return;
        }
        char label[128]{};
        if (xcat::IsNormalAttackCode(code)) {
            strncpy_s(label, xcat::kNormalAttackDisplayName, _TRUNCATE);
        } else {
            xcat::BuffSkillDisplayLabel(code, name, label, sizeof(label), ui.prefsBinDir.c_str());
        }
            Row r;
        r.code = xcat::IsNormalAttackCode(code) ? xcat::kNormalAttackCode : code;
        r.name = label[0] ? label : (name && name[0] ? name : code);
        r.level = level;
            rows.push_back(std::move(r));
    };
    // 普攻固定置顶（非 SkillRecord；走 attack_input_port）。
    pushDisplayRow(xcat::kNormalAttackCode, xcat::kNormalAttackDisplayName, 0);
    for (const auto& e : learned) {
        pushDisplayRow(e.code, e.name, e.level);
        }
    for (const std::string& code : selected) {
        pushDisplayRow(code.c_str(), code.c_str(), 0);
    }

    int hiddenByType = 0;
    int shown = 0;
    if (ImGui::BeginTable("##ms_table", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("开", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
        ImGui::TableSetupColumn("技能", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (search[0]) {
                if (!strstr(r.name.c_str(), search) && !strstr(r.code.c_str(), search)) continue;
            }
            const bool sel = isSelected(r.code.c_str());
            if (!s_showAllSkills &&
                !xcat::SkillLooksLikeAttackCandidate(skillPack, r.code.c_str(), sel)) {
                ++hiddenByType;
                continue;
            }
            ++shown;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = sel;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox("##sel", &on)) {
                setSelected(r.code.c_str(), on);
                changed = true;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.code.c_str());
        }
        ImGui::EndTable();
    }
    if (shown == 0) {
        if (!skillPack.typesLoaded) {
            ImGui::TextDisabled("未加载 skill_catalog_full.tsv，类型筛选暂不可用（等同显示全部）。");
        } else if (hiddenByType > 0) {
            ImGui::TextDisabled("没有匹配的攻击技（已隐藏 %d 个辅助/被动）。", hiddenByType);
        } else {
            ImGui::TextDisabled("暂无已学技能（进图后点刷新）。");
        }
    } else if (!s_showAllSkills && hiddenByType > 0) {
        ImGui::TextDisabled("已隐藏 %d 个辅助/被动技", hiddenByType);
    }

    if (changed) {
        s_saveFailed = !saveCore();
        if (!s_saveFailed) {
            xcat::log::Ok("App", "已下发 multiSkill=%d gap=%d safe=%d sendUse=%d sel=%d",
                          master ? 1 : 0, gapMs, safeSerial ? 1 : 0, sendUseRequest ? 1 : 0,
                          static_cast<int>(selected.size()));
        }
    }
    if (s_saveFailed) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 multiSkill / 勾选清单失败");
    }
}

void DrawReloginTab(LaunchUiState& ui) {
    DesignBanner();

    static bool detect = true;
    static bool stopCombat = false;
    static bool stopGather = false;
    static bool channelHop = true;
    static bool gmEscalate = true;
    static bool hideOthers = false;
    static bool gatherOn = false;
    static std::string s_loadedBin;
    static uint64_t s_lastTick = 0;
    static bool s_saveFailed = false;

    auto loadUi = [&]() {
        if (ui.prefsBinDir.empty()) {
            detect = true;
            stopCombat = false;
            stopGather = false;
            channelHop = true;
            gmEscalate = true;
            hideOthers = false;
            gatherOn = false;
            s_lastTick = 0;
            return;
        }
        xcat::PayloadControl c{};
        if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControlSetDefaults(c);
        }
        detect = c.autoRelogin != 0;
        stopCombat = c.autoReloginStopCombat != 0;
        stopGather = c.autoReloginStopGather != 0;
        channelHop = c.autoReloginReconnect != 0;
        gmEscalate = c.autoReloginGmEscalate != 0;
        hideOthers = c.hideOtherPlayers != 0;
        gatherOn = c.mobGather != 0;
        s_lastTick = c.writeTickMs;
    };

    auto saveUi = [&]() -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.autoRelogin = detect ? 1u : 0u;
        c.autoReloginStopCombat = stopCombat ? 1u : 0u;
        c.autoReloginStopGather = stopGather ? 1u : 0u;
        c.autoReloginReconnect = channelHop ? 1u : 0u;
        c.autoReloginGmEscalate = gmEscalate ? 1u : 0u;
        c.hideOtherPlayers = hideOthers ? 1u : 0u;
        xcat::ApplyMobGatherEncounterForce(c);
        c.writeTickMs = GetTickCount64();
        if (!xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) return false;
        s_lastTick = c.writeTickMs;
        return true;
    };

    // 保存失败时回滚到磁盘，避免 ImGui 已翻转、DLL/ini 仍旧值（界面关实际开）。
    auto trySaveOrRevert = [&]() {
        if (saveUi()) {
            s_saveFailed = false;
            return;
        }
        s_saveFailed = true;
        loadUi();
    };

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadUi();
        s_saveFailed = false;
    } else if (!ui.prefsBinDir.empty() && !ImGui::IsAnyItemActive() && !s_saveFailed) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk) &&
            disk.writeTickMs != s_lastTick) {
            loadUi();
        }
    }

    const bool gatherForce = gatherOn || gUiMobGather;
    if (gatherForce) {
        detect = true;
        stopGather = true;
        channelHop = true;
    }

    xcat::ui::CardGuard card("##tab_relogin", "有人来时怎么办");
    ImGui::BeginDisabled(gatherForce);
    if (xcat::ui::OptionCheckbox("检测同图玩家", &detect)) trySaveOrRevert();
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextUnformatted("处理流程");
    if (xcat::ui::OptionCheckbox("先停手", &stopCombat)) trySaveOrRevert();
    if (WorkspaceGatherTabUnlocked() || gatherForce) {
        ImGui::BeginDisabled(gatherForce);
        if (xcat::ui::OptionCheckbox("遇人停吸", &stopGather)) trySaveOrRevert();
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            gatherForce ? "吸怪开启时强制遇人停吸 + 软重连进新频，避免别人看见吸怪。"
                        : "勾选后遇人（含 GM/隐身升级）会卸掉正在吸的怪，人走净再继续。不改「吸怪 快攻」TAB 开关。");
    }
    ImGui::BeginDisabled(gatherForce);
    if (xcat::ui::OptionCheckbox("一直有人就换频", &channelHop)) trySaveOrRevert();
    ImGui::EndDisabled();
    ImGui::TextDisabled("勾上：遇人则软重连进随机新频，不回原频道。首页「软重连试连」须开。F10 手动仍走游戏换频。");
    if (gatherForce) {
        ImGui::TextDisabled("吸怪开启时强制启用，关吸怪后才能改。");
    }
    if (xcat::ui::OptionCheckbox("GM/隐身立即处理", &gmEscalate)) trySaveOrRevert();
    ImGui::TextDisabled(
        "Admin/Manager(800/900)或未藏人时的隐身实体 → 立刻停手/软重连进新频 + 强制 Alarm（不依赖上方停手/换频勾选）。");
        ImGui::TextDisabled("关则普通遇人仍跟「先停手 / 遇人停吸 / 一直有人就换频」；服务端不广播跟踪仍无法发现。");
    ImGui::Separator();
    if (xcat::ui::OptionCheckbox("隐藏同图其他玩家", &hideOthers)) trySaveOrRevert();
    ImGui::TextDisabled("藏皮/伤字(DamageSkin)/技能特效；自己可见；不影响遇人人数检测。");
    if (s_saveFailed) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存遇人策略失败（已恢复为磁盘值）");
    }
}

void DrawTimedKeysTab(LaunchUiState& ui) {
    DesignBanner();

    // 独立 UI 状态：避免 !IsAnyItemActive 时每帧 Load 与 DragInt 打架。
    struct TimedKeysUiState {
        bool enabled[xcat::kTimedKeySlotCount]{};
        int intervalSec[xcat::kTimedKeySlotCount]{};
    };
    static TimedKeysUiState s_ui{};
    static std::string s_loadedBin;
    static bool s_saveFailed = false;
    static uint64_t s_uiWriteTick = 0;

    auto loadUi = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::TimedKeysConfig cfg{};
        if (!xcat::ReadTimedKeys(ui.prefsBinDir.c_str(), cfg)) {
            xcat::TimedKeysSetDefaults(cfg);
        }
        s_uiWriteTick = cfg.writeTickMs;
        for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
            s_ui.enabled[i] = cfg.slots[i].enabled != 0;
            s_ui.intervalSec[i] =
                static_cast<int>(xcat::TimedKeysIntervalSecFromMs(cfg.slots[i].intervalMs));
        }
    };

    auto saveUi = [&]() -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::TimedKeysConfig cfg{};
        if (!xcat::ReadTimedKeys(ui.prefsBinDir.c_str(), cfg)) {
            xcat::TimedKeysSetDefaults(cfg);
        }
        // payload halt 等外部写盘会抬高 writeTick；编辑中保存时同步已取消的勾选。
        if (cfg.writeTickMs > s_uiWriteTick) {
            for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
                if (cfg.slots[i].enabled == 0) s_ui.enabled[i] = false;
            }
            s_uiWriteTick = cfg.writeTickMs;
        }
        for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
            cfg.slots[i].enabled = s_ui.enabled[i] ? 1u : 0u;
            cfg.slots[i].intervalMs =
                xcat::TimedKeysIntervalMsFromSec(static_cast<uint32_t>(s_ui.intervalSec[i]));
        }
        cfg.magic = xcat::kTimedKeysMagic;
        cfg.version = xcat::kTimedKeysVersion;
        cfg.masterEnabled = xcat::TimedKeysAnySlotEnabled(cfg) ? 1u : 0u;
        if (!xcat::WriteTimedKeys(ui.prefsBinDir.c_str(), cfg)) return false;
        xcat::TimedKeysConfig after{};
        xcat::ReadTimedKeys(ui.prefsBinDir.c_str(), after);
        s_uiWriteTick = after.writeTickMs;
        return true;
    };

    if (ui.prefsBinDir.empty()) {
        xcat::ui::CardGuard card("##tab_timed", "定时按键");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 user.ini [timed_keys]。");
        return;
    }

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadUi();
        xcat::TimedKeysConfig probe{};
        if (!xcat::ReadTimedKeys(ui.prefsBinDir.c_str(), probe)) {
            s_saveFailed = !saveUi();
        } else {
            s_saveFailed = false;
        }
    } else if (!ImGui::IsAnyItemActive() && !s_saveFailed) {
        loadUi();
    }

    xcat::ui::CardGuard card("##tab_timed", "定时按键");
    ImGui::TextDisabled("勾选即启用；取消全部勾选即关闭（需注入后生效）");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "循环按下 7/8/9/0/-/=/Z；勾选后立即触发一次，之后按各键自身间隔重复。\n"
            "多键同时到期时约 150ms 依次触发；间隔 1~3600 秒，含 ±0.5s 抖动。\n"
            "按键走 InputManager.KeyDownTouch（非 SendInput）。");
    }
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.5f));

    bool changed = false;
    if (ImGui::BeginTable("##timed_keys", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(40.f));
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(48.f));
        ImGui::TableSetupColumn("间隔秒", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < xcat::kTimedKeySlotCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(xcat::TimedKeySlotLabel(i));
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (xcat::ui::OptionCheckbox("##en", &s_ui.enabled[i])) changed = true;
            ImGui::TableNextColumn();
            if (!s_ui.enabled[i]) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragInt("##sec", &s_ui.intervalSec[i], 1,
                               static_cast<int>(xcat::kTimedKeysMinIntervalSec),
                               static_cast<int>(xcat::kTimedKeysMaxIntervalSec))) {
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("间隔含 ±0.5s 抖动");
            }
            if (!s_ui.enabled[i]) ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (changed) s_saveFailed = !saveUi();
    if (s_saveFailed) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [timed_keys] 失败");
}

void DrawBuffsTab(LaunchUiState& ui) {
    DesignBanner();

    static std::string s_loadedBin;
    static xcat::BuffsConfig s_cfg{};
    static bool s_saveFailed = false;
    static char search[64]{};
    static bool s_showAllSkills = false;  // 默认只显示辅助/BUFF 候选
    static uint32_t s_refreshExpectSeq = 0;
    static uint64_t s_refreshRequestedAt = 0;
    static uint64_t s_hintUntil = 0;
    static const char* s_refreshHint = "";

    auto loadCfg = [&]() {
        if (ui.prefsBinDir.empty()) return;
        if (!xcat::ReadBuffs(ui.prefsBinDir.c_str(), s_cfg)) xcat::BuffsSetDefaults(s_cfg);
    };

    auto saveCfg = [&]() -> bool {
        if (ui.prefsBinDir.empty()) return false;
        s_cfg.magic = xcat::kBuffsMagic;
        s_cfg.version = xcat::kBuffsVersion;
        s_cfg.writeTickMs = GetTickCount64();
        return xcat::WriteBuffs(ui.prefsBinDir.c_str(), s_cfg);
    };

    auto findSlot = [](const xcat::BuffsConfig& cfg, const char* code) -> int {
        if (!code || !code[0]) return -1;
        for (size_t i = 0; i < xcat::kBuffSlotCount; ++i) {
            if (cfg.slots[i].code[0] && strcmp(cfg.slots[i].code, code) == 0)
                return static_cast<int>(i);
        }
        return -1;
    };

    auto allocSlot = [](xcat::BuffsConfig& cfg, const char* code) -> int {
        int existing = -1;
        for (size_t i = 0; i < xcat::kBuffSlotCount; ++i) {
            if (cfg.slots[i].code[0] && strcmp(cfg.slots[i].code, code) == 0)
                return static_cast<int>(i);
            if (existing < 0 && !cfg.slots[i].code[0]) existing = static_cast<int>(i);
        }
        if (existing < 0) return -1;
        strncpy_s(cfg.slots[existing].code, code, _TRUNCATE);
        cfg.slots[existing].kind = xcat::kBuffKindSkill;
        cfg.slots[existing].strategy = xcat::kBuffRenewByPresence;
        cfg.slots[existing].intervalSec = 180;
        return existing;
    };

    auto formatSec = [](float sec, char* out, size_t outSz) {
        if (sec <= 0.f) {
            strncpy_s(out, outSz, "-", _TRUNCATE);
            return;
        }
        // <3 分钟一律显示秒，避免 65s→「1m」掩盖数量级错误（肉眼十几秒却显示 1m）。
        if (sec >= 180.f)
            snprintf(out, outSz, "%dm", (int)(sec / 60.f + 0.5f));
        else
            snprintf(out, outSz, "%ds", (int)(sec + 0.5f));
    };

    if (ui.prefsBinDir.empty()) {
        xcat::ui::CardGuard card("##tab_buffs", "BUFF 管理器");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 user.ini [buffs]。");
        return;
    }

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadCfg();
        s_saveFailed = false;
    } else if (!ImGui::IsAnyItemActive() && !s_saveFailed) {
        loadCfg();
    }

    xcat::BuffsRuntimeSnapshot snapshot{};
    const bool haveSnapshot = xcat::ReadBuffsRuntimeSnapshot(ui.prefsBinDir.c_str(), snapshot);
    const uint64_t now = GetTickCount64();
    const bool fresh = haveSnapshot && snapshot.writeTickMs && now >= snapshot.writeTickMs &&
                       now - snapshot.writeTickMs <= 60000;
    bool changed = false;

    const bool refreshPending =
        s_refreshRequestedAt != 0 && (now - s_refreshRequestedAt) < 15000ull &&
        snapshot.refreshAckSeq < s_refreshExpectSeq;
    if (s_refreshRequestedAt != 0 && snapshot.refreshAckSeq >= s_refreshExpectSeq) {
        s_refreshRequestedAt = 0;
        s_refreshHint = "列表已刷新";
        s_hintUntil = now + 3000ull;
    } else if (s_refreshRequestedAt != 0 && (now - s_refreshRequestedAt) >= 15000ull) {
        s_refreshRequestedAt = 0;
        s_refreshHint = "刷新超时：请确认已进图且 payload 在线";
        s_hintUntil = now + 5000ull;
    }
    if (s_hintUntil && now >= s_hintUntil) {
        s_hintUntil = 0;
        s_refreshHint = "";
    }

    xcat::ui::CardGuard card("##tab_buffs", "BUFF 管理器", /*fillRemaining=*/true);
    bool master = s_cfg.masterEnabled != 0;
    if (xcat::ui::OptionCheckbox("启用 BUFF 续航", &master)) {
        const bool was = s_cfg.masterEnabled != 0;
        s_cfg.masterEnabled = master ? 1u : 0u;
        changed = true;
        if (!was && master) {
            s_cfg.refreshSeq = s_cfg.refreshSeq == 0 ? 1u : s_cfg.refreshSeq + 1u;
            s_refreshExpectSeq = s_cfg.refreshSeq;
            s_refreshRequestedAt = now;
            s_refreshHint = "已请求刷新列表…";
            s_hintUntil = now + 15000ull;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
        s_cfg.refreshSeq = s_cfg.refreshSeq == 0 ? 1u : s_cfg.refreshSeq + 1u;
        if (!saveCfg()) {
            s_refreshHint = "保存 user.ini [buffs] 失败";
            s_hintUntil = now + 5000ull;
            s_saveFailed = true;
        } else {
            s_refreshExpectSeq = s_cfg.refreshSeq;
            s_refreshRequestedAt = now;
            s_refreshHint = "已请求刷新…";
            s_hintUntil = now + 15000ull;
            s_saveFailed = false;
            changed = false;  // already saved
        }
    }
    ImGui::SameLine();
    const char* status = refreshPending
                             ? "刷新中…"
                             : (fresh ? (snapshot.status[0] ? snapshot.status : "runtime 已连接")
                                      : (haveSnapshot ? "快照过期：请点刷新"
                                                      : "等待 payload BUFF 快照（检查注入/进图）"));
    ImGui::TextDisabled("%s", status);
    if (s_refreshHint && s_refreshHint[0]) ImGui::TextDisabled("%s", s_refreshHint);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##buff_search", "搜索技能名 / 技能ID", search, sizeof(search));
    xcat::ui::OptionCheckbox("显示全部已学技能", &s_showAllSkills);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "默认只显示离线表判定为「辅助」的技能（治疗/祝福/增益等）。\n"
            "在身中、已开槽的始终显示。\n"
            "官方 SkillEntry.SkillType 是 Mastery/Booster/FinalAttack，\n"
            "不是 BUFF/攻击分类，故用 skill_catalog_full 启发式 type。");
    }

    int shown = 0;
    int hiddenByType = 0;
    if (fresh) {
        const xcat::SkillNamesPack& skillPack =
            xcat::GetSharedSkillNames(ui.prefsBinDir.c_str());
        if (ImGui::BeginTable("##buff_table", 5,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 0))) {
            ImGui::TableSetupColumn("开", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 2.2f);
            ImGui::TableSetupColumn("技能", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("剩余", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(48.f));
            ImGui::TableSetupColumn("CD", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(48.f));
            ImGui::TableSetupColumn("策略", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(72.f));
            ImGui::TableHeadersRow();
            const uint32_t count =
                snapshot.count < xcat::kBuffsRuntimeMaxSkills ? snapshot.count
                                                              : (uint32_t)xcat::kBuffsRuntimeMaxSkills;
            for (uint32_t i = 0; i < count; ++i) {
                const auto& skill = snapshot.skills[i];
                if (!skill.learned && !skill.active) continue;
                char label[160]{};
                xcat::BuffSkillDisplayLabel(skill.code, skill.name, label, sizeof(label),
                                            ui.prefsBinDir.c_str());
                if (search[0] && !strstr(label, search) && !strstr(skill.code, search)) continue;
                int slot = findSlot(s_cfg, skill.code);
                bool enabled = slot >= 0 && s_cfg.slots[slot].enabled != 0;
                if (!s_showAllSkills &&
                    !xcat::SkillLooksLikeBuffCandidate(skillPack, skill.code,
                                                       skill.active != 0 || enabled)) {
                    ++hiddenByType;
                    continue;
                }
                ++shown;
                ImGui::PushID(skill.code[0] ? skill.code : label);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##on", &enabled)) {
                    slot = allocSlot(s_cfg, skill.code);
                    if (slot >= 0) {
                        s_cfg.slots[slot].enabled = enabled ? 1u : 0u;
                        changed = true;
                    }
                }
                ImGui::TableNextColumn();
                ImGui::Text("%s %s", skill.active ? "*" : " ", label);
                if (ImGui::IsItemHovered() && skill.code[0])
                    ImGui::SetTooltip("技能ID: %s", skill.code);
                ImGui::TableNextColumn();
                char remain[24]{};
                formatSec(skill.remainBuffSec, remain, sizeof(remain));
                ImGui::TextUnformatted(skill.active ? remain : "-");
                ImGui::TableNextColumn();
                {
                    // CD>0 优先显示剩余冷却；否则可放 / -
                    char cd[24]{};
                    if (skill.remainCooldownSec > 0.01f) {
                    formatSec(skill.remainCooldownSec, cd, sizeof(cd));
                    ImGui::TextUnformatted(cd);
                    } else if (skill.canCast || skill.learned) {
                        ImGui::TextUnformatted("可放");
                    } else {
                        ImGui::TextUnformatted("-");
                    }
                }
                ImGui::TableNextColumn();
                if (slot >= 0) {
                    int strat = static_cast<int>(s_cfg.slots[slot].strategy);
                    if (strat < 0 || strat > 2) strat = 0;
                    const char* strategies[] = {"掉了补", "CD好补", "按间隔"};
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##st", &strat, strategies, 3)) {
                        s_cfg.slots[slot].strategy = static_cast<uint32_t>(strat);
                        changed = true;
                    }
                } else {
                    ImGui::TextDisabled("掉了补");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (shown == 0) {
            ImGui::TextDisabled(hiddenByType > 0
                                    ? "没有匹配的辅助技（可开「显示全部已学技能」或改搜索）。"
                                    : "没有匹配的已学 BUFF（进图后点刷新）。");
        } else if (!s_showAllSkills && hiddenByType > 0) {
            ImGui::TextDisabled("已隐藏 %d 个攻击/被动技", hiddenByType);
        }
        if (!skillPack.typesLoaded) {
            ImGui::TextDisabled("未加载 skill_catalog_full.tsv，类型筛选暂不可用（等同显示全部）。");
        }
    } else {
        ImGui::TextDisabled("等待注入后的技能列表快照…");
    }

    if (changed) s_saveFailed = !saveCfg();
    if (s_saveFailed)
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [buffs] 失败");
}

void DrawTravelTab(LaunchUiState& ui) {
    DesignBanner();
    static char go[128]{};
    static char search[128]{};
    static int region = 0;
    static int selected = -1;
    static std::string status;
    static std::string loadedBin;
    static uint32_t lastAutoSyncMapId = 0;  // 跟盘：mapId 变化时切板块+选中当前图（低频）
    static bool lastAutoSyncHave = false;   // 出生图 mapId=0，不能用 0 当「从未跟盘」
    static bool scrollToSelected = false;
    static std::vector<std::string> regions{"全部大区"};
    struct MapItem {
        std::string key;
        std::string name;
        std::string street;
    };
    static std::vector<MapItem> catalog;

    auto padKey = [](const std::string& raw) -> std::string {
        if (raw.empty()) return {};
        for (char c : raw) {
            if (c < '0' || c > '9') return raw;
        }
        if (raw.size() >= 9) return raw;
        return std::string(9 - raw.size(), '0') + raw;
    };

    auto writeTravelCmd = [&](const std::string& cmd) -> bool {
        if (ui.prefsBinDir.empty()) return false;
        const std::string stateDir = xcat::JoinBinPath(ui.prefsBinDir.c_str(), "state");
        (void)xcat::CreateDirectoryUtf8(stateDir);
        const std::string path = xcat::JoinBinPath(ui.prefsBinDir.c_str(), "state\\travel_cmd.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
        const size_t n = fwrite(cmd.data(), 1, cmd.size(), f);
        fclose(f);
        return n == cmd.size();
    };

    auto selectRegionByStreet = [&](const std::string& street) -> bool {
        if (street.empty()) return false;
        for (int i = 1; i < static_cast<int>(regions.size()); ++i) {
            if (regions[i] == street) {
                region = i;
                return true;
            }
        }
        return false;
    };

    // 同一次线性扫：切大区 + 高亮当前图；不改 go（避免冲掉用户已填目的地）
    auto syncCatalogToMapKey = [&](const std::string& mapKey) -> bool {
        if (mapKey.empty() || catalog.empty()) return false;
        for (int i = 0; i < static_cast<int>(catalog.size()); ++i) {
            if (catalog[i].key != mapKey) continue;
            (void)selectRegionByStreet(catalog[i].street);
            selected = i;
            scrollToSelected = true;
            return true;
        }
        return false;
    };

    auto reloadCatalog = [&]() {
        catalog.clear();
        regions = {"全部大区"};
        selected = -1;
        lastAutoSyncMapId = 0;  // 重载后按当前图再跟一次
        lastAutoSyncHave = false;
        scrollToSelected = false;
        if (ui.prefsBinDir.empty()) return;
        const std::string path =
            xcat::JoinBinPath(ui.prefsBinDir.c_str(), "dataservice\\map_names.tsv");
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;
        char line[1024]{};
        std::unordered_set<std::string> seenStreet;
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (!len || line[0] == '#') continue;
            // code \t streetName \t mapName \t mapDesc
            char* t0 = strchr(line, '\t');
            if (!t0) continue;
            *t0 = 0;
            char* t1 = strchr(t0 + 1, '\t');
            if (t1) *t1 = 0;
            char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
            if (t2) *t2 = 0;
            MapItem it;
            it.key = padKey(line);
            it.street = t0 + 1;
            it.name = t1 ? (t1 + 1) : "";
            if (it.key.empty()) continue;
            if (it.name.empty()) it.name = it.street;
            catalog.push_back(it);
            if (!it.street.empty() && seenStreet.insert(it.street).second)
                regions.push_back(it.street);
        }
        fclose(f);
        std::sort(regions.begin() + 1, regions.end());
        std::sort(catalog.begin(), catalog.end(), [](const MapItem& a, const MapItem& b) {
            if (a.street != b.street) return a.street < b.street;
            return a.name < b.name;
        });
    };

    if (loadedBin != ui.prefsBinDir) {
        loadedBin = ui.prefsBinDir;
        lastAutoSyncMapId = 0;
        reloadCatalog();
    }

    // 低频跟盘：仅 mapId 边沿；手选了其它图则不抢（仍跟上次自动选中的「当前图」）
    // 出生图 Field 0：playReady + HasMapData 时 st.mapId=0 仍是合法图，禁止再用 mapId>0。
    if (!catalog.empty() && !ui.prefsBinDir.empty()) {
        xcat::PayloadStatus st{};
        if (xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) &&
            xcat::PayloadStatusHeartbeatFresh(st, GetTickCount64(), 5000) && st.playReady != 0 &&
            (!lastAutoSyncHave || st.mapId != lastAutoSyncMapId)) {
            const std::string prevKey =
                lastAutoSyncHave ? padKey(std::to_string(lastAutoSyncMapId)) : std::string{};
            const bool follow =
                selected < 0 ||
                (!prevKey.empty() && selected < static_cast<int>(catalog.size()) &&
                 catalog[selected].key == prevKey);
            const std::string key = padKey(std::to_string(st.mapId));
            if (follow) {
                if (syncCatalogToMapKey(key)) {
                    lastAutoSyncMapId = st.mapId;
                    lastAutoSyncHave = true;
                }
            } else {
                lastAutoSyncMapId = st.mapId;
                lastAutoSyncHave = true;
            }
        }
    }

    const MapItem* selectedItem =
        (selected >= 0 && selected < static_cast<int>(catalog.size())) ? &catalog[selected]
                                                                       : nullptr;

    {
        xcat::ui::CardGuard card("##tab_travel_dest", "目的地");
        if (selectedItem)
            ImGui::Text("已选  %s · %s", selectedItem->name.c_str(), selectedItem->key.c_str());
        else
            ImGui::TextDisabled("从下方目录点选，或输入地图名 / 图号");

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##go", "地图名 / 图号", go, sizeof(go));
        const bool canGo = go[0] != '\0';
        ImGui::BeginDisabled(!canGo);
        if (ImGui::Button("开始赶路", ImVec2(AppDpi_Px(100.f), 0.f)) && canGo) {
            std::string cmd = "goto ";
            cmd += go;
            status = writeTravelCmd(cmd) ? ("已下发：前往 " + std::string(go)) : "写入失败";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("停止赶路", ImVec2(AppDpi_Px(100.f), 0.f))) {
            status = writeTravelCmd("stop") ? "已下发：停止" : "写入失败";
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!selectedItem);
        if (ImGui::Button("填入已选", ImVec2(AppDpi_Px(100.f), 0.f)) && selectedItem) {
            strncpy_s(go, selectedItem->key.c_str(), _TRUNCATE);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("目录双击 / 世界地图 Spot 确认 = 赶路");
        if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_travel_cat", "地图目录", /*fillRemaining=*/true);
        ImGui::SetNextItemWidth(AppDpi_Px(160.f));
        const char* regionPreview =
            (region >= 0 && region < static_cast<int>(regions.size())) ? regions[region].c_str()
                                                                       : "全部大区";
        if (ImGui::BeginCombo("##region", regionPreview)) {
            for (int i = 0; i < static_cast<int>(regions.size()); ++i) {
                const bool sel = (i == region);
                if (ImGui::Selectable(regions[i].c_str(), sel)) region = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##map_search", "搜索", search, sizeof(search));
        if (ImGui::Button("重新加载")) {
            reloadCatalog();
            status = catalog.empty() ? "未找到 map_names.tsv" : "目录已刷新";
        }
        ImGui::SameLine();
        if (ImGui::Button("保存学习图")) {
            status = writeTravelCmd("save") ? "已下发：保存学习图" : "写入失败";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("目录 %d 张", static_cast<int>(catalog.size()));

        ImGui::BeginChild("##map_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        int shown = 0;
        const char* regionNeed =
            (region > 0 && region < static_cast<int>(regions.size())) ? regions[region].c_str()
                                                                     : nullptr;
        for (int i = 0; i < static_cast<int>(catalog.size()); ++i) {
            const MapItem& it = catalog[i];
            if (regionNeed && it.street != regionNeed) continue;
            if (search[0]) {
                if (!strstr(it.name.c_str(), search) && !strstr(it.key.c_str(), search) &&
                    !strstr(it.street.c_str(), search))
                    continue;
            }
            if (++shown > 400) {
                ImGui::TextDisabled("结果过多，仅显示前 400 条");
                break;
            }
            char line[192]{};
            snprintf(line, sizeof(line), "%s  (%s)", it.name.c_str(), it.key.c_str());
            if (ImGui::Selectable(line, selected == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = i;
                strncpy_s(go, it.key.c_str(), _TRUNCATE);
                if (ImGui::IsMouseDoubleClicked(0)) {
                    std::string cmd = "goto ";
                    cmd += it.key;
                    status = writeTravelCmd(cmd) ? ("已下发：前往 " + it.name) : "写入失败";
                }
            }
            if (selected == i && scrollToSelected) {
                ImGui::SetScrollHereY(0.25f);
                scrollToSelected = false;
            }
        }
        ImGui::EndChild();
    }
}

void DrawBetaTab(LaunchUiState& ui) {
    DesignBanner();
    static bool dropInCombat = true;
    static bool auctionTownBypass = true;
    static bool restMpAccel = false;
    static int restMpAccelIntervalMs = (int)xcat::kRestMpAccelIntervalDefaultMs;
    static bool forceTrade = false;
    static bool frameLock = true;
    static int frameLockFps = (int)xcat::kFrameLockFpsDefault;
    static bool skipDialog = false;
    static bool autoAccept = true;
    static bool autoFirst = false;
    static bool dropLoaded = false;
    static uint64_t dropSeenTick = 0;

    if (!ui.prefsBinDir.empty()) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
            if (!dropLoaded || disk.writeTickMs != dropSeenTick) {
                dropInCombat = disk.dropAlertBypass != 0;
                auctionTownBypass = disk.auctionTownBypass != 0;
                restMpAccel = disk.restMpAccel != 0;
                restMpAccelIntervalMs = (int)xcat::ClampRestMpAccelIntervalMs(
                    disk.restMpAccelIntervalMs ? disk.restMpAccelIntervalMs
                                               : xcat::kRestMpAccelIntervalDefaultMs);
                forceTrade = xcat::kForceTradeUserEnabled && disk.forceTrade != 0;
                frameLock = disk.frameLock != 0;
                frameLockFps = (int)xcat::ClampFrameLockFps(
                    disk.frameLockFps ? disk.frameLockFps : xcat::kFrameLockFpsDefault);
                gUiCombatLiveStep = disk.simpleCombatLiveStep != 0;
                gUiAttackRpc = disk.attackRpc != 0;
                gUiCurFhGateBypass = disk.curFhGateBypass != 0;
                gUiAttackRpcMobs = (int)xcat::ClampAttackRpcMobs(
                    disk.attackRpcMobs ? disk.attackRpcMobs : xcat::kAttackRpcMobsDefault);
                gUiAttackRpcIntervalMs = (int)xcat::ClampAttackRpcIntervalMs(
                    disk.attackRpcIntervalMs ? disk.attackRpcIntervalMs
                                             : xcat::kAttackRpcIntervalDefaultMs);
                gUiAttackRpcDamage = (int)xcat::ClampAttackRpcDamage(
                    disk.attackRpcDamage ? disk.attackRpcDamage
                                         : xcat::kAttackRpcDamageDefault);
                gUiCombatForgeHit = disk.simpleCombatForgeHit != 0;
                gUiForgeHitFrontDx = (int)xcat::ClampForgeHitFrontDx(disk.simpleCombatForgeHitFrontDx);
                gUiForgeHitFrontDy = (int)xcat::ClampForgeHitFrontDy(disk.simpleCombatForgeHitFrontDy);
                gUiMapAttack = disk.mapAttack != 0;
                dropSeenTick = disk.writeTickMs;
                dropLoaded = true;
            }
        } else if (!dropLoaded) {
            dropLoaded = true;
        }
    } else if (!dropLoaded) {
        dropLoaded = true;
    }

    auto persistDrop = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.dropAlertBypass = dropInCombat ? 1u : 0u;
        c.auctionTownBypass = auctionTownBypass ? 1u : 0u;
        c.restMpAccel = restMpAccel ? 1u : 0u;
        c.restMpAccelIntervalMs = xcat::ClampRestMpAccelIntervalMs(
            static_cast<uint32_t>(restMpAccelIntervalMs < 0 ? 0 : restMpAccelIntervalMs));
        restMpAccelIntervalMs = (int)c.restMpAccelIntervalMs;
        c.forceTrade = (xcat::kForceTradeUserEnabled && forceTrade) ? 1u : 0u;
        c.infiniteStars = 0;
        c.frameLock = frameLock ? 1u : 0u;
        c.frameLockFps = xcat::ClampFrameLockFps(
            static_cast<uint32_t>(frameLockFps < 0 ? 0 : frameLockFps));
        frameLockFps = (int)c.frameLockFps;
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            // WritePayloadControl 可能单调 +1 writeTickMs；必须回读，否则
            // dropSeenTick 落后 → 下帧从盘把勾选冲回旧值（曾出现开 1 半秒后被写回 0）。
            xcat::PayloadControl verify{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                dropSeenTick = verify.writeTickMs;
                dropInCombat = verify.dropAlertBypass != 0;
                auctionTownBypass = verify.auctionTownBypass != 0;
                restMpAccel = verify.restMpAccel != 0;
                restMpAccelIntervalMs = (int)xcat::ClampRestMpAccelIntervalMs(
                    verify.restMpAccelIntervalMs ? verify.restMpAccelIntervalMs
                                                 : xcat::kRestMpAccelIntervalDefaultMs);
                forceTrade = xcat::kForceTradeUserEnabled && verify.forceTrade != 0;
                frameLock = verify.frameLock != 0;
                frameLockFps = (int)xcat::ClampFrameLockFps(
                    verify.frameLockFps ? verify.frameLockFps : xcat::kFrameLockFpsDefault);
            } else {
                dropSeenTick = c.writeTickMs;
            }
            xcat::log::Ok("App",
                          "已下发 core：战斗中可丢物=%d 野外可开拍卖=%d 坐下回蓝加速=%d "
                          "回蓝间隔=%ums 强制交易=%d 引擎帧率锁=%d fps=%u",
                          dropInCombat ? 1 : 0, auctionTownBypass ? 1 : 0, restMpAccel ? 1 : 0,
                          (uint32_t)restMpAccelIntervalMs, forceTrade ? 1 : 0, frameLock ? 1 : 0,
                          (uint32_t)frameLockFps);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] drop/auction/restMp/forceTrade/frameLock 失败");
        }
    };

    auto persistAttackRpc = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.attackRpc = gUiAttackRpc ? 1u : 0u;
        c.attackRpcMobs = xcat::ClampAttackRpcMobs(
            static_cast<uint32_t>(gUiAttackRpcMobs < 0 ? 0 : gUiAttackRpcMobs));
        c.attackRpcIntervalMs = xcat::ClampAttackRpcIntervalMs(
            static_cast<uint32_t>(gUiAttackRpcIntervalMs < 0 ? 0 : gUiAttackRpcIntervalMs));
        c.attackRpcDamage = xcat::ClampAttackRpcDamage(
            static_cast<uint32_t>(gUiAttackRpcDamage < 0 ? 0 : gUiAttackRpcDamage));
        gUiAttackRpcMobs = (int)c.attackRpcMobs;
        gUiAttackRpcIntervalMs = (int)c.attackRpcIntervalMs;
        gUiAttackRpcDamage = (int)c.attackRpcDamage;
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControl verify{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                dropSeenTick = verify.writeTickMs;
            } else {
                dropSeenTick = c.writeTickMs;
            }
            xcat::log::Ok("App", "已下发 core：attackRpc=%d mobs=%u ms=%u dmg=%u（实验）",
                          gUiAttackRpc ? 1 : 0, c.attackRpcMobs, c.attackRpcIntervalMs,
                          c.attackRpcDamage);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] attackRpc 失败");
        }
    };

    auto persistForgeHit = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.simpleCombatForgeHit = gUiCombatForgeHit ? 1u : 0u;
        c.simpleCombatForgeHitFrontDx = xcat::ClampForgeHitFrontDx(
            static_cast<uint32_t>(gUiForgeHitFrontDx < 0 ? 0 : gUiForgeHitFrontDx));
        c.simpleCombatForgeHitFrontDy = xcat::ClampForgeHitFrontDy(
            static_cast<uint32_t>(gUiForgeHitFrontDy < 0 ? 0 : gUiForgeHitFrontDy));
        c.simpleCombatForgeHitMobs = xcat::kForgeHitMobsDefault;
        c.simpleCombatForgeHitFillList = 0;
        c.simpleCombatForgeHitMultiPkt = 0;
        gUiForgeHitFrontDx = (int)c.simpleCombatForgeHitFrontDx;
        gUiForgeHitFrontDy = (int)c.simpleCombatForgeHitFrontDy;
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControl verify{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                dropSeenTick = verify.writeTickMs;
            } else {
                dropSeenTick = c.writeTickMs;
            }
            xcat::log::Ok("App", "已下发 core：forgeHit=%d box=%u×%u（实验）",
                          gUiCombatForgeHit ? 1 : 0, c.simpleCombatForgeHitFrontDx,
                          c.simpleCombatForgeHitFrontDy);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] simpleCombatForgeHit 失败");
        }
    };

    auto persistMapAttack = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.mapAttack = gUiMapAttack ? 1u : 0u;
        if (gUiMapAttack) {
            gUiCombatForgeHit = false;
            gUiAttackRpc = false;
            c.simpleCombatForgeHit = 0;
            c.attackRpc = 0;
        }
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControl verify{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                dropSeenTick = verify.writeTickMs;
            } else {
                dropSeenTick = c.writeTickMs;
            }
            xcat::log::Ok("App", "已下发 core：mapAttack=%d（实验 P2 扩盒）",
                          gUiMapAttack ? 1 : 0);
        } else {
            xcat::log::Warn("App", "写入 mapAttack 会话态失败");
        }
    };

    {
        xcat::ui::CardGuard card("##tab_beta_forge_hit", "出刀自组攻包");
        if (xcat::ui::OptionCheckbox("出刀自组攻包（钉锁）", &gUiCombatForgeHit)) persistForgeHit();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("近战 50 / A槽魔法 52；过远看下方攻击盒 · 失败不出刀 · 落盘");
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项。落盘 user.ini。简单战斗普攻才自己组包。\n"
                "发包走 Network.SendOutPacket（禁止 Session.Send 旁路）。\n"
                "默认：命中环只填当前锁 oid。近战 50；A 槽魔法攻击技 52。\n"
                "过远（本卡攻击盒）/ SendOut 失败 / 射击 51：这一刀作废，不退 OnFuncKey。\n"
                "\n"
                "和「打怪实验」里的「攻包伪造探针」不是同一个勾：\n"
                "· 探针 = 自己 Tick 扫近距怪，满 2 次自动关\n"
                "· 本项 = 劫持打怪出刀，节奏跟面板间隔\n"
                "\n"
                "攻击盒与探针「多怪数」、首页「站桩输出」横向/竖直各存各的。");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("X");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##forge_hit_front_dx", &gUiForgeHitFrontDx, 10,
                           (int)xcat::kForgeHitFrontDxMin, (int)xcat::kForgeHitFrontDxMax)) {
            gUiForgeHitFrontDx = (int)xcat::ClampForgeHitFrontDx(
                static_cast<uint32_t>(gUiForgeHitFrontDx < 0 ? 0 : gUiForgeHitFrontDx));
            persistForgeHit();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "人↔怪 AbsPos 横向半宽（px）。默认 %u。0=该轴不限。\n"
                "只挡自组攻包钉锁，不影响站桩输出面前盒。",
                (unsigned)xcat::kForgeHitFrontDxDefault);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("px");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Y");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##forge_hit_front_dy", &gUiForgeHitFrontDy, 5,
                           (int)xcat::kForgeHitFrontDyMin, (int)xcat::kForgeHitFrontDyMax)) {
            gUiForgeHitFrontDy = (int)xcat::ClampForgeHitFrontDy(
                static_cast<uint32_t>(gUiForgeHitFrontDy < 0 ? 0 : gUiForgeHitFrontDy));
            persistForgeHit();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "人↔怪 AbsPos 竖直半高（px）。默认 %u。0=该轴不限。\n"
                "AbsPos：更大 Y = 更高。只挡自组攻包钉锁。",
                (unsigned)xcat::kForgeHitFrontDyDefault);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("px");
    }

    CardGap();
    {
    xcat::ui::CardGuard card("##tab_beta", "实验功能");
        if (xcat::ui::OptionCheckbox("战斗中可丢物", &dropInCombat)) persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "改 IsAlertMode 比较阈值（装一次）：战斗中可丢物，并抑制客户端警戒\n"
                "（出刀刷戳无空窗；打怪后警戒立不住属预期）。仅客户端；服务端 Drop 权威不变。默认开。");
        }
        if (xcat::ui::OptionCheckbox("野外可开拍卖（仅客户端）", &auctionTownBypass))
            persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
            "静默绕过：开启后点游戏状态栏拍卖按钮，野外也会发迁移包(0x002E)\n"
            "机制：零.text；换图快写一次 IsTown=1/清 Option&0x10，稳住后约1s校验\n"
            "（已正确则不写）。不能「仅点击才写」：状态栏直调，无点击回调。\n"
            "仅客户端门控。服端常拒/断线(含 GlobalMarketTerminated)；\n"
            "若开着「守护模式」会把断线当踢线→5秒干净重拉（像被杀死）。\n"
            "挂机/守护期间建议关。默认开。\n"
            "开启期间其它读 IsTown/该 Option 位的逻辑也会受影响。");
        }
        if (!xcat::kAuctionGateProbeUserEnabled) ImGui::BeginDisabled();
        if (ImGui::Button("拍卖原生按钮（一次）") && xcat::kAuctionGateProbeUserEnabled) {
            if (ui.prefsBinDir.empty()) {
                xcat::log::Warn("App", "拍卖探针：prefsBinDir 空");
            } else {
                xcat::PayloadControl c{};
                (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
                c.auctionGateProbeSeq = c.auctionGateProbeSeq + 1u;
                if (c.auctionGateProbeSeq == 0) c.auctionGateProbeSeq = 1;
                c.writeTickMs = GetTickCount64();
                if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                    xcat::PayloadControl verify{};
                    if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify))
                        dropSeenTick = verify.writeTickMs;
                    else
                        dropSeenTick = c.writeTickMs;
                    xcat::log::Ok("App",
                                  "已下发拍卖原生按钮探针 seq=%u（OnClickButton(17)，不改等级）",
                                  verify.auctionGateProbeSeq ? verify.auctionGateProbeSeq
                                                             : c.auctionGateProbeSeq);
                } else {
                    xcat::log::Warn("App", "写入 auctionGateProbeSeq 失败");
                }
            }
        }
        if (!xcat::kAuctionGateProbeUserEnabled) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("当前暂不可用");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "已停用：15/24h 探针实测无效，不再点状态栏拍卖、不改等级/建角。\n"
                    "野外开拍卖仍走上方「野外可开拍卖」。代码保留。");
            }
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "一次探针：进图后点。主泵上 FindAll 状态栏，调官方 OnClickButton(17)，\n"
                "与手点拍卖同一条链（含红号检查→迁拍卖 0x002E）。不改等级/建角时间。\n"
                "15/24h 仍走客户端官方闸；服端权威不变。日志 AuctionGateProbe。");
        }
        if (xcat::ui::OptionCheckbox("回蓝加速（实验）", &restMpAccel)) persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "按间隔写满 WorldManager 回蓝累加器（+0x17C 休息 / +0x180 椅子）。\n"
                "BIN 已证真蓝会动；过密会踢——用下方间隔自己调。\n"
                "不要求坐椅（站着也会催）。日志：XCat_data/logs/rest_mp_accel.log\n"
                "默认关·间隔默认 2500ms（曾 16ms 狂刷会秒踢）。");
        }
        ImGui::BeginDisabled(!restMpAccel);
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::SliderInt("回蓝间隔(ms)##rest_mp_iv", &restMpAccelIntervalMs,
                             (int)xcat::kRestMpAccelIntervalMinMs,
                             (int)xcat::kRestMpAccelIntervalMaxMs)) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "两次催回蓝的最小间隔。越小越快、踢风险越高。\n"
                "范围 %u~%u，默认 %u。建议从 1000+ 往下拧。",
                xcat::kRestMpAccelIntervalMinMs, xcat::kRestMpAccelIntervalMaxMs,
                xcat::kRestMpAccelIntervalDefaultMs);
        }
        ImGui::EndDisabled();
        if (!xcat::kForceTradeUserEnabled) {
            forceTrade = false;
            ImGui::BeginDisabled();
        }
        if (xcat::ui::OptionCheckbox("强制交易（实验）", &forceTrade)) persistDrop();
        if (!xcat::kForceTradeUserEnabled) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("当前暂不可用");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "已停用：改人物卡 15 级交易门实测无效（服端仍拒），不再改阈值。\n"
                    "代码保留；需要时把 kForceTradeUserEnabled 改回 true。");
            }
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "改 UIUserInfo 人物卡「交易」按钮的等级比较阈值（装一次）：\n"
                "15 级以下也能点交易。仅客户端 UI；服务端仍可能拒包。\n"
                "不覆盖右键菜单 / 丢物。关即还原官方 15 级门。默认关。");
        }
        ImGui::Separator();
        if (xcat::ui::OptionCheckbox("引擎帧率锁", &frameLock)) persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "锁 Unity 主循环目标帧率（Application.targetFrameRate），并关闭引擎 vSync。\n"
                "不修改显示器硬件刷新率。用于高低配显示器对齐打怪节奏。\n"
                "预设两行：120/240/360/480 与 640/720/860/1000；也可自定义（%u~%u）。默认开·1000。\n"
                "关闭时还原引擎 vSync=1（游戏无公开 getter，按经典版常见默认）。",
                xcat::kFrameLockFpsMin, xcat::kFrameLockFpsMax);
        }
        ImGui::BeginDisabled(!frameLock);
        {
            auto presetBtn = [&](int fps) {
                char lab[32];
                std::snprintf(lab, sizeof(lab), "%d##fl_pre_%d", fps, fps);
                const bool sel = frameLockFps == fps;
                if (sel)
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                         ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(lab, ImVec2(52.f, 0))) {
                    frameLockFps = fps;
                    persistDrop();
                }
                if (sel) ImGui::PopStyleColor();
            };
            const float labelW =
                ImGui::CalcTextSize("预设").x + ImGui::GetStyle().ItemSpacing.x;
            ImGui::TextUnformatted("预设");
            ImGui::SameLine();
            presetBtn(120);
            ImGui::SameLine();
            presetBtn(240);
            ImGui::SameLine();
            presetBtn(360);
            ImGui::SameLine();
            presetBtn(480);
            // 第二行：与首行按钮左对齐
            ImGui::Dummy(ImVec2(labelW, 0.f));
            ImGui::SameLine(0.f, 0.f);
            presetBtn(640);
            ImGui::SameLine();
            presetBtn(720);
            ImGui::SameLine();
            presetBtn(860);
            ImGui::SameLine();
            presetBtn(1000);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("点选即下发；开启态约 200ms 内由载荷重刷。");
            }
        }
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::DragInt("自定义##frameLockFps", &frameLockFps, 1.f,
                           (int)xcat::kFrameLockFpsMin, (int)xcat::kFrameLockFpsMax, "%d fps")) {
            frameLockFps = (int)xcat::ClampFrameLockFps(
                static_cast<uint32_t>(frameLockFps < 0 ? 0 : frameLockFps));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistDrop();
        ImGui::EndDisabled();
        {
            int rb = -1;
            bool rbFresh = false;
            if (!ui.prefsBinDir.empty()) {
                xcat::PayloadStatus st{};
                if (xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) &&
                    xcat::PayloadStatusHeartbeatFresh(st, GetTickCount64(), 5000)) {
                    rb = st.frameLockReadback;
                    rbFresh = true;
                }
            }
            if (!rbFresh) {
                ImGui::TextDisabled("引擎读回：等待注入心跳…（非显示器 Hz）");
            } else if (rb < 0) {
                ImGui::TextDisabled("引擎读回：尚未采到（目标 %d）", frameLockFps);
            } else {
                const int drift = rb > frameLockFps ? (rb - frameLockFps) : (frameLockFps - rb);
                if (frameLock && drift > 1) {
                    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f),
                                       "引擎读回：%d（目标 %d，偏差较大）", rb, frameLockFps);
                } else {
                    ImGui::TextDisabled("引擎读回：%d（目标 %d）", rb, frameLockFps);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "读回 = Application.get_targetFrameRate（SHM PayloadStatus v7）。\n"
                    "与目标差 >1 时常见于驱动/OS 覆盖；改的不是显示器硬件刷新率。");
            }
        }
        ImGui::BeginDisabled();
        skipDialog = false;
    xcat::ui::OptionCheckbox("快速跳过对话", &skipDialog);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("当前未开发，暂不可用。");
        }
        ImGui::EndDisabled();
        (void)autoAccept;
        (void)autoFirst;
    }

    CardGap();
    {
        // 自动召唤宠物：从首页迁入实验；置灰强制关（暂不可用）。
        xcat::ui::CardGuard card("##tab_beta_pet_summon", "自动召唤宠物");
        if (!xcat::kPetSummonUserEnabled) {
            gUiPetSummon = false;
            gUiPetSummonRequireFood = false;
        }
        ImGui::BeginDisabled();
        xcat::ui::OptionCheckbox("自动召唤宠物", &gUiPetSummon);
        ImGui::SameLine();
        xcat::ui::OptionCheckbox("有粮才召", &gUiPetSummonRequireFood);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("当前暂不可用");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "原首页入口，已迁到本卡并暂时关闭。\n"
                "勾选不可用；落盘强制关，避免旧 user.ini 仍自动召宠。");
        }
        ImGui::TextDisabled("喂食交游戏(≈50)");
    }

    CardGap();
    {
        // 跳过攻击动画：从吸怪 TAB 迁入实验；不绑吸怪解锁。
        xcat::ui::CardGuard card("##tab_beta_skip_prepare", "跳过攻击动画");
        static bool skipPrepLoaded = false;
        static uint64_t skipPrepSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!skipPrepLoaded || disk.writeTickMs != skipPrepSeen) {
                    gUiAttackAccelSkipPrepare = disk.attackAccelSkipPrepare != 0;
                    skipPrepSeen = disk.writeTickMs;
                    skipPrepLoaded = true;
                }
            } else if (!skipPrepLoaded) {
                skipPrepLoaded = true;
            }
        } else if (!skipPrepLoaded) {
            skipPrepLoaded = true;
        }

        auto persistSkipPrep = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.attackAccelSkipPrepare = gUiAttackAccelSkipPrepare ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                skipPrepSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：attackAccelSkipPrepare=%d（实验）",
                              gUiAttackAccelSkipPrepare ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] attackAccelSkipPrepare 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("启用", &gUiAttackAccelSkipPrepare)) persistSkipPrep();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：出刀时不播攻击抬手/挥砍动作，减轻动画堆积。\n"
                "站立呼吸等待机动作仍保留；进图/换图落地约 1 秒内暂不生效。\n"
                "从关→开也会重新等落地。不影响出刀间隔。\n"
                "与下方「砍动画倒计时」不要同时开（开本项则不砍倒计时）。\n"
                "可能皮错/偶发卡刀，出问题先关掉。");
        }
        ImGui::TextDisabled("默认关；与「砍动画倒计时」勿同时开");
    }

    CardGap();
    {
        // 砍动画倒计时：从吸怪 TAB 迁入实验；默认关；不绑吸怪解锁。
        xcat::ui::CardGuard card("##tab_beta_cut_layer", "砍动画倒计时");
        static bool cutLayerLoaded = false;
        static uint64_t cutLayerSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!cutLayerLoaded || disk.writeTickMs != cutLayerSeen) {
                    gUiAttackAccelCutLayer = disk.attackAccelCutLayer != 0;
                    cutLayerSeen = disk.writeTickMs;
                    cutLayerLoaded = true;
                }
            } else if (!cutLayerLoaded) {
                cutLayerLoaded = true;
            }
        } else if (!cutLayerLoaded) {
            cutLayerLoaded = true;
        }

        auto persistCutLayer = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.attackAccelCutLayer = gUiAttackAccelCutLayer ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                cutLayerSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：attackAccelCutLayer=%d（实验）",
                              gUiAttackAccelCutLayer ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] attackAccelCutLayer 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("启用", &gUiAttackAccelCutLayer)) persistCutLayer();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：周期把动作层 layer+0x14 倒计时置 0，\n"
                "逼动画帧尽快推进，减轻连挥堆叠（偏视觉）。\n"
                "不改「攻击无CD」/攻速逻辑。\n"
                "可能空砍/皮抽；可单独开。\n"
                "会连带催快待机呼吸——更想少抬手请用上方「跳过攻击动画」。\n"
                "与「跳过攻击动画」互斥（开跳过则不砍层）。");
        }
        ImGui::TextDisabled("默认关；与「跳过攻击动画」勿同时开");
    }

    CardGap();
    {
        // 近战不挥拳：从首页迁入；普攻分支实验项。
        xcat::ui::CardGuard card("##tab_beta_melee_veto", "近战不挥拳");
        static bool meleeVetoLoaded = false;
        static uint64_t meleeVetoSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!meleeVetoLoaded || disk.writeTickMs != meleeVetoSeen) {
                    gUiMeleeVeto = disk.meleeVeto != 0;
                    meleeVetoSeen = disk.writeTickMs;
                    meleeVetoLoaded = true;
                }
            } else if (!meleeVetoLoaded) {
                meleeVetoLoaded = true;
            }
        } else if (!meleeVetoLoaded) {
            meleeVetoLoaded = true;
        }

        auto persistMeleeVeto = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.meleeVeto = gUiMeleeVeto ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                meleeVetoSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：meleeVeto=%d（实验）", gUiMeleeVeto ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] meleeVeto 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("启用", &gUiMeleeVeto)) persistMeleeVeto();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "把贴脸时那一发挥拳**取消掉**（实测是取消，不是改成远程那一发）。\n"
                "只动普攻，技能攻击完全不碰 —— 靠技能键输出的循环不受影响。\n"
                "\n"
                "**只拦投掷飞镖**（武器类型 47），勾上即刻生效、不需要观测。\n"
                "拿别的武器勾了普攻照旧，只会在日志里记一笔观测数据。弓尤其不能拦：\n"
                "弓的普攻伤害本来就在近战分支里产生，拦了会变成「只飘伤害数字、\n"
                "怪不掉血」，所以干脆不让它进这条路。\n"
                "\n"
                "判决只保证伤害源不在近战分支，服务端认不认还得看 combat.log 里\n"
                "怪物血量有没有真的掉。发现只飘数字不掉血，立刻取消勾选。");
        }
        ImGui::TextDisabled("原首页入口；仅飞镖（武器类型 47）生效");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_booster", "攻速槽 nBooster_");
        static bool boosterLoaded = false;
        static uint64_t boosterSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!boosterLoaded || disk.writeTickMs != boosterSeen) {
                    gUiAttackAccelBooster =
                    xcat::kAttackAccelBoosterUserEnabled && disk.attackAccelBooster != 0;
                    boosterSeen = disk.writeTickMs;
                    boosterLoaded = true;
                }
            } else if (!boosterLoaded) {
                boosterLoaded = true;
            }
        } else if (!boosterLoaded) {
            boosterLoaded = true;
        }

        auto persistBooster = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.attackAccelBooster =
            (xcat::kAttackAccelBoosterUserEnabled && gUiAttackAccelBooster) ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                boosterSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：attackAccelBooster=%d（实验）",
                              gUiAttackAccelBooster ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] attackAccelBooster 失败");
            }
        };

        if (!xcat::kAttackAccelBoosterUserEnabled) {
            gUiAttackAccelBooster = false;
            ImGui::BeginDisabled();
        }
        if (xcat::ui::OptionCheckbox("攻速槽 nBooster_", &gUiAttackAccelBooster))
            persistBooster();
        if (!xcat::kAttackAccelBoosterUserEnabled) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("当前暂不可用");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "已停用：写 nBooster_=-8 超出合法值域，存在指纹风险。\n"
                    "代码保留；需要时把 kAttackAccelBoosterUserEnabled 改回 true。");
            }
        } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：写 SecondaryStat.nBooster_=-8，把攻速 degree 夹到最快的 2，\n"
                "攻击延迟 ×0.75；到期时间按游戏钟每拍续 60s，关勾选时原值奉还。\n"
                "与「吸怪 快攻」TAB「攻击无CD」完全独立，就是为了能分别开来做对照：\n"
                "实测「攻击无CD」开着时本项净收益为 0 —— 忙锁一清，引擎那道延迟闸就没了。\n"
                "它真正的用法是**替掉**「攻击无CD」：只开本项，不碰动作忙锁，约慢 5ms 但更干净。\n"
                "注意 -8 超出合法 booster 值域（正常只有 -1/-2），存在被识别的风险。");
        }
        ImGui::TextDisabled(
            xcat::kAttackAccelBoosterUserEnabled
                ? "对照用：与「攻击无CD」分开开关，可单独开"
                : "已禁用（不写 nBooster_）· 代码保留");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_action_speed", "A系 nSpeed_");
        static bool actSpLoaded = false;
        static uint64_t actSpSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!actSpLoaded || disk.writeTickMs != actSpSeen) {
                    gUiAttackAccelActionSpeed = disk.attackAccelActionSpeed != 0;
                    actSpSeen = disk.writeTickMs;
                    actSpLoaded = true;
                }
            } else if (!actSpLoaded) {
                actSpLoaded = true;
            }
        } else if (!actSpLoaded) {
            actSpLoaded = true;
        }

        auto persistActSp = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.attackAccelActionSpeed = gUiAttackAccelActionSpeed ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                actSpSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：attackAccelActionSpeed=%d（实验）",
                              gUiAttackAccelActionSpeed ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] attackAccelActionSpeed 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("A系 nSpeed_ (+40)", &gUiAttackAccelActionSpeed))
            persistActSp();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：写 SecondaryStat.nSpeed_@0x84=+40，\n"
                "GetActionSpeed = nSpeed(+80) + Max(40,Dash) → Prepare clamp [70,140]。\n"
                "只加速抬手/动画，不改 deg（伤害节奏）。\n"
                "若鞋/Forced 已把 +80 顶到近 140，写成功也无体感——看日志 aSp/n80；\n"
                "要「攻击速度快乐」请用 PartyBooster / nBooster_（B 系）。");
        }
        ImGui::TextDisabled("动画 Prepare；顶格则无体感（用 Party）");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_party_booster", "PartyBooster");
        static bool partyLoaded = false;
        static uint64_t partySeen = 0;
        static bool editingPartyV = false;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                // 滑条拖动中禁止回读盘，否则其它卡片 bump writeTick 会把 Value 冲回旧值。
                if (!partyLoaded ||
                    (!editingPartyV && disk.writeTickMs != partySeen)) {
                    gUiAttackAccelPartyBooster = disk.attackAccelPartyBooster != 0;
                    gUiAttackAccelPartyBoosterValue = (int)xcat::ClampAttackAccelPartyBoosterValue(
                        disk.attackAccelPartyBoosterValue);
                    partySeen = disk.writeTickMs;
                    partyLoaded = true;
                }
            } else if (!partyLoaded) {
                partyLoaded = true;
            }
        } else if (!partyLoaded) {
            partyLoaded = true;
        }

        auto persistParty = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.attackAccelPartyBooster = gUiAttackAccelPartyBooster ? 1u : 0u;
            c.attackAccelPartyBoosterValue =
                xcat::ClampAttackAccelPartyBoosterValue(gUiAttackAccelPartyBoosterValue);
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                // WritePayloadControl 可能把 writeTickMs 单调 +1；必须回读，否则
                // partySeen 落后 → 下帧把滑条 Value 从盘冲回旧值。
                xcat::PayloadControl verify{};
                if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                    partySeen = verify.writeTickMs;
                    dropSeenTick = verify.writeTickMs;
                    gUiAttackAccelPartyBooster = verify.attackAccelPartyBooster != 0;
                    gUiAttackAccelPartyBoosterValue =
                        (int)xcat::ClampAttackAccelPartyBoosterValue(
                            verify.attackAccelPartyBoosterValue);
                } else {
                    partySeen = c.writeTickMs;
                    dropSeenTick = c.writeTickMs;
                }
                xcat::log::Ok("App", "已下发 core：attackAccelPartyBooster=%d value=%d（实验）",
                              gUiAttackAccelPartyBooster ? 1 : 0, gUiAttackAccelPartyBoosterValue);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] attackAccelPartyBooster 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("PartyBooster", &gUiAttackAccelPartyBooster)) persistParty();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：写 SecondaryStat.TempStats[4].Value\n"
                "（PartyBooster），加入 GetAttackSpeedDegree 公式，\n"
                "与 nBooster_ 同属 B 系伤害延迟，可叠加；不解忙锁。\n"
                "TempStats 槽缺失时只打日志不写。关勾选时原值奉还。\n"
                "BIN 对照：party=1/<Value> deg=…；Value=0 → deg≈武器档，-8 → deg=2。");
        }

        ImGui::BeginDisabled(!gUiAttackAccelPartyBooster);
        ImGui::SetNextItemWidth(AppDpi_Px(-1.f));
        if (ImGui::SliderInt("##party_boost_v", &gUiAttackAccelPartyBoosterValue,
                             (int)xcat::kAttackAccelPartyBoosterValueMin,
                             (int)xcat::kAttackAccelPartyBoosterValueMax, "Value = %d")) {
            gUiAttackAccelPartyBoosterValue =
                (int)xcat::ClampAttackAccelPartyBoosterValue(gUiAttackAccelPartyBoosterValue);
            persistParty();
        }
        editingPartyV = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            editingPartyV = false;
            persistParty();
        }

        auto bumpPartyV = [&](int v) {
            gUiAttackAccelPartyBoosterValue =
                (int)xcat::ClampAttackAccelPartyBoosterValue(v);
            persistParty();
        };
        if (ImGui::Button("-8##party_v_m8")) bumpPartyV(-8);
        ImGui::SameLine();
        if (ImGui::Button("-4##party_v_m4")) bumpPartyV(-4);
        ImGui::SameLine();
        if (ImGui::Button("-2##party_v_m2")) bumpPartyV(-2);
        ImGui::SameLine();
        if (ImGui::Button("-1##party_v_m1")) bumpPartyV(-1);
        ImGui::SameLine();
        if (ImGui::Button("0##party_v_0")) bumpPartyV(0);
        ImGui::EndDisabled();

        ImGui::TextDisabled("越负越快；BIN 看 party=1/<v> 与 deg（0→慢，-8→deg2）");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_break_degree_floor", "破 degree 下限");
        static bool brkLoaded = false;
        static uint64_t brkSeen = 0;
        static bool editingBrkLo = false;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!brkLoaded || (!editingBrkLo && disk.writeTickMs != brkSeen)) {
                    gUiAttackAccelBreakDegreeFloor = disk.attackAccelBreakDegreeFloor != 0;
                    gUiAttackAccelBreakDegreeFloorLo =
                        (int)xcat::ClampAttackAccelBreakDegreeFloorLo(
                            disk.attackAccelBreakDegreeFloorLo);
                    brkSeen = disk.writeTickMs;
                    brkLoaded = true;
                }
            } else if (!brkLoaded) {
                brkLoaded = true;
            }
        } else if (!brkLoaded) {
            brkLoaded = true;
        }

        auto persistBrk = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.attackAccelBreakDegreeFloor = gUiAttackAccelBreakDegreeFloor ? 1u : 0u;
            c.attackAccelBreakDegreeFloorLo =
                xcat::ClampAttackAccelBreakDegreeFloorLo(gUiAttackAccelBreakDegreeFloorLo);
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                xcat::PayloadControl verify{};
                if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify)) {
                    brkSeen = verify.writeTickMs;
                    dropSeenTick = verify.writeTickMs;
                    gUiAttackAccelBreakDegreeFloor = verify.attackAccelBreakDegreeFloor != 0;
                    gUiAttackAccelBreakDegreeFloorLo =
                        (int)xcat::ClampAttackAccelBreakDegreeFloorLo(
                            verify.attackAccelBreakDegreeFloorLo);
                } else {
                    brkSeen = c.writeTickMs;
                    dropSeenTick = c.writeTickMs;
                }
                xcat::log::Ok("App", "已下发 core：attackAccelBreakDegreeFloor=%d lo=%d（实验）",
                              gUiAttackAccelBreakDegreeFloor ? 1 : 0,
                              gUiAttackAccelBreakDegreeFloorLo);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] attackAccelBreakDegreeFloor 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("破 degree 下限", &gUiAttackAccelBreakDegreeFloor))
            persistBrk();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：写 CalcWeaponAttackSpeedTier 独占 GA 种子，\n"
                "把 B 系 degree clamp 下限改成下方 lo（数据面，禁 E9）。\n"
                "与 PartyBooster 完全独立：不开本项时 PB=-8 仍顶格 deg=2。\n"
                "lo 越负越快；叠 PB 时 deg 可落到 lo；lo=-10 → 延迟可×0。\n"
                "关勾选种子原值奉还。");
        }

        ImGui::BeginDisabled(!gUiAttackAccelBreakDegreeFloor);
        ImGui::SetNextItemWidth(AppDpi_Px(-1.f));
        if (ImGui::SliderInt("##break_deg_lo", &gUiAttackAccelBreakDegreeFloorLo,
                             (int)xcat::kAttackAccelBreakDegreeFloorLoMin,
                             (int)xcat::kAttackAccelBreakDegreeFloorLoMax, "lo = %d")) {
            gUiAttackAccelBreakDegreeFloorLo =
                (int)xcat::ClampAttackAccelBreakDegreeFloorLo(gUiAttackAccelBreakDegreeFloorLo);
            persistBrk();
        }
        editingBrkLo = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            editingBrkLo = false;
            persistBrk();
        }

        auto bumpBrkLo = [&](int v) {
            gUiAttackAccelBreakDegreeFloorLo =
                (int)xcat::ClampAttackAccelBreakDegreeFloorLo(v);
            persistBrk();
        };
        if (ImGui::Button("-10##brk_lo_m10")) bumpBrkLo(-10);
        ImGui::SameLine();
        if (ImGui::Button("-8##brk_lo_m8")) bumpBrkLo(-8);
        ImGui::SameLine();
        if (ImGui::Button("-4##brk_lo_m4")) bumpBrkLo(-4);
        ImGui::SameLine();
        if (ImGui::Button("-2##brk_lo_m2")) bumpBrkLo(-2);
        ImGui::SameLine();
        if (ImGui::Button("0##brk_lo_0")) bumpBrkLo(0);
        ImGui::EndDisabled();

        ImGui::TextDisabled("越负越快；BIN 看 brkFloor=1 与 deg≥lo");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_final_attack", "终极一击");
        static bool faForceLoaded = false;
        static uint64_t faForceSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!faForceLoaded || disk.writeTickMs != faForceSeen) {
                    gUiFinalAttackForce =
                        xcat::kFinalAttackForceUserEnabled && disk.finalAttackForce != 0;
                    faForceSeen = disk.writeTickMs;
                    faForceLoaded = true;
                }
            } else if (!faForceLoaded) {
                faForceLoaded = true;
            }
        } else if (!faForceLoaded) {
            faForceLoaded = true;
        }

        auto persistFaForce = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.finalAttackForce =
                (xcat::kFinalAttackForceUserEnabled && gUiFinalAttackForce) ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                faForceSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：finalAttackForce=%d（实验）",
                              gUiFinalAttackForce ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] finalAttackForce 失败");
            }
        };

        if (!xcat::kFinalAttackForceUserEnabled) {
            gUiFinalAttackForce = false;
            ImGui::BeginDisabled();
        }
        if (xcat::ui::OptionCheckbox("普攻必出终极一击", &gUiFinalAttackForce))
            persistFaForce();
        if (!xcat::kFinalAttackForceUserEnabled) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("当前暂不可用");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "已弃用：不启 FaForce worker（曾 off-pump GetSkill 空 TLS）。\n"
                    "代码保留；需要时把 kFinalAttackForceUserEnabled 改回 true。");
            }
        } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：已学终极攻击（如狂战士終極之劍/斧）\n"
                "Prop→100，并强制注册 FinalAttack 出刀。\n"
                "数据面改 SkillLevelData / FinalAttack 结构体，不改 GameAssembly 代码。\n"
                "需已学习对应武器的终极技能；关掉后会尽量还原原 Prop。\n"
                "服端若校验伤害/技能，以服为准。");
        }
        ImGui::TextDisabled(xcat::kFinalAttackForceUserEnabled
                                ? "Prop100 + 强制注册出刀 · 日志 FaForce"
                                : "已禁用（不启 worker）· 代码保留");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_skill_max", "技能满级");
        static bool skillMaxLoaded = false;
        static uint64_t skillMaxSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!skillMaxLoaded || disk.writeTickMs != skillMaxSeen) {
                    gUiSkillMaxLevel =
                        xcat::kSkillMaxLevelUserEnabled && disk.skillMaxLevel != 0;
                    skillMaxSeen = disk.writeTickMs;
                    skillMaxLoaded = true;
                }
            } else if (!skillMaxLoaded) {
                skillMaxLoaded = true;
            }
        } else if (!skillMaxLoaded) {
            skillMaxLoaded = true;
        }

        auto persistSkillMax = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.skillMaxLevel =
                (xcat::kSkillMaxLevelUserEnabled && gUiSkillMaxLevel) ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                skillMaxSeen = c.writeTickMs;
                dropSeenTick = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：skillMaxLevel=%d（实验）",
                              gUiSkillMaxLevel ? 1 : 0);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] skillMaxLevel 失败");
            }
        };

        if (!xcat::kSkillMaxLevelUserEnabled) {
            gUiSkillMaxLevel = false;
            ImGui::BeginDisabled();
        }
        if (xcat::ui::OptionCheckbox("已学技能按满级生效", &gUiSkillMaxLevel))
            persistSkillMax();
        if (!xcat::kSkillMaxLevelUserEnabled) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("当前暂不可用");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "已停用：只改客户端等级，服端伤害仍按库里真实等级。\n"
                    "代码保留；需要时把 kSkillMaxLevelUserEnabled 改回 true。");
            }
        } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：A 写 SkillRecord/Ex 已学等级→满级；\n"
                "B Hook UserLocal + SkillInfo.GetSkillLevel/GetPure；\n"
                "C skill_port 施法/读级双保险。\n"
                "日志 SkillMax · hookUl/hookSi/hookPure。\n"
                "关掉还原字典原等级并卸钩；服端结算以服为准。");
        }
        ImGui::TextDisabled(
            xcat::kSkillMaxLevelUserEnabled
                ? "dict + UL/SI/Pure GetSkillLevel hook · src 见日志"
                : "已禁用（不启 worker）· 服端伤害不认客户端满级");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_combat_exp", "打怪实验");
        if (xcat::ui::OptionCheckbox("怪物刷新感知增强", &gUiMobPoolObserve)) {
            if (!ui.prefsBinDir.empty()) {
                xcat::PayloadControl c{};
                (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
                c.mobPoolObserve = gUiMobPoolObserve ? 1u : 0u;
                c.writeTickMs = GetTickCount64();
                if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                    dropSeenTick = c.writeTickMs;
                    xcat::log::Ok("App", "已下发 core：mobPoolObserve=%d（实验）",
                                  gUiMobPoolObserve ? 1 : 0);
                } else {
                    xcat::log::Warn("App", "写入 user.ini [core] mobPoolObserve 失败");
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("进/离场立刻读怪（默认关；1ms 热扫下无体感）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项：怪进场/离场时立刻唤醒读怪（MethodInfo 观察，不改 .text）。\n"
                "只吃掉「最多等一个读取周期」；读取速度已是 1ms 时开不开都一样。\n"
                "钩子尚未打穿进出场包，打开后写 mobpool_obs.log 供对照。");
        }

        if (xcat::ui::OptionCheckbox("LiveStep 跟位", &gUiCombatLiveStep)) {
            if (ui.prefsBinDir.empty()) {
                // no-op
            } else {
                xcat::PayloadControl c{};
                (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
                c.simpleCombatLiveStep = gUiCombatLiveStep ? 1u : 0u;
                c.writeTickMs = GetTickCount64();
                if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                    dropSeenTick = c.writeTickMs;  // 触发其它 TAB 跟盘
                    xcat::log::Ok("App", "已下发 core：LiveStep=%d（实验）",
                                  gUiCombatLiveStep ? 1 : 0);
                } else {
                    xcat::log::Warn("App", "写入 user.ini [core] simpleCombatLiveStep 失败");
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("锁怪后同层微瞬移跟位（默认关）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项：用瞬移技做跟位，可能加重客服坐标压力\n"
                "关：只在出带/换靶时贴怪（推荐）\n"
                "开：怪挪了也在同层用内置间隔跟位；hop<80 才走短收态");
        }

        if (xcat::ui::OptionCheckbox("地面门旁路", &gUiCurFhGateBypass)) {
            if (!ui.prefsBinDir.empty()) {
                xcat::PayloadControl c{};
                (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
                c.curFhGateBypass = gUiCurFhGateBypass ? 1u : 0u;
                c.writeTickMs = GetTickCount64();
                if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                    dropSeenTick = c.writeTickMs;
                    xcat::log::Ok("App", "已下发 core：curFhGateBypass=%d（实验）",
                                  gUiCurFhGateBypass ? 1 : 0);
                } else {
                    xcat::log::Warn("App", "写入 user.ini [core] curFhGateBypass 失败");
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("改引擎判空跳转（≠站立伪装，默认关）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项：直接改 GameAssembly 里 Magic/Shoot/Prepare 的 CurFh 判空分支，\n"
                "空台也走「有台」路径——不种台、不写 VecCtrl。\n"
                "\n"
                "和首页「站立伪装」怎么选：\n"
                "· 站立伪装 = 出刀瞬间种 CurFh（改内存字段）\n"
                "· 地面门旁路 = 改判定跳转（改 .text）\n"
                "两套独立，可单独开做 A/B；别当成同一个勾选。\n"
                "近战 Melee 看的是绳梯门，本旁路帮不上。\n"
                "改 .text 会留脏页，完整性校验可能扫到；建议短开对照 combat.log。");
        }

        {
            static bool sStopBoot = false;
            static uint32_t sLastStop = 0;
            if (!ui.prefsBinDir.empty()) {
                const uint32_t stop = xcat::ReadAttackRpcStopSeq(ui.prefsBinDir.c_str());
                if (!sStopBoot) {
                    sStopBoot = true;
                    sLastStop = stop;
                } else if (stop != 0 && stop > sLastStop) {
                    sLastStop = stop;
                    if (gUiAttackRpc) {
                        gUiAttackRpc = false;
                        persistAttackRpc();
                        notify::PushLocal(/*Info*/ 1, "attack-rpc", "探针已停",
                                         "2刀已发完，勾选已关。贴脸后再勾选可再打2刀", 4000);
                    }
                }
            }
        }

        if (xcat::ui::OptionCheckbox("攻包伪造探针", &gUiAttackRpc)) persistAttackRpc();
        ImGui::SameLine();
        ImGui::TextDisabled("近战 50 / A槽魔法 52；飞镖弓弩枪 51 不发（默认关）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项：伪造普通攻击出站包（事件），不是伪造伤害数字\n"
                "近战 opcode 50；A 槽魔法攻击技（魔力爪等）opcode 52。\n"
                "飞镖/弓/弩/枪（51）、杖普攻 NA、蓄力技不发。\n"
                "先 SetAttackAction+Collect，再 SendOutPacket\n"
                "成功标志：x.jsonl AttackRpc forge BODY op= / wt= / dist=\n"
                "单次勾选满 2 次 ok 自动关勾选（防延后踢）；间隔建议 ≥800ms\n"
                "只打贴脸 <=50px；更远会拒发（以前 180px 会 normal ok 但怪不死）\n"
                "进程累计 20 刀后需点「清零探针计数」或取消勾选等 2.5s 再勾\n"
                "也可设环境变量 ATTACK_RPC=1");
        }
        if (ImGui::Button("清零探针计数##arpc_reset", ImVec2(-1.f, 0.f))) {
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warning*/ 2, "attack-rpc", "下发失败", "无数据目录", 3000);
            } else {
                const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
                if (leds.gamePid == 0) {
                    notify::PushLocal(/*Warning*/ 2, "attack-rpc", "下发失败", "需已注入游戏",
                                     3000);
                } else {
                    uint32_t seq = xcat::ReadAttackRpcResetSeq(ui.prefsBinDir.c_str());
                    seq = seq == 0 ? 1u : seq + 1u;
                    if (seq == 0) seq = 1u;
                    if (xcat::WriteAttackRpcResetSeq(ui.prefsBinDir.c_str(), seq)) {
                        xcat::log::Ok("App", "attackRpcResetSeq=%u（清零 session cap，不写 user.ini）",
                                      seq);
                        notify::PushLocal(/*Info*/ 1, "attack-rpc", "已下发清零",
                                         "距上一刀<2.5s会排队，满闲置后自动清", 3500);
                    } else {
                        notify::PushLocal(/*Warning*/ 2, "attack-rpc", "下发失败",
                                         "写会话 seq 失败", 3000);
                    }
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "清零进程内伪造成功计数，不必重开游戏/重注 DLL。\n"
                "距上一刀不足 2.5s 会排队，满闲置后自动清（按钮不是坏了）。\n"
                "清零后：贴脸再勾选，又是 2 刀。累计 20 刀也会停，再点一次本按钮。\n"
                "连发仍可能延后踢，贴脸、隔开看掉血。");
        }
        if (ImGui::Button("发一刀伪造攻包##arpc_oneshot", ImVec2(-1.f, 0.f))) {
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warning*/ 2, "attack-rpc", "下发失败", "无数据目录", 3000);
            } else {
                const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
                if (leds.gamePid == 0) {
                    notify::PushLocal(/*Warning*/ 2, "attack-rpc", "下发失败", "需已注入游戏",
                                     3000);
                } else {
                    uint32_t seq = xcat::ReadAttackRpcFireSeq(ui.prefsBinDir.c_str());
                    seq = seq == 0 ? 1u : seq + 1u;
                    if (seq == 0) seq = 1u;
                    if (xcat::WriteAttackRpcFireSeq(ui.prefsBinDir.c_str(), seq)) {
                        xcat::log::Ok("App", "attackRpcFireSeq=%u（会话 oneshot，不写 user.ini）",
                                      seq);
                        notify::PushLocal(/*Info*/ 1, "attack-rpc", "已下发一刀",
                                         "贴脸<=50px；不改业务开关", 3500);
                    } else {
                        notify::PushLocal(/*Warning*/ 2, "attack-rpc", "下发失败",
                                         "写会话 seq 失败", 3000);
                    }
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "点一下发一包完整普攻（P0c：头+命中环+包尾角色 XY）。\n"
                "默认关：不勾探针、不写 user.ini、不改 F5。\n"
                "贴脸 <=50px 即可，不强制抢控（Passive 蜗牛也能发）。\n"
                "旁无 <=50px 的活怪会拒发 no_near_targets。\n"
                "SendOutPacket；不绕 HashSet。连点容易踢，隔开看掉血/掉落。");
        }
        if (gUiAttackRpc) {
        ImGui::Indent(ui::Gap() * 1.2f);
            if (ImGui::DragInt("##arpc_mobs", &gUiAttackRpcMobs, 1,
                               (int)xcat::kAttackRpcMobsMin, (int)xcat::kAttackRpcMobsMax)) {
                persistAttackRpc();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("多怪数");
            if (ImGui::DragInt("##arpc_ms", &gUiAttackRpcIntervalMs, 1,
                               (int)xcat::kAttackRpcIntervalMinMs,
                               (int)xcat::kAttackRpcIntervalMaxMs)) {
                persistAttackRpc();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("间隔 ms");
            if (ImGui::DragInt("##arpc_dmg", &gUiAttackRpcDamage, 1,
                               (int)xcat::kAttackRpcDamageMin, (int)xcat::kAttackRpcDamageMax)) {
                persistAttackRpc();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("线占位(非造伤)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("写入包内 Encode4 占位；正式服通常重算，改这个不会改实际伤害");
            }
        ImGui::Unindent(ui::Gap() * 1.2f);
        }

        if (xcat::ui::OptionCheckbox("全图攻击（P2 扩盒）", &gUiMapAttack))
            persistMapAttack();
        ImGui::SameLine();
        ImGui::TextDisabled("FindHit 盒=本图 AABB；一刀仍一只（默认关，不落盘）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项（默认关，不写入 user.ini；重启面板即关）：\n"
                "P2：FindHit 入参 Rect 换成当前图 foothold AABB。不抬 maxCount。\n"
                "近战/射击普攻官方 mc=1，一刀仍一只；测的是这只能不能离角色很远。\n"
                "看 x.jsonl tag=MapAtk：exp=1、xywh 变成上千、oid0 的 dx/dy。\n"
                "combat.log 看远处怪 hp 是否下降。踢号或不掉血 = 结案，不改去造包。\n"
                "\n"
                "mc=1 时 FindHit 可能仍先扫到身边那只：走开近怪再打，看 dx 是否变大。\n"
                "多目标仍要 A 槽绑 MobCount>=2 的技能（下一刀）。\n"
                "\n"
                "开此项时 DLL 会打下：打中换怪 / 出刀自组攻包 / 攻包伪造探针。\n"
                "F5 出刀仍是 OnFuncKey。");
        }
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_tdr", "TDR黑屏缓解");
        static tdr::Snapshot snap{};
        static bool snapLoaded = false;
        static std::string lastMsg;
        if (!snapLoaded) {
            snap = tdr::Read();
            snapLoaded = true;
        }

        if (snap.readable) {
            ImGui::Text("TdrDelay：%u 秒%s", snap.delaySec,
                        snap.delayPresent ? "" : "（系统默认）");
            ImGui::Text("TdrDdiDelay：%u 秒%s", snap.ddiSec,
                        snap.ddiPresent ? "" : "（系统默认）");
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f), "读取失败：%s",
                               snap.err.empty() ? "未知" : snap.err.c_str());
        }
        ImGui::TextDisabled(
            "缓解 GPU 超时误杀（DEVICE_REMOVED）；改完须重启虚拟机/本机才生效。不治根。");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "写 HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\n"
                "TdrDelay / TdrDdiDelay（推荐 8 秒；系统默认 Delay≈2 / Ddi≈5）。\n"
                "与攻击加速/游戏 core 无关；勿关 TDR（TdrLevel=0）。\n"
                "需管理员（XCat 已提权）。改完必须重启才生效。");
        }

        const float halfW = (ImGui::GetContentRegionAvail().x - ui::Gap()) * 0.5f;
        const float btnH = ui::BtnH();
        if (ImGui::Button("设为 8 秒##tdr_apply", ImVec2(halfW, btnH))) {
            std::string err;
            if (tdr::ApplyRecommended(tdr::kRecommendedDelaySec, &err)) {
                snap = tdr::Read();
                lastMsg = "已写入 8 秒——请重启虚拟机后再生效";
                xcat::log::Ok("App", "TDR：已写 TdrDelay/TdrDdiDelay=%u（须重启）",
                              tdr::kRecommendedDelaySec);
                notify::PushLocal(1, "tdr", "TDR 已写入",
                                  "请重启虚拟机后再生效（缓解显示超时误杀）");
            } else {
                lastMsg = "写入失败：" + (err.empty() ? std::string("未知") : err);
                xcat::log::Warn("App", "TDR 写入失败：%s", err.c_str());
                notify::PushLocal(2, "tdr", "TDR 写入失败", err.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("恢复默认##tdr_restore", ImVec2(halfW, btnH))) {
            std::string err;
            if (tdr::RestoreDefaults(&err)) {
                snap = tdr::Read();
                lastMsg = "已删自定义键——请重启虚拟机后恢复系统默认";
                xcat::log::Ok("App", "TDR：已删除 TdrDelay/TdrDdiDelay（须重启）");
                notify::PushLocal(1, "tdr", "TDR 已恢复默认", "请重启虚拟机后再生效");
            } else {
                lastMsg = "恢复失败：" + (err.empty() ? std::string("未知") : err);
                xcat::log::Warn("App", "TDR 恢复失败：%s", err.c_str());
                notify::PushLocal(2, "tdr", "TDR 恢复失败", err.c_str());
            }
        }
        if (ImGui::Button("刷新读数##tdr_refresh", ImVec2(-1.f, btnH))) {
            snap = tdr::Read();
            lastMsg.clear();
        }
        if (!lastMsg.empty()) {
            ImGui::TextWrapped("%s", lastMsg.c_str());
        }
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_classic", "专项（预留）");
    ImGui::TextDisabled("跨产品专属项不迁入本渠道");
    }
}

void DrawDebugTab(LaunchUiState& ui) {
    DesignBanner();
    {
        xcat::ui::CardGuard card("##tab_dbg_status", "运行状态");
        ImGui::TextUnformatted("产品：XCat");
        ImGui::Text("版本：%s (build %u)", xcat::kXcatVersionString, xcat::kXcatBuildId);
        ImGui::Text("启动模式：%s",
                    attach_inject::LaunchModeLabel(attach_inject::GetLaunchMode()));
        ImGui::Text("取票策略：%s", msc::weblogin::AuthStrategyLabel(msc::weblogin::GetAuthStrategy()));
        ImGui::Text("验证码UI：%s", msc::weblogin::CaptchaUiModeLabel(msc::weblogin::GetCaptchaUiMode()));
        ImGui::TextDisabled("换票：GAMA PASS CDP / HTTP Beanfun（无 WebView2）");
        ImGui::Text("换票会话：%s", msc::weblogin::IsBusy() ? "忙碌" : "空闲");
        ImGui::TextDisabled("顶栏 5 灯：IPC / GameContext / LocalPlayer / Map / Cache");
        ImGui::TextDisabled("注入后由 PayloadStatus SHM 点亮 LP/Map；Cache=测谎 TypeResolve");
        ImGui::TextDisabled("游戏 PID / 注入状态：见顶栏灯与状态条");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_cmd", "指令");
        static char cmdBuf[32]{};
        static const char* cmdMsg = "";
        static bool cmdOk = false;

        EnsureGatherUnlockLoaded();
        if (gGatherUnlockSaved) ImGui::TextDisabled("已保存");
        else if (gGatherTabUnlocked) ImGui::TextDisabled("已解锁（未保存）");

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("指令");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(160.f));
        const bool enter = ImGui::InputText("##dbg_ws_cmd", cmdBuf, sizeof(cmdBuf),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        const bool click = ImGui::Button("确定##dbg_ws_cmd");
        if (gGatherUnlockSaved) {
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            if (ImGui::Button("卸载##dbg_ws_cmd")) {
                if (ClearGatherUnlockReg()) {
                    gGatherTabUnlocked = false;
                    gGatherUnlockSaved = false;
                    gSyncedGatherUnlockWant = 0xFFFFFFFFu;
                    // 吸怪 TAB 附属能力一并关掉（跳过攻击动画 / 砍动画倒计时在实验 TAB，保留）。
                    gUiAttackAccelClearBusy = false;
                    SyncGatherUnlockToPayload(ui);
                    if (!ui.prefsBinDir.empty()) {
                        xcat::PayloadControl c{};
                        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c) &&
                            c.attackAccelClearBusy != 0) {
                            c.attackAccelClearBusy = 0;
                            c.gatherTabUnlocked = 0;
                            c.writeTickMs = GetTickCount64();
                            (void)xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c);
                        }
                    }
                    cmdOk = true;
                    cmdMsg = "已卸载";
                    cmdBuf[0] = '\0';
                    sound::UiClick();
                    xcat::log::Ok("App", "workspace command cleared");
                } else {
                    cmdOk = false;
                    cmdMsg = "卸载失败";
                    sound::UiError();
                }
            }
        }
        if (enter || click) {
            TrimCmdBuf(cmdBuf);
            if (cmdBuf[0] == '\0') {
                // 空提交不报错，避免点「确定」误伤已保存态。
            } else if (std::strcmp(cmdBuf, kGatherTabCmd) == 0) {
                gGatherTabUnlocked = true;
                cmdBuf[0] = '\0';
                if (WriteGatherUnlockReg()) {
                    gGatherUnlockSaved = true;
                    cmdOk = true;
                    cmdMsg = "";
                    sound::UiClick();
                    xcat::log::Ok("App", "workspace command ok");
                } else {
                    gGatherUnlockSaved = false;
                    cmdOk = false;
                    cmdMsg = "已解锁（保存失败）";
                    sound::UiError();
                    xcat::log::Warn("App", "workspace command persist failed");
                }
                gSyncedGatherUnlockWant = 0xFFFFFFFFu;
                SyncGatherUnlockToPayload(ui);
            } else {
                cmdOk = false;
                cmdMsg = "无效指令";
                cmdBuf[0] = '\0';
                sound::UiError();
            }
        }
        if (cmdMsg && cmdMsg[0]) {
            if (cmdOk) ImGui::TextDisabled("%s", cmdMsg);
            else ImGui::TextUnformatted(cmdMsg);
        }
    }
    CardGap();
    {
        // 卷軸掉落提示：从首页拾物迁到调试（日常挂机少碰；与测谎诊断同区）
        xcat::ui::CardGuard card("##tab_dbg_scroll_notify", "卷軸掉落提示");
        static bool scrollDbgLoaded = false;
        static uint64_t scrollDbgSeen = 0;
        static bool scrollDropNotify = true;
        static bool scrollSaveFailed = false;
        if (!ui.prefsBinDir.empty()) {
            xcat::PetLootConfig disk{};
            if (xcat::ReadPetLoot(ui.prefsBinDir.c_str(), disk)) {
                if (!scrollDbgLoaded || disk.writeTickMs != scrollDbgSeen) {
                    scrollDropNotify = disk.scrollDropNotify != 0;
                    scrollDbgSeen = disk.writeTickMs;
                    scrollDbgLoaded = true;
                }
            } else if (!scrollDbgLoaded) {
                scrollDbgLoaded = true;
            }
        } else if (!scrollDbgLoaded) {
            scrollDbgLoaded = true;
        }

        auto persistScrollNotify = [&]() {
            scrollSaveFailed = false;
            if (ui.prefsBinDir.empty()) return;
            xcat::PetLootConfig cfg{};
            (void)xcat::ReadPetLoot(ui.prefsBinDir.c_str(), cfg);
            cfg.scrollDropNotify = scrollDropNotify ? 1u : 0u;
            xcat::PetLootNormalize(cfg);
            cfg.writeTickMs = GetTickCount64();
            if (xcat::WritePetLoot(ui.prefsBinDir.c_str(), cfg)) {
                scrollDbgSeen = cfg.writeTickMs;
                xcat::log::Ok("App", "已下发 pet_loot：scrollDropNotify=%d（调试）",
                              cfg.scrollDropNotify ? 1 : 0);
            } else {
                scrollSaveFailed = true;
                xcat::log::Warn("App", "写入 user.ini [pet_loot] scrollDropNotify 失败");
            }
        };

        if (xcat::ui::OptionCheckbox("卷軸掉落提示音", &scrollDropNotify)) persistScrollNotify();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "可捡卷軸新落地时弹出通知，先叮咚再播报掉了什么（晓晓离线零件）。\n"
                "捡进包后（掉落从池消失）再播「拾取成功」；雷之鏢播「雷之镖拾取成功」。\n"
                "没听到就去现场看。\n"
                "雷之鏢（2070005）同样提醒，整句「掉落雷之鏢」。其它装备不提醒。\n"
                "不受「通知静音」影响（与测谎/限制警报同级）。\n"
                "拾物关闭时也可单独开启叮咚；默认开启。拾取成功只在拾物开着时播。\n"
                "写入 [pet_loot] scrollDropNotify。");
        }
        {
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float halfW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
            if (ImGui::Button("试播卷轴##dbg_scroll_voice", ImVec2(halfW, 0.f)))
                xcat::sound::PlayScrollDropAnnounce(2040001);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("2040001 头盔防御卷轴60%。走正式口播，不经过掉落池。");
            ImGui::SameLine();
            if (ImGui::Button("试播雷之鏢##dbg_thunder_dart", ImVec2(halfW, 0.f)))
                xcat::sound::PlayScrollDropAnnounce(2070005);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("2070005 整句「掉落雷之镖」。走正式口播，不经过掉落池。");
            if (ImGui::Button("试播拾取成功##dbg_pick_ok", ImVec2(halfW, 0.f)))
                xcat::sound::PlayPickupSuccessAnnounce(0);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("烘焙口播「拾取成功」。卷軸从池消失且拾物开着时自动播。");
            ImGui::SameLine();
            if (ImGui::Button("试播雷之鏢成功##dbg_pick_ok_dart", ImVec2(halfW, 0.f)))
                xcat::sound::PlayPickupSuccessAnnounce(2070005);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("烘焙口播「雷之镖拾取成功」。2070005 从池消失且拾物开着时自动播。");
        }
        if (scrollSaveFailed)
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [pet_loot] 失败");
        ImGui::TextDisabled("首页「拾物」改其它项不会冲掉本开关。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_soft_dismiss", "断线弹窗");
        ImGui::TextDisabled(
            "自动关窗失败时点一次。安全路径：CloseDialog + SetActive，不点「確認」。");
        if (ImGui::Button("关闭断线弹窗", ImVec2(-1.f, 0.f))) {
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warning*/ 2, "soft-dismiss", "下发失败", "无数据目录", 3000);
            } else {
                const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
                if (leds.gamePid == 0) {
                    notify::PushLocal(/*Warning*/ 2, "soft-dismiss", "下发失败", "需已注入游戏",
                                     3000);
                } else {
                    xcat::PayloadControl c{};
                    if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
                        xcat::PayloadControlSetDefaults(c);
                    }
                    c.softLoginDismissSeq =
                        c.softLoginDismissSeq == 0 ? 1u : c.softLoginDismissSeq + 1u;
                    if (c.softLoginDismissSeq == 0) c.softLoginDismissSeq = 1u;
                    c.writeTickMs = GetTickCount64();
                    if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                        xcat::log::Ok("App", "softLoginDismissSeq=%u（调试关断线弹窗）",
                                      c.softLoginDismissSeq);
                        notify::PushLocal(/*Info*/ 1, "soft-dismiss", "已下发关窗",
                                         "载荷将 CloseDialog+SetActive", 3500);
                    } else {
                        notify::PushLocal(/*Warning*/ 2, "soft-dismiss", "下发失败",
                                         "写 core 失败", 3000);
                    }
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "与软重连自动关窗同路径；不会点「確認」（防误认踢）。\n"
                "需游戏已注入；结果见 soft_login.log / 气泡。");
        }
    }
#if 0  // 暂时隐藏：自定义 DLL 注入入口（后端 InjectCustomDll 仍保留，改 1 即恢复 UI）
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_custom_inject", "自定义 DLL 注入");
        static char dllPathBuf[1024]{};
        static bool waitGa = false;  // 默认不等 GA；需要时再勾
        static bool pathLoaded = false;
        static std::string pathLoadedFor;
        static std::string lastStatus;

        const std::string persistPath =
            ui.prefsBinDir.empty() ? std::string{}
                                   : (ui.prefsBinDir + "\\state\\custom_inject_dll.txt");
        if (!persistPath.empty() && (!pathLoaded || pathLoadedFor != persistPath)) {
            pathLoaded = true;
            pathLoadedFor = persistPath;
            dllPathBuf[0] = '\0';
            waitGa = false;
            std::ifstream in(persistPath, std::ios::binary);
            if (in) {
                std::string line;
                if (std::getline(in, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                        line.pop_back();
                    std::snprintf(dllPathBuf, sizeof(dllPathBuf), "%s", line.c_str());
                }
                std::string flag;
                if (std::getline(in, flag)) {
                    while (!flag.empty() && (flag.back() == '\r' || flag.back() == '\n'))
                        flag.pop_back();
                    if (flag == "1" || flag == "true" || flag == "yes") waitGa = true;
                }
            }
        }

        auto persistCustomInject = [&]() {
            if (persistPath.empty()) return;
            const size_t slash = persistPath.find_last_of("\\/");
            if (slash != std::string::npos) {
                (void)xcat::CreateDirectoryUtf8(persistPath.substr(0, slash));
            }
            std::ofstream out(persistPath, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << dllPathBuf << "\n" << (waitGa ? "1" : "0") << "\n";
        };

        ImGui::TextWrapped(
            "向当前 Maplestory_Classic.exe 注入自选 DLL（LoadLibraryW）。"
            "与正式 xcat.dll 注入共用忙锁；成功不标记「已注入载荷」，监视仍会补正式 DLL。");

        const DWORD gamePid = xcat::FindProcessIdByName(L"Maplestory_Classic.exe");
        if (gamePid) {
            ImGui::Text("目标进程：PID %lu", static_cast<unsigned long>(gamePid));
        } else {
            ImGui::TextDisabled("目标进程：未找到 Maplestory_Classic.exe");
        }

        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextWithHint("##dbg_custom_dll", "DLL 绝对路径（.dll）", dllPathBuf,
                                     sizeof(dllPathBuf))) {
            persistCustomInject();
        }

        const float gap = ui::Gap();
        const float halfW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ImGui::Button("浏览…##dbg_custom_dll_browse", ImVec2(halfW, 0.f))) {
            sound::UiClick();
            wchar_t fileBuf[MAX_PATH]{};
            const std::wstring cur = xcat::Utf8ToWide(dllPathBuf);
            if (!cur.empty() && cur.size() < MAX_PATH) {
                wcsncpy_s(fileBuf, cur.c_str(), _TRUNCATE);
            }
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFilter = L"DLL (*.dll)\0*.dll\0All (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = L"选择要注入的 DLL";
            if (GetOpenFileNameW(&ofn)) {
                const std::string utf8 = xcat::WideToUtf8(fileBuf);
                std::snprintf(dllPathBuf, sizeof(dllPathBuf), "%s", utf8.c_str());
                persistCustomInject();
                lastStatus.clear();
            }
        }
        ImGui::SameLine(0.f, gap);
        const bool injBusy = attach_inject::IsInjectBusy();
        if (injBusy || !gamePid || !dllPathBuf[0]) ImGui::BeginDisabled();
        if (ImGui::Button("注入所选 DLL##dbg_custom_dll_go", ImVec2(halfW, 0.f))) {
            sound::UiClick();
            persistCustomInject();
            std::wstring err;
            if (!attach_inject::InjectCustomDll(xcat::Utf8ToWide(dllPathBuf), waitGa, &err)) {
                lastStatus = err.empty() ? "自定义注入失败" : xcat::WideToUtf8(err);
                ui.status = lastStatus;
                notify::PushLocal(/*Danger*/ 3, "custom-inject", "注入失败", lastStatus.c_str(),
                                  4000);
                sound::UiError();
            } else {
                lastStatus = waitGa ? "已开始自定义注入（等待 GameAssembly）…"
                                    : "已开始自定义注入…";
                ui.status = lastStatus;
                notify::PushLocal(/*Ok*/ 1, "custom-inject", "已开始注入", lastStatus.c_str(),
                                  2500);
            }
        }
        if (injBusy || !gamePid || !dllPathBuf[0]) ImGui::EndDisabled();

        if (ImGui::Checkbox("等待 GameAssembly 后再注入##dbg_custom_dll_waitga", &waitGa)) {
            persistCustomInject();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "勾选：与正式注入相同，等 GameAssembly.dll 出现后再 LoadLibrary（推荐游戏相关 DLL）。\n"
                "取消：立刻注入（适合不依赖 GA 的工具 DLL）。");
        }
        if (injBusy) {
            ImGui::TextColored(PrepHintBlue(), "注入进行中…");
        } else if (!lastStatus.empty()) {
            const std::string lastStatusUi = SanitizeImGuiLogLine(lastStatus);
            ImGui::TextDisabled("%s", lastStatusUi.c_str());
        }
        ImGui::TextDisabled("进度见面板日志 / bin/logs/launcher.jsonl（标签 Attach）");
    }
#endif
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_maint", "日志 / 更新");
        ImGui::TextWrapped("启动器 JSONL：bin/logs/launcher.jsonl");
        ImGui::TextWrapped("注入 JSONL：bin/logs/inject.jsonl");
        ImGui::TextWrapped("载荷 JSONL：bin/XCat_data/logs/x.jsonl");
        ImGui::TextWrapped("换票文本：bin/launcher.log");
        ImGui::TextWrapped("账号：仅 bin/account.txt（与 xcat.exe 同级；不写 LocalAppData）");
        if (ImGui::Button("清空面板日志", ImVec2(AppDpi_Px(140.f), 0.f))) ui.logTail.clear();
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.6f));
        ImGui::Separator();
        ImGui::TextUnformatted("上报日志到 ops 服务");
        DrawLogUploadPanel(ui.prefsBinDir);
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.6f));
        ImGui::Separator();
        DrawUpdateControl();
    }
    // 手填 OPS TOKEN 已退休：身份改由 gate/1 签名卡密（X-XCat-Gate-Token）提供可信 uid，
    // 服务端验签后用于配额 / 吊销 / 运维认人，用户无需再填第二个标识串，故隐去该入口。
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_combat_tick", "打怪节奏");
        static bool tickDbgLoaded = false;
        static uint64_t tickDbgSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!tickDbgLoaded || disk.writeTickMs != tickDbgSeen) {
                    gUiAttackHoldMs = (int)xcat::ClampAttackHoldMs(
                        disk.simpleCombatAttackHoldMs ? disk.simpleCombatAttackHoldMs
                                                      : xcat::kAttackHoldDefaultMs);
                    tickDbgSeen = disk.writeTickMs;
                    tickDbgLoaded = true;
                }
            } else if (!tickDbgLoaded) {
                tickDbgLoaded = true;
            }
        } else if (!tickDbgLoaded) {
            tickDbgLoaded = true;
        }

        auto persistHold = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.simpleCombatAttackHoldMs = xcat::ClampAttackHoldMs(
                static_cast<uint32_t>(gUiAttackHoldMs < 0 ? 0 : gUiAttackHoldMs));
            gUiAttackHoldMs = (int)c.simpleCombatAttackHoldMs;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                tickDbgSeen = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：hold=%u（调试）", c.simpleCombatAttackHoldMs);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] 打怪节奏参数失败");
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("按键hold");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_attack_hold_ms", &gUiAttackHoldMs, 1,
                           (int)xcat::kAttackHoldMinMs, (int)xcat::kAttackHoldMaxMs))
            persistHold();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "出刀按键按下到松开的时长（%u–%u ms，默认 %u）。\n"
                "实际取 min(此值, 出刀间隔)：hold ≥ 间隔会把下一刀锁死。\n"
                "注意：开着「攻击加速」时走 Down+Up 同泵的 pulse 路径，hold=0，此项不参与；\n"
                "只有关掉加速、走异步松键时才有效。\n"
                "调太小可能个别刀不被引擎识别（看日志 whiff 是否变多）。\n"
                "TICK 心跳已迁到「吸怪 快攻」TAB「快攻」卡。",
                (unsigned)xcat::kAttackHoldMinMs, (unsigned)xcat::kAttackHoldMaxMs,
                (unsigned)xcat::kAttackHoldDefaultMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("ms");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("加速开启时不参与");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_hangup_unbind", "主动软重连");
        static bool unbindDbgLoaded = false;
        static uint64_t unbindDbgSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!unbindDbgLoaded || disk.writeTickMs != unbindDbgSeen) {
                    gUiMobGatherHangupUnbindF5 = disk.mobGatherHangupUnbindF5 != 0;
                    unbindDbgSeen = disk.writeTickMs;
                    unbindDbgLoaded = true;
                }
            } else if (!unbindDbgLoaded) {
                unbindDbgLoaded = true;
            }
        } else if (!unbindDbgLoaded) {
            unbindDbgLoaded = true;
        }
        if (xcat::ui::OptionCheckbox("解除主动软重连绑定F5", &gUiMobGatherHangupUnbindF5)) {
            if (!ui.prefsBinDir.empty()) {
                xcat::PayloadControl c{};
                (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
                c.mobGatherHangupUnbindF5 = gUiMobGatherHangupUnbindF5 ? 1u : 0u;
                c.writeTickMs = GetTickCount64();
                if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                    unbindDbgSeen = c.writeTickMs;
                    xcat::log::Ok("App", "已下发 core：mobGatherHangupUnbindF5=%d（调试）",
                                  gUiMobGatherHangupUnbindF5 ? 1 : 0);
                } else {
                    xcat::log::Warn("App", "写入 user.ini [core] 解除绑定F5 失败");
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关。关着：追怪选「瞬移找怪」且 F5 开着时，「吸怪 快攻」TAB「快攻」卡「主动软重连」秒数闸强制开（面板置灰，不改落盘）。\n"
                "勾上：不再强制，秒数闸只跟该勾选。出刀累计闸不受影响。\n"
                "「软重连试连」仍须开才会拆会话。");
        }
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_pump_drain", "主线程泵");
        static int pumpDrainBudget = (int)xcat::kPumpDrainBudgetDefault;
        static bool drainDbgLoaded = false;
        static uint64_t drainDbgSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!drainDbgLoaded || disk.writeTickMs != drainDbgSeen) {
                    pumpDrainBudget = (int)xcat::ClampPumpDrainBudget(
                        disk.pumpDrainBudget ? disk.pumpDrainBudget
                                             : xcat::kPumpDrainBudgetDefault);
                    gUiPumpCongestion =
                        (int)xcat::ClampPumpCongestion(disk.pumpCongestionThreshold);
                    drainDbgSeen = disk.writeTickMs;
                    drainDbgLoaded = true;
                }
            } else if (!drainDbgLoaded) {
                drainDbgLoaded = true;
            }
        } else if (!drainDbgLoaded) {
            drainDbgLoaded = true;
        }

        auto persistPump = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.pumpDrainBudget = xcat::ClampPumpDrainBudget(
                static_cast<uint32_t>(pumpDrainBudget < 0 ? 0 : pumpDrainBudget));
            pumpDrainBudget = (int)c.pumpDrainBudget;
            c.pumpCongestionThreshold = xcat::ClampPumpCongestion(
                static_cast<uint32_t>(gUiPumpCongestion < 0 ? 0 : gUiPumpCongestion));
            gUiPumpCongestion = (int)c.pumpCongestionThreshold;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                drainDbgSeen = c.writeTickMs;
                xcat::log::Ok("App",
                              "已下发 core：pumpDrainBudget=%u pumpCongestion=%u（调试）",
                              c.pumpDrainBudget, c.pumpCongestionThreshold);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] 主线程泵参数失败");
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("拥堵让路阈值");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_pump_congestion", &gUiPumpCongestion, 1,
                           (int)xcat::kPumpCongestionMin, (int)xcat::kPumpCongestionMax)) {
            gUiPumpCongestion = (int)xcat::ClampPumpCongestion(
                static_cast<uint32_t>(gUiPumpCongestion < 0 ? 0 : gUiPumpCongestion));
            persistPump();
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextDisabled("格");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "游戏主线程排队任务达到这么多格时，出刀/瞬移先让一拍（背压，防灌爆触发 GC 弹窗）。\n"
                "队列共 %u 格，默认 %u。调小=更早让路更保守；调大=更少让路；\n"
                "0=关闭背压（高风险，恢复旧行为）。",
                (unsigned)xcat::kPumpCongestionMax, (unsigned)xcat::kPumpCongestionDefault);
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("每 tick Drain");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_pump_drain", &pumpDrainBudget, 1,
                           (int)xcat::kPumpDrainBudgetMin, (int)xcat::kPumpDrainBudgetMax)) {
            pumpDrainBudget = (int)xcat::ClampPumpDrainBudget(
                static_cast<uint32_t>(pumpDrainBudget < 0 ? 0 : pumpDrainBudget));
            persistPump();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "主线程泵每个宿主 tick 最多执行多少个排队 job（%u–%u，默认 %u=抽干整队）。\n"
                "队列共 %u 格。调小=单帧更轻、易 defer/跳刀；调大=吞吐更高、单帧可能更尖。\n"
                "与上方「拥堵让路阈值」无关：那是背压，这项是每 tick 清队上限。",
                (unsigned)xcat::kPumpDrainBudgetMin, (unsigned)xcat::kPumpDrainBudgetMax,
                (unsigned)xcat::kPumpDrainBudgetDefault, (unsigned)xcat::kPumpDrainBudgetMax);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextDisabled("格 · 默认抽干");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_tp_hop", "贴怪 hop");
        static int teleportMaxHop = (int)xcat::kCombatTeleportMaxHopDefault;
        static bool hopDbgLoaded = false;
        static uint64_t hopDbgSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!hopDbgLoaded || disk.writeTickMs != hopDbgSeen) {
                    teleportMaxHop = (int)xcat::ClampCombatTeleportMaxHop(
                        disk.simpleCombatTeleportMaxHop ? disk.simpleCombatTeleportMaxHop
                                                       : xcat::kCombatTeleportMaxHopDefault);
                    hopDbgSeen = disk.writeTickMs;
                    hopDbgLoaded = true;
                }
            } else if (!hopDbgLoaded) {
                hopDbgLoaded = true;
            }
        } else if (!hopDbgLoaded) {
            hopDbgLoaded = true;
        }

        auto persistHop = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.simpleCombatTeleportMaxHop = xcat::ClampCombatTeleportMaxHop(
                static_cast<uint32_t>(teleportMaxHop < 0 ? 0 : teleportMaxHop));
            teleportMaxHop = (int)c.simpleCombatTeleportMaxHop;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                hopDbgSeen = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：simpleCombatTeleportMaxHop=%u（调试）",
                              c.simpleCombatTeleportMaxHop);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] simpleCombatTeleportMaxHop 失败");
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("单次上限");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(64.f));
        if (ImGui::DragInt("##dbg_tp_max_hop", &teleportMaxHop, 1,
                           (int)xcat::kCombatTeleportMaxHopMin, INT_MAX)) {
            teleportMaxHop = (int)xcat::ClampCombatTeleportMaxHop(
                static_cast<uint32_t>(teleportMaxHop < 0 ? 0 : teleportMaxHop));
            persistHop();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "贴怪单次瞬移最大距离（下限 %u px，默认 %u，无上限）。\n"
                "更远会分段贴近；过大易软断（lean_local_or_soft）。",
                (unsigned)xcat::kCombatTeleportMaxHopMin,
                (unsigned)xcat::kCombatTeleportMaxHopDefault);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("px");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("下限 %u · 默认 %u · 无上限",
                            (unsigned)xcat::kCombatTeleportMaxHopMin,
                            (unsigned)xcat::kCombatTeleportMaxHopDefault);
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_travel", "超级赶路");
        static int portalAimLiftY = (int)xcat::kTravelPortalAimLiftDefault;
        static bool travelDbgLoaded = false;
        static uint64_t travelDbgSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!travelDbgLoaded || disk.writeTickMs != travelDbgSeen) {
                    portalAimLiftY = (int)xcat::ClampTravelPortalAimLiftY(
                        disk.travelPortalAimLiftY ? disk.travelPortalAimLiftY
                                                 : xcat::kTravelPortalAimLiftDefault);
                    travelDbgSeen = disk.writeTickMs;
                    travelDbgLoaded = true;
                }
            } else if (!travelDbgLoaded) {
                travelDbgLoaded = true;
            }
        } else if (!travelDbgLoaded) {
            travelDbgLoaded = true;
        }

        auto persistTravelLift = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.travelPortalAimLiftY = xcat::ClampTravelPortalAimLiftY(
                static_cast<uint32_t>(portalAimLiftY < 0 ? 0 : portalAimLiftY));
            portalAimLiftY = (int)c.travelPortalAimLiftY;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                travelDbgSeen = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：travelPortalAimLiftY=%u（调试）",
                              c.travelPortalAimLiftY);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] travelPortalAimLiftY 失败");
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("贴门抬升");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_travel_aim_lift", &portalAimLiftY, 1,
                           (int)xcat::kTravelPortalAimLiftMin,
                           (int)xcat::kTravelPortalAimLiftMax)) {
            portalAimLiftY = (int)xcat::ClampTravelPortalAimLiftY(
                static_cast<uint32_t>(portalAimLiftY < 0 ? 0 : portalAimLiftY));
            persistTravelLift();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "贴门旋翼相对台面再抬高多少（%u–%u px，默认 %u）。\n"
                "AbsPos：更大 Y = 更高（远离掉落方向）。\n"
                "末段 / 台下恢复用这个值。同层已挂台不抬，贴台滑。\n"
                "只有门心±16 发门带里没台才悬停进门。\n"
                "太小会贴台掉虚空；太大在门口晃一下再落。\n"
                "写入 [core] travelPortalAimLiftY；已注入则即时生效。",
                (unsigned)xcat::kTravelPortalAimLiftMin, (unsigned)xcat::kTravelPortalAimLiftMax,
                (unsigned)xcat::kTravelPortalAimLiftDefault);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("px");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("AbsPos 更大 Y=更高 · 默认 %u",
                            (unsigned)xcat::kTravelPortalAimLiftDefault);
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_miss", "锚点 MISS 灯");
        ImGui::TextDisabled("绿=OK · 黄=降级(shape/部分MI) · 红=MISS · 灰=pending(已占位未决)");
        ImGui::TextDisabled("注入后整表先灰后变色；某灯一直灰=对应 feature 尚未上报/卡在绑定前");
        xcat::AnchorLampsStatus lamps{};
        const bool have = !ui.prefsBinDir.empty() &&
                          xcat::ReadAnchorLamps(ui.prefsBinDir.c_str(), lamps) &&
                          xcat::AnchorLampsFresh(lamps, GetTickCount64(), 8000);
        if (!have) {
            ImGui::TextDisabled("等待注入后 payload 上报（约 0.5s 心跳）…");
        } else {
            const float cellW = AppDpi_Px(78.f);
            const float cellH = AppDpi_Px(28.f);
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float avail = ImGui::GetContentRegionAvail().x;
            int cols = (std::max)(1, (int)((avail + gap) / (cellW + gap)));
            if (cols > 7) cols = 7;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (uint32_t i = 0; i < lamps.count; ++i) {
                if (i > 0 && (int)(i % (uint32_t)cols) != 0) ImGui::SameLine(0.f, gap);
                const auto& e = lamps.entries[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::InvisibleButton("##lamp", ImVec2(cellW, cellH));
                const ImVec2 p0 = ImGui::GetItemRectMin();
                const ImVec2 p1 = ImGui::GetItemRectMax();
                ImU32 fill = IM_COL32(120, 120, 120, 255);
                const char* codeLabel = "未知";
                switch (static_cast<xcat::AnchorLampCode>(e.code)) {
                case xcat::AnchorLampCode::Ok:
                    fill = IM_COL32(64, 220, 98, 255);
                    codeLabel = "OK";
                    break;
                case xcat::AnchorLampCode::Degraded:
                    fill = IM_COL32(240, 190, 64, 255);
                    codeLabel = "降级";
                    break;
                case xcat::AnchorLampCode::Miss:
                    fill = IM_COL32(230, 72, 72, 255);
                    codeLabel = "MISS";
                    break;
                default:
                    break;
                }
                if (dl) {
                    const float r = AppDpi_Px(4.5f);
                    const ImVec2 c(p0.x + AppDpi_Px(10.f), (p0.y + p1.y) * 0.5f);
                    dl->AddCircleFilled(c, r + AppDpi_Px(2.5f),
                                        (fill & 0x00FFFFFFu) | 0x50000000u, 16);
                    dl->AddCircleFilled(c, r, fill, 16);
                    const ImVec2 t(c.x + AppDpi_Px(10.f), p0.y + (cellH - ImGui::GetTextLineHeight()) * 0.5f);
                    dl->AddText(t, ImGui::GetColorU32(ImGuiCol_Text), e.id[0] ? e.id : "?");
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("%s\n状态：%s\n%s", e.id[0] ? e.id : "?", codeLabel,
                                      e.detail[0] ? e.detail : "(无明细)");
                }
                ImGui::PopID();
            }
            ImGui::TextDisabled("共 %u 项 · tick %llu", lamps.count,
                                static_cast<unsigned long long>(lamps.writeTickMs));
        }
    }
    CardGap();
    {
        static int flyHopCdMs = (int)xcat::kFlyHopCdDefaultMs;
        static int flyRoute = (int)xcat::kFlyModeDefault;
        static bool flyDbgLoaded = false;
        static uint64_t flyDbgTick = 0;
        static bool editingFlyCd = false;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                // 正在改间隔时不要被其它写盘把值拽回去
                if (!flyDbgLoaded || (!editingFlyCd && disk.writeTickMs != flyDbgTick)) {
                    flyHopCdMs = (int)xcat::ClampFlyHopCdMs(
                        disk.flyHopCdMs ? disk.flyHopCdMs : xcat::kFlyHopCdDefaultMs);
                    flyRoute = (int)xcat::ClampFlyMode(disk.flyMode);
                    // 倍率在首页「飞行速度」卡；此处只跟盘同步共享变量，避免调试页改路线时盖掉倍率。
                    gUiManualFlySpeedPct = (int)xcat::ClampHeliSpeedPct(
                        disk.flySpeedPct ? disk.flySpeedPct : xcat::kFlySpeedPctDefault);
                    gUiFlySpeedPct = (int)xcat::ClampHeliSpeedPct(
                        disk.simpleCombatFlySpeedPct ? disk.simpleCombatFlySpeedPct
                                                     : xcat::kHeliSpeedPctDefault);
                    gUiAntiJitter = disk.simpleCombatAntiJitter != 0;
                    flyDbgTick = disk.writeTickMs;
                    flyDbgLoaded = true;
                }
            } else if (!flyDbgLoaded) {
                flyDbgLoaded = true;
            }
        }
        auto persistFlyDbg = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.flyMode = xcat::ClampFlyMode(static_cast<uint32_t>(flyRoute < 0 ? 0 : flyRoute));
            c.flyHopCdMs = xcat::ClampFlyHopCdMs(
                static_cast<uint32_t>(flyHopCdMs < 0 ? 0 : flyHopCdMs));
            // 不覆盖 flySpeedPct / simpleCombatFlySpeedPct（首页卡真源）
            flyRoute = (int)c.flyMode;
            flyHopCdMs = (int)c.flyHopCdMs;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) flyDbgTick = c.writeTickMs;
        };
        auto persistAntiJitter = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.simpleCombatAntiJitter = gUiAntiJitter ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) flyDbgTick = c.writeTickMs;
        };

        xcat::ui::CardGuard card("##tab_dbg_fly", "飞行调试");
        ImGui::TextDisabled("F6 飞行（闭环旋翼）；武装期禁挂台");
        ImGui::TextDisabled("F6/F5 速度倍率：首页「飞行速度」卡（当前手动 %d%% / 滑翔 %d%%）",
                            gUiManualFlySpeedPct, gUiFlySpeedPct);

        if (xcat::ui::OptionCheckbox("防抖", &gUiAntiJitter)) persistAntiJitter();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "空中贴怪防抖 = 钉 Y + X 跟 ideal + 两轴软悬停（默认开）。\n"
                "开：锁怪后接近静止；关：回旧跟点 + 90ms 进近律（会重新有微晃）。\n"
                "仍用 Station（保留满速进站预刹）；不会改成 Hold。\n"
                "ini：core.simpleCombatAntiJitter=0 也可关。");
        }
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("F5 空中贴怪 · 默认开");

        ImGui::Separator();
        ImGui::TextUnformatted("推进路线");
        ImGui::SameLine();
        if (ImGui::RadioButton("路线 A##dbg_fly_nb", flyRoute == (int)xcat::kFlyModeImpactNockBack)) {
            flyRoute = (int)xcat::kFlyModeImpactNockBack;
            persistFlyDbg();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("路线 B##dbg_fly_sin",
                               flyRoute == (int)xcat::kFlyModeImpactSetNext)) {
            flyRoute = (int)xcat::kFlyModeImpactSetNext;
            persistFlyDbg();
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("换闭环旋翼后这两条路线对飞行已不起作用：\n"
                              "冲量路由由旋翼内部决定。保留仅为存档兼容。");
        }

        ImGui::TextUnformatted("目标刷新间隔 (ms)");
        ImGui::SetNextItemWidth(AppDpi_Px(120.f));
        if (ImGui::InputInt("##dbg_fly_hop_cd_in", &flyHopCdMs, 1, 10)) {
            flyHopCdMs = (int)xcat::ClampFlyHopCdMs(
                static_cast<uint32_t>(flyHopCdMs < 0 ? 0 : flyHopCdMs));
            persistFlyDbg();
        }
        editingFlyCd = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) persistFlyDbg();
        ImGui::SetNextItemWidth(AppDpi_Px(-1.f));
        if (ImGui::SliderInt("##dbg_fly_hop_cd_sl", &flyHopCdMs, (int)xcat::kFlyHopCdMinMs,
                             (int)xcat::kFlyHopCdMaxMs, "%d ms")) {
            flyHopCdMs = (int)xcat::ClampFlyHopCdMs(
                static_cast<uint32_t>(flyHopCdMs < 0 ? 0 : flyHopCdMs));
            persistFlyDbg();
        }
        editingFlyCd = editingFlyCd || ImGui::IsItemActive();
        // 快捷档：极致 / 跟手 / 干净包
        auto bumpCd = [&](int v) {
            flyHopCdMs = (int)xcat::ClampFlyHopCdMs(static_cast<uint32_t>(v));
            persistFlyDbg();
        };
        if (ImGui::Button("5ms##dbg_fly_cd5")) bumpCd(5);
        ImGui::SameLine();
        if (ImGui::Button("16ms##dbg_fly_cd16")) bumpCd(16);
        ImGui::SameLine();
        if (ImGui::Button("40ms##dbg_fly_cd40")) bumpCd(40);
        ImGui::SameLine();
        if (ImGui::Button("80ms##dbg_fly_cd80")) bumpCd(80);
        ImGui::SameLine();
        if (ImGui::Button("120ms##dbg_fly_cd120")) bumpCd(120);
        ImGui::SameLine();
        if (ImGui::Button("400ms##dbg_fly_cd400")) bumpCd(400);
        ImGui::TextDisabled("范围 %u–%u，默认 %u（多久重算一次鼠标世界点）",
                            xcat::kFlyHopCdMinMs, xcat::kFlyHopCdMaxMs,
                            xcat::kFlyHopCdDefaultMs);
        ImGui::TextDisabled("实测系统时钟一格 15.6ms：低于 16 的设定不会更跟手，走同一条路径。");
    }
    if (gGatherTabUnlocked) {
        CardGap();
        DrawMobGatherDyLimScanCard(ui);
        CardGap();
        DrawMobGatherFlyDebugCards(ui);
    }
    CardGap();
    {
        // MovePath.Flush 采证：inline hook 上报包，dump MoveElem → logs\movepath_flush.log。
        // 本仓禁止常驻 inline hook——此开关默认关，只在抓「被踢前上报了什么」时手动开，用完即关。
        static bool mpFlush = false;
        static bool mpLoaded = false;
        static uint64_t mpTick = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!mpLoaded || disk.writeTickMs != mpTick) {
                    mpFlush = disk.movepathFlushProbe != 0;
                    mpTick = disk.writeTickMs;
                    mpLoaded = true;
                }
            } else if (!mpLoaded) {
                mpLoaded = true;
            }
        }
        auto persistMpFlush = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.movepathFlushProbe = mpFlush ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) mpTick = c.writeTickMs;
        };
        xcat::ui::CardGuard card("##tab_dbg_mpflush", "上报采证(MovePath.Flush)");
        if (xcat::ui::OptionCheckbox("采证上报包(测试专用·用完即关)", &mpFlush)) persistMpFlush();
        ImGui::SameLine();
        ImGui::TextDisabled("勾选自动允许.text钩");
        ImGui::TextDisabled("进图后开→飞/跑一段；对 logs\\movepath_flush.log");
        ImGui::TextDisabled("标注：a=N!非Normal  fh=0*空中  err=..!!越包络(地>18/空>27)  头 maxErr/air/tel/over");
    }
    CardGap();
    {
        // 测谎干跑 / 泵测：从首页挪到调试，避免打扰日常挂机界面
        static bool dryRun = false;
        static bool dryLoaded = false;
        static uint64_t dryTick = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!dryLoaded || disk.writeTickMs != dryTick) {
                    dryRun = disk.autoLieDryRun != 0;
                    dryTick = disk.writeTickMs;
                    dryLoaded = true;
                }
            } else if (!dryLoaded) {
                dryLoaded = true;
            }
        }

        auto persistDry = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.autoLieDryRun = dryRun ? 1u : 0u;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) dryTick = c.writeTickMs;
        };
        enum class LieDbgSeq { Alarm, MouseSmoke, MouseSim };
        auto bumpSeq = [&](LieDbgSeq which) {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.autoLieDryRun = dryRun ? 1u : 0u;
            uint32_t* slot = which == LieDbgSeq::Alarm        ? &c.autoLieAlarmTestSeq
                             : which == LieDbgSeq::MouseSmoke ? &c.autoLieMouseSmokeSeq
                                                              : &c.autoLieMouseSimSeq;
            if (++*slot == 0) *slot = 1;  // payload 侧按 seq 相等去重，0 是「未请求」
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) dryTick = c.writeTickMs;
        };

        xcat::ui::CardGuard card("##tab_dbg_lie", "测谎诊断");
        if (xcat::ui::OptionCheckbox("测谎干跑(不OnOk)", &dryRun)) persistDry();
        ImGui::SameLine();
        ImGui::TextDisabled("服端未开时可先验泵/报警");
        {
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float rowW = ImGui::GetContentRegionAvail().x;
            const float btnW = (std::max)(1.f, (rowW - gap * 4.f) * 0.2f);
            if (ImGui::Button("本地泵", ImVec2(btnW, 0.f))) {
                const std::string id =
                    xcat::app::LieAiPump_EnqueueFixture(ui.prefsBinDir, /*echoOnly=*/true);
                if (id.empty()) {
                    notify::PushLocal(/*Danger*/ 3, "lie-fix", "测谎夹具失败",
                                      "无法写入 lie_ai/req", 4500);
                } else {
                    notify::PushLocal(/*Info*/ 0, "lie-fix", "本地泵已投递", id.c_str(), 3500);
                }
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("LLM泵", ImVec2(btnW, 0.f))) {
                const std::string id =
                    xcat::app::LieAiPump_EnqueueFixture(ui.prefsBinDir, /*echoOnly=*/false);
                if (id.empty()) {
                    notify::PushLocal(/*Danger*/ 3, "lie-fix", "测谎夹具失败",
                                      "无法写入 lie_ai/req", 4500);
                } else {
                    notify::PushLocal(/*Info*/ 0, "lie-fix", "LLM泵已投递", id.c_str(), 3500);
                }
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("测试报警", ImVec2(btnW, 0.f))) {
                bumpSeq(LieDbgSeq::Alarm);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("需已注入：约 12 秒通知+Alarm 音效（每 3s），不答题。");
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("光标烟测", ImVec2(btnW, 0.f))) {
                bumpSeq(LieDbgSeq::MouseSmoke);
                notify::PushLocal(/*Warning*/ 2, "lie-mouse-smoke", "光标烟测",
                                  "约 3 秒；仅游戏前台锁光标。需已注入。", 4500);
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("模拟答题", ImVec2(btnW, 0.f))) {
                bumpSeq(LieDbgSeq::MouseSim);
                notify::PushLocal(/*Warning*/ 2, "lie-mouse-sim", "模拟答题",
                                  "约 15 秒：165 准备帧 + 330 点轨迹回放，锁光标并硬闸战斗。",
                                  5000);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "需已注入：按真题时序回放内置轨迹（165+330@33Hz），不提交答案。\n"
                    "面板几何优先级 LIVE > REPLAY > SYNTH，当前档位见青框标签与 AutoLieSim 日志。");
            }
        }
        {
            // 已测谎统计：payload 落 state\lie_stats.tsv（角色 × 题型累计）。
            // 两套数：seen/已答/未答/掉线/超时 = 我们干得如何；通过/判败 = 服端判定（_isSuccess）。
            struct LieStatRow {
                std::string name;
                std::string kind;
                unsigned long seen = 0, answered = 0, missed = 0, timeout = 0;
                unsigned long passed = 0, failed = 0, dropped = 0;
            };
            static std::vector<LieStatRow> statRows;
            static uint64_t statNextPoll = 0;
            const uint64_t nowMs = GetTickCount64();
            if (nowMs >= statNextPoll) {
                statNextPoll = nowMs + 2000;
                statRows.clear();
                if (!ui.prefsBinDir.empty()) {
                    std::ifstream f(ui.prefsBinDir + "\\state\\lie_stats.tsv", std::ios::binary);
                    std::string line;
                    while (std::getline(f, line)) {
                        if (line.empty() || line[0] == '#') continue;
                        if (line.back() == '\r') line.pop_back();
                        std::istringstream ss(line);
                        LieStatRow r;
                        std::string seen, ans, miss, to, last, pass, fail, drop;
                        if (!std::getline(ss, r.name, '\t')) continue;
                        if (!std::getline(ss, r.kind, '\t')) continue;
                        if (!std::getline(ss, seen, '\t')) continue;
                        if (!std::getline(ss, ans, '\t')) continue;
                        if (!std::getline(ss, miss, '\t')) continue;
                        if (!std::getline(ss, to, '\t')) continue;
                        // ver1 的表只到 lastSec；判定两列是后加的，缺了当 0。
                        if (!std::getline(ss, last, '\t')) last = "0";
                        if (!std::getline(ss, pass, '\t')) pass = "0";
                        if (!std::getline(ss, fail, '\t')) fail = "0";
                        if (!std::getline(ss, drop, '\t')) drop = "0";
                        r.seen = strtoul(seen.c_str(), nullptr, 10);
                        r.answered = strtoul(ans.c_str(), nullptr, 10);
                        r.missed = strtoul(miss.c_str(), nullptr, 10);
                        r.timeout = strtoul(to.c_str(), nullptr, 10);
                        r.passed = strtoul(pass.c_str(), nullptr, 10);
                        r.failed = strtoul(fail.c_str(), nullptr, 10);
                        r.dropped = strtoul(drop.c_str(), nullptr, 10);
                        statRows.push_back(std::move(r));
                    }
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("已测谎统计（本机累计 · 按角色）");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "前五列是本工具的作答情况：\n"
                    "遇到=题目弹出过；已答=答案交出去了（知识题 OnOk / 轨迹题采满或服端已回结果）；\n"
                    "未答=关窗时我们没答上（建图失败、坏答案等）；超时=AI 未在限时内出答案。\n"
                    "掉线=开题到收尾之间断过线，题是被网络冲掉的，不算我们没答上——\n"
                    "　　这一列高就该去查网络，调工具没用（轨迹题要连续采满 330 点才能交卷）。\n"
                    "\n"
                    "后两列是服端判定（读 NonFinite._isSuccess，交卷后的淡出期取到）：\n"
                    "通过 / 判败 = 官方结果。它与「已答」不该相等——已答只说明点交出去了，\n"
                    "交上去的轨迹合不合格是另一回事，差额就是脏轨迹的量。\n"
                    "判定读不到时（-1）不记账，所以两边之和可能少于已答。\n"
                    "\n"
                    "干跑期间不记账。数据文件：state\\lie_stats.tsv");
            }
            if (statRows.empty()) {
                ImGui::TextDisabled("暂无记录 — 遇到真题后自动累计");
            } else if (ImGui::BeginTable("##lie_stats", 9,
                                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("角色");
                ImGui::TableSetupColumn("题型");
                ImGui::TableSetupColumn("遇到");
                ImGui::TableSetupColumn("已答");
                ImGui::TableSetupColumn("未答");
                ImGui::TableSetupColumn("掉线");
                ImGui::TableSetupColumn("超时");
                ImGui::TableSetupColumn("通过");
                ImGui::TableSetupColumn("判败");
                ImGui::TableHeadersRow();
                for (const LieStatRow& r : statRows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.kind == "mouse" ? "轨迹题" : "知识题");
                    ImGui::TableNextColumn();
                    ImGui::Text("%lu", r.seen);
                    ImGui::TableNextColumn();
                    ImGui::Text("%lu", r.answered);
                    ImGui::TableNextColumn();
                    if (r.missed) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "%lu", r.missed);
                    else ImGui::Text("%lu", r.missed);
                    ImGui::TableNextColumn();
                    // 冷色：这列高是网络的事，不是工具答错了，别跟红色的「未答」混为一谈。
                    if (r.dropped) ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.f, 1.f), "%lu", r.dropped);
                    else ImGui::TextDisabled("0");
                    ImGui::TableNextColumn();
                    if (r.timeout) ImGui::TextColored(ImVec4(1.f, 0.78f, 0.3f, 1.f), "%lu", r.timeout);
                    else ImGui::Text("%lu", r.timeout);
                    ImGui::TableNextColumn();
                    if (r.passed) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.f), "%lu", r.passed);
                    else ImGui::TextDisabled("0");
                    ImGui::TableNextColumn();
                    if (r.failed) ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "%lu", r.failed);
                    else ImGui::TextDisabled("0");
                }
                ImGui::EndTable();
            }
        }
    }
    CardGap();
    {
        // 原生瞬移调试入口已关闭；不再对外展示试推控件。
        xcat::ui::CardGuard card("##tab_dbg_tp", "瞬移 / 踢号压测");
        ImGui::TextDisabled("原生瞬移调试按钮已关闭（测试贴怪 / 原生CALL / 踢号压测）。");
        ImGui::TextDisabled("旧 user.ini 的 teleport*Seq 会被 payload 拒发并写 Control 日志。");
    }
    CardGap();
    {
        static bool memWatch = true;
        xcat::ui::CardGuard card("##tab_dbg_mem", "低内存守护");
        xcat::ui::OptionCheckbox("低内存自动回收", &memWatch);
        xcat::ui::OptionCheckbox("换图后回收", &memWatch);
        if (ImGui::Button("手动安全回收一次", ImVec2(AppDpi_Px(160.f), 0.f))) {
        }
        ImGui::TextDisabled("payload 内存指标：未注入");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_theme", "界面（诊断）");
        ImGui::Text("主题：%s", AppTheme_IsLight() ? "白天" : "暗夜");
        ImGui::TextWrapped("顶栏可切换主题；偏好写入 %s\\state\\user.ini",
                           ui.prefsBinDir.empty() ? "XCat_data" : ui.prefsBinDir.c_str());
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_about", "关于");
        ImGui::TextWrapped(
            "目标渠道：TW（换票 / 官方启动链）。\n"
            "换票：同进程 GAMA PASS CDP / HTTP Beanfun。\n"
            "UI：工作区 Tab + CardGuard。\n"
            "功能页按模块接入；挂机时段已接 launcher 调度（杀/启游戏）。");
    }
}

void DrawSkillDragGrip(const ImVec2& size) {
    ImGui::InvisibleButton("##grip", size);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
    const ImU32 col = ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float w = AppDpi_Px(11.f);
    const float thick = AppDpi_Px(1.6f);
    const float gap = AppDpi_Px(3.6f);
    for (int k = -1; k <= 1; ++k) {
        const float y = cy + static_cast<float>(k) * gap;
        dl->AddLine(ImVec2(cx - w * 0.5f, y), ImVec2(cx + w * 0.5f, y), col, thick);
    }
}

struct SkillListAnim {
    int dragId = 0;
    int liftId = 0;
    int insertAt = 0;
    float grabOff = 0.f;
    float lift = 0.f;
    float y[xcat::kAutoSkillOrderMax]{};
    float vy[xcat::kAutoSkillOrderMax]{};
    int idAt[xcat::kAutoSkillOrderMax]{};
    uint32_t n = 0;
};

void SkillListAnimSnap(SkillListAnim& a, const int* ids, uint32_t n, float rowH) {
    a.dragId = 0;
    a.liftId = 0;
    a.insertAt = 0;
    a.grabOff = 0.f;
    a.lift = 0.f;
    a.n = n;
    for (uint32_t i = 0; i < xcat::kAutoSkillOrderMax; ++i) {
        a.idAt[i] = (i < n) ? ids[i] : 0;
        a.y[i] = static_cast<float>(i) * rowH;
        a.vy[i] = 0.f;
    }
}

void SkillListSpring(float& y, float& vy, float target, float dt) {
    constexpr float kStiff = 420.f;
    constexpr float kDamp = 30.f;
    const float err = target - y;
    vy += (kStiff * err - kDamp * vy) * dt;
    y += vy * dt;
    if (fabsf(err) < 0.45f && fabsf(vy) < 12.f) {
        y = target;
        vy = 0.f;
    }
}

float SkillListRubber(float v, float lo, float hi) {
    if (v < lo) return lo + (v - lo) * 0.22f;
    if (v > hi) return hi + (v - hi) * 0.22f;
    return v;
}

int SkillListInsertAt(SkillListAnim& a, int dragFrom, uint32_t n, float rowH) {
    if (dragFrom < 0 || n == 0) return -1;
    const float slot = (a.y[dragFrom] + rowH * 0.5f) / rowH;
    int raw = static_cast<int>(floorf(slot));
    if (raw < 0) raw = 0;
    if (raw >= static_cast<int>(n)) raw = static_cast<int>(n) - 1;
    int cur = a.insertAt;
    if (cur < 0 || cur >= static_cast<int>(n)) cur = dragFrom;
    if (raw > cur && slot > static_cast<float>(cur) + 0.62f) cur = raw;
    else if (raw < cur && slot < static_cast<float>(cur) + 0.38f) cur = raw;
    a.insertAt = cur;
    return cur;
}

void SkillListReorder(int* ids, int* tgt, float* ys, float* vys, uint32_t n, int from, int to) {
    if (from < 0 || to < 0 || from == to) return;
    if (static_cast<uint32_t>(from) >= n || static_cast<uint32_t>(to) >= n) return;
    const int id = ids[from];
    const int t = tgt[from];
    const float y = ys[from];
    const float v = vys[from];
    if (from < to) {
        for (int k = from; k < to; ++k) {
            ids[k] = ids[k + 1];
            tgt[k] = tgt[k + 1];
            ys[k] = ys[k + 1];
            vys[k] = vys[k + 1];
        }
    } else {
        for (int k = from; k > to; --k) {
            ids[k] = ids[k - 1];
            tgt[k] = tgt[k - 1];
            ys[k] = ys[k - 1];
            vys[k] = vys[k - 1];
        }
    }
    ids[to] = id;
    tgt[to] = t;
    ys[to] = y;
    vys[to] = v;
}

float SkillListTargetY(uint32_t index, int dragFrom, int insertAt, float rowH) {
    if (dragFrom < 0) return static_cast<float>(index) * rowH;
    if (static_cast<int>(index) == dragFrom) return -1.f;
    int compact = static_cast<int>(index);
    if (compact > dragFrom) compact -= 1;
    if (compact >= insertAt) compact += 1;
    return static_cast<float>(compact) * rowH;
}

struct SkillListCols {
    float pad = 0.f;
    float gap = 0.f;
    float wGrip = 0.f;
    float wIdx = 0.f;
    float wName = 0.f;
    float wTgt = 0.f;
    float wMax = 0.f;
    float wDel = 0.f;
    float xGrip = 0.f;
    float xIdx = 0.f;
    float xName = 0.f;
    float xTgt = 0.f;
    float xMax = 0.f;
    float xDel = 0.f;
    float rowW = 0.f;
};

SkillListCols MakeSkillListCols(float avail) {
    SkillListCols c{};
    c.pad = AppDpi_Px(10.f);
    c.gap = ImGui::GetStyle().ItemSpacing.x;
    if (c.gap < AppDpi_Px(8.f)) c.gap = AppDpi_Px(8.f);
    const float gripGap = AppDpi_Px(10.f);
    c.wGrip = AppDpi_Px(28.f);
    c.wIdx = (std::max)(ImGui::CalcTextSize("顺序").x + AppDpi_Px(6.f), AppDpi_Px(28.f));
    c.wTgt = AppDpi_Px(56.f);
    c.wMax = AppDpi_Px(40.f);
    c.wDel = AppDpi_Px(28.f);
    c.rowW = avail;
    const float fixed = c.wGrip + c.wIdx + c.wTgt + c.wMax + c.wDel;
    const float gaps = gripGap + c.gap * 4.f;
    c.wName = avail - c.pad * 2.f - fixed - gaps;
    if (c.wName < AppDpi_Px(72.f)) c.wName = AppDpi_Px(72.f);
    c.xGrip = c.pad;
    c.xIdx = c.xGrip + c.wGrip + gripGap;
    c.xName = c.xIdx + c.wIdx + c.gap;
    c.xTgt = c.xName + c.wName + c.gap;
    c.xMax = c.xTgt + c.wTgt + c.gap;
    c.xDel = c.xMax + c.wMax + c.gap;
    return c;
}

void SkillListHeader(const SkillListCols& c) {
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float y = o.y;
    const float h = ImGui::GetFrameHeight();
    auto at = [&](float x, const char* s) {
        ImGui::SetCursorScreenPos(ImVec2(o.x + x, y));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", s);
    };
    at(c.xIdx, "顺序");
    at(c.xName, "技能");
    at(c.xTgt, "目标");
    at(c.xMax, "满级");
    if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
        const ImU32 line = ImGui::GetColorU32(ImGuiCol_Border, 0.45f);
        const float y1 = y + h - 1.f;
        dl->AddLine(ImVec2(o.x + c.pad, y1), ImVec2(o.x + c.rowW - c.pad, y1), line);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(c.rowW, h));
}

bool SkillListPickAdd(const char* bin, int job, const int* ids, uint32_t n, int* outId) {
    if (outId) *outId = 0;
    if (!outId || job <= 0) return false;
    int catalog[xcat::kAutoSkillOrderMax]{};
    uint32_t nc = 0;
    xcat::AutoSkillFillDefaultOrder(bin, job, catalog, &nc, nullptr);
    int missing[xcat::kAutoSkillOrderMax]{};
    uint32_t nm = 0;
    for (uint32_t i = 0; i < nc; ++i) {
        bool have = false;
        for (uint32_t k = 0; k < n; ++k) {
            if (ids[k] == catalog[i]) {
                have = true;
                break;
            }
        }
        if (!have && nm < xcat::kAutoSkillOrderMax) missing[nm++] = catalog[i];
    }
    if (nm == 0) return false;
    if (n >= xcat::kAutoSkillOrderMax) {
        ImGui::TextDisabled("列表已满");
        return false;
    }
    ImGui::SetNextItemWidth(-1.f);
    bool picked = false;
    if (ImGui::BeginCombo("##add", "添加技能…")) {
        for (uint32_t i = 0; i < nm; ++i) {
            const char* name = xcat::AutoSkillSkillName(bin, missing[i]);
            char row[96]{};
            if (name && name[0]) {
                snprintf(row, sizeof(row), "%s", name);
            } else {
                snprintf(row, sizeof(row), "%d", missing[i]);
            }
            if (ImGui::Selectable(row)) {
                *outId = missing[i];
                picked = true;
            }
        }
        ImGui::EndCombo();
    }
    return picked;
}

void DrawAutoSkillCard(LaunchUiState& ui) {
    static bool enabled = false;
    static bool en1 = false;
    static bool en2 = false;
    static int job1 = 0;
    static int job2 = 0;
    static int order1[xcat::kAutoSkillOrderMax]{};
    static int order2[xcat::kAutoSkillOrderMax]{};
    static int tgt1[xcat::kAutoSkillOrderMax]{};
    static int tgt2[xcat::kAutoSkillOrderMax]{};
    static uint32_t n1 = 0;
    static uint32_t n2 = 0;
    static uint64_t tick = 0;
    static bool loaded = false;
    static std::string s_loadedBin;
    static bool s_saveFailed = false;
    static uint64_t s_lastPollMs = 0;
    static bool s_loggedShow = false;
    static uint32_t s_pendingFillMask = 0;  // bit0=job1  bit1=job2；Combo 关闭后再填表写盘
    static bool s_pendingPersist = false;
    static bool s_needStub = true;
    static bool s_orderDirty = false;
    static SkillListAnim s_anim1{};
    static SkillListAnim s_anim2{};

    auto copyRows = [](int* dId, int* dT, uint32_t& dn, const int* sId, const int* sT, uint32_t sn) {
        dn = 0;
        for (uint32_t i = 0; i < xcat::kAutoSkillOrderMax; ++i) {
            dId[i] = 0;
            dT[i] = 0;
        }
        for (uint32_t i = 0; i < sn && i < xcat::kAutoSkillOrderMax; ++i) {
            if (sId[i] <= 0) continue;
            dId[dn] = sId[i];
            int t = sT[i];
            if (t < 0) t = 0;
            if (t > xcat::kAutoSkillTargetMax) t = xcat::kAutoSkillTargetMax;
            dT[dn] = t;
            ++dn;
        }
    };

    auto loadUi = [&]() {
        enabled = false;
        en1 = en2 = false;
        job1 = job2 = 0;
        n1 = n2 = 0;
        tick = 0;
        for (uint32_t i = 0; i < xcat::kAutoSkillOrderMax; ++i) {
            order1[i] = 0;
            order2[i] = 0;
            tgt1[i] = 0;
            tgt2[i] = 0;
        }
        s_anim1 = {};
        s_anim2 = {};
        if (ui.prefsBinDir.empty()) {
            loaded = true;
            return;
        }
        xcat::AutoSkillConfig cfg{};
        if (xcat::ReadAutoSkill(ui.prefsBinDir.c_str(), cfg)) {
            enabled = cfg.enabled != 0;
            en1 = cfg.job1Enabled != 0;
            en2 = cfg.job2Enabled != 0;
            job1 = cfg.job1;
            job2 = cfg.job2;
            copyRows(order1, tgt1, n1, cfg.job1Order, cfg.job1Target, cfg.job1Count);
            copyRows(order2, tgt2, n2, cfg.job2Order, cfg.job2Target, cfg.job2Count);
            tick = cfg.writeTickMs;
        }
        loaded = true;
    };

    auto persist = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::AutoSkillConfig cfg{};
        cfg.enabled = enabled ? 1u : 0u;
        cfg.job1Enabled = en1 ? 1u : 0u;
        cfg.job2Enabled = en2 ? 1u : 0u;
        cfg.job1 = job1;
        cfg.job2 = job2;
        copyRows(cfg.job1Order, cfg.job1Target, cfg.job1Count, order1, tgt1, n1);
        copyRows(cfg.job2Order, cfg.job2Target, cfg.job2Count, order2, tgt2, n2);
        xcat::AutoSkillNormalize(cfg);
        enabled = cfg.enabled != 0;
        en1 = cfg.job1Enabled != 0;
        en2 = cfg.job2Enabled != 0;
        cfg.writeTickMs = GetTickCount64();
        if (xcat::WriteAutoSkill(ui.prefsBinDir.c_str(), cfg, 0u)) {
            tick = cfg.writeTickMs;
            s_saveFailed = false;
            s_pendingPersist = false;
            xcat::log::Ok("App", "已下发 auto_skill：开=%d j1=%d/%d j2=%d/%d n1=%u n2=%u",
                          cfg.enabled ? 1 : 0, cfg.job1, cfg.job1Enabled ? 1 : 0, cfg.job2,
                          cfg.job2Enabled ? 1 : 0, cfg.job1Count, cfg.job2Count);
        } else {
            s_pendingPersist = true;
        }
    };

    if (ui.prefsBinDir.empty()) {
        ImGui::TextUnformatted("自动加技能点");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 user.ini [auto_skill]。");
        return;
    }

    // 进 Tab 第一帧只出壳、立刻泵消息。读 ini / 解析 TSV / CardGuard 拉丝都放到下一帧，
    // 否则 Debug 下 ChannelsSplit+fillRemaining 或抢 user.ini 锁会让窗口直接「未响应」。
    if (s_needStub) {
        s_needStub = false;
        xcat::log::Ok("App", "auto_skill tab enter");
        ImGui::TextUnformatted("自动加技能点");
        ImGui::TextDisabled("正在读取配置…");
        return;
    }

    xcat::AutoSkillWarmCatalogAsync(ui.prefsBinDir.c_str());

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadUi();
        s_saveFailed = false;
        s_lastPollMs = GetTickCount64();
    } else if (!loaded) {
        loadUi();
        s_lastPollMs = GetTickCount64();
    } else if (s_pendingFillMask == 0 && !s_pendingPersist && !s_orderDirty &&
               s_anim1.dragId == 0 && s_anim2.dragId == 0 && s_anim1.lift <= 0.02f &&
               s_anim2.lift <= 0.02f && !ImGui::IsAnyItemActive() &&
               ImGui::GetDragDropPayload() == nullptr && !s_saveFailed) {
        // 禁止每帧 LoadIniFile：换票/注入时 payload 会占 user.ini 锁，UI 线程最多干等 5s/次。
        // 有 pending 填表/写盘时不要先 loadUi，否则会把刚选的职业冲掉。
        const uint64_t now = GetTickCount64();
        if (now - s_lastPollMs >= 400ull) {
            s_lastPollMs = now;
            xcat::AutoSkillConfig disk{};
            if (xcat::ReadAutoSkill(ui.prefsBinDir.c_str(), disk) && disk.writeTickMs != tick) {
                loadUi();
            }
        }
    }

    if (s_pendingFillMask) {
        const uint32_t mask = s_pendingFillMask;
        s_pendingFillMask = 0;
        const char* fillBin = ui.prefsBinDir.c_str();
        xcat::log::Ok("App", "auto_skill fill start job1=%d job2=%d mask=%u", job1, job2, mask);
        if (mask & 1u) {
            if (job1 > 0) {
                (void)xcat::AutoSkillFillDefaultOrder(fillBin, job1, order1, &n1, tgt1);
                s_anim1 = {};
            } else {
                n1 = 0;
                for (uint32_t i = 0; i < xcat::kAutoSkillOrderMax; ++i) {
                    order1[i] = 0;
                    tgt1[i] = 0;
                }
                s_anim1 = {};
                en1 = false;
            }
        }
        if (mask & 2u) {
            if (job2 > 0) {
                (void)xcat::AutoSkillFillDefaultOrder(fillBin, job2, order2, &n2, tgt2);
                s_anim2 = {};
            } else {
                n2 = 0;
                for (uint32_t i = 0; i < xcat::kAutoSkillOrderMax; ++i) {
                    order2[i] = 0;
                    tgt2[i] = 0;
                }
                s_anim2 = {};
                en2 = false;
            }
        }
        persist();
        xcat::log::Ok("App", "auto_skill fill done n1=%u n2=%u", n1, n2);
    }

    if (!s_loggedShow) {
        s_loggedShow = true;
        xcat::log::Ok("App", "auto_skill card shown job1=%d job2=%d n1=%u n2=%u", job1, job2, n1,
                      n2);
    }

    const char* bin = ui.prefsBinDir.c_str();

    auto drawJobCombo = [&](const char* label, const char* id, int* job, const int* opts,
                            uint32_t nOpts, bool isJob1, const char* restoreId, uint32_t fillBit) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        char preview[96]{};
        if (*job <= 0) {
            snprintf(preview, sizeof(preview), isJob1 ? "选择职业" : "不选");
        } else {
            const char* name = xcat::AutoSkillJobLabel(bin, *job);
            if (name && name[0]) {
                snprintf(preview, sizeof(preview), "%d  %s", *job, name);
            } else {
                snprintf(preview, sizeof(preview), "%d", *job);
            }
        }
        const float restoreW =
            restoreId ? (ImGui::CalcTextSize("恢复默认").x + ImGui::GetStyle().FramePadding.x * 2.f +
                         AppDpi_Px(8.f))
                      : 0.f;
        const float gap = restoreId ? ImGui::GetStyle().ItemSpacing.x : 0.f;
        ImGui::SetNextItemWidth((std::max)(AppDpi_Px(80.f), ImGui::GetContentRegionAvail().x - restoreW - gap));
        if (ImGui::BeginCombo(id, preview)) {
            if (!isJob1) {
                const bool noneSel = (*job <= 0);
                if (ImGui::Selectable("不选（不花 2 转技能点）", noneSel)) {
                    if (*job != 0) {
                        *job = 0;
                        en2 = false;
                        s_pendingFillMask |= 2u;
                    }
                }
                if (noneSel) ImGui::SetItemDefaultFocus();
            }
            for (uint32_t i = 0; i < nOpts; ++i) {
                const int j = opts[i];
                const char* name = xcat::AutoSkillJobLabel(bin, j);
                char row[96]{};
                if (name && name[0]) {
                    snprintf(row, sizeof(row), "%d  %s", j, name);
                } else {
                    snprintf(row, sizeof(row), "%d", j);
                }
                const bool sel = (*job == j);
                if (ImGui::Selectable(row, sel)) {
                    if (*job != j) {
                        *job = j;
                        if (isJob1) {
                            int j2opts[16]{};
                            uint32_t nj2sel = 0;
                            xcat::AutoSkillListJob2(job1, j2opts, &nj2sel);
                            bool keep = false;
                            for (uint32_t k = 0; k < nj2sel; ++k) {
                                if (j2opts[k] == job2) {
                                    keep = true;
                                    break;
                                }
                            }
                            if (!keep) {
                                job2 = 0;
                                s_pendingFillMask |= 1u | 2u;
                            } else {
                                s_pendingFillMask |= 1u;
                            }
                        } else {
                            if (job1 <= 0) {
                                job1 = xcat::AutoSkillJob1OfJob2(j);
                                s_pendingFillMask |= 1u | 2u;
                            } else {
                                s_pendingFillMask |= 2u;
                            }
                        }
                    }
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (restoreId) {
            ImGui::SameLine();
            ImGui::BeginDisabled(*job <= 0);
            if (ImGui::SmallButton(restoreId)) s_pendingFillMask |= fillBit;
            ImGui::EndDisabled();
        }
    };

    auto removeRow = [](int* ids, int* tgt, float* ys, float* vys, uint32_t& n, uint32_t i) {
        if (i >= n) return;
        for (uint32_t k = i; k + 1 < n; ++k) {
            ids[k] = ids[k + 1];
            tgt[k] = tgt[k + 1];
            if (ys) ys[k] = ys[k + 1];
            if (vys) vys[k] = vys[k + 1];
        }
        ids[n - 1] = 0;
        tgt[n - 1] = 0;
        if (ys) ys[n - 1] = 0.f;
        if (vys) vys[n - 1] = 0.f;
        --n;
    };

    auto drawOrder = [&](const char* listId, int* ids, int* tgt, uint32_t& n, SkillListAnim& anim,
                         int job, uint32_t fillBit) {
        ImGui::PushID(listId);
        auto appendSkill = [&](int skillId) {
            if (skillId <= 0 || n >= xcat::kAutoSkillOrderMax) return;
            for (uint32_t k = 0; k < n; ++k) {
                if (ids[k] == skillId) return;
            }
            ids[n] = skillId;
            tgt[n] = 0;
            ++n;
            s_pendingPersist = true;
        };
        if (n == 0) {
            anim = {};
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (job <= 0) {
                if (fillBit == 2u) {
                    ImGui::TextWrapped("未选 2 转：不会动 2 转技能点。需要时再选职业。");
                } else {
                    ImGui::TextWrapped("先在上面选 1 转职业。");
                }
            } else {
                ImGui::TextWrapped("还没有技能。可填入该书默认顺序，或从下面挑着加。");
            }
            ImGui::PopStyleColor();
            if (job > 0) {
                if (ImGui::Button("填入默认")) s_pendingFillMask |= fillBit;
                int addId = 0;
                if (SkillListPickAdd(bin, job, ids, n, &addId)) appendSkill(addId);
            }
            ImGui::PopID();
            return;
        }
        const float rowH = ImGui::GetFrameHeightWithSpacing();
        const float avail = ImGui::GetContentRegionAvail().x;
        const SkillListCols col = MakeSkillListCols(avail);

        bool same = anim.n == n;
        if (same) {
            for (uint32_t i = 0; i < n; ++i) {
                if (anim.idAt[i] != ids[i]) {
                    same = false;
                    break;
                }
            }
        }
        if (!same) SkillListAnimSnap(anim, ids, n, rowH);

        int dragFrom = -1;
        if (anim.dragId != 0) {
            for (uint32_t i = 0; i < n; ++i) {
                if (ids[i] == anim.dragId) {
                    dragFrom = static_cast<int>(i);
                    break;
                }
            }
            if (dragFrom < 0) anim.dragId = 0;
        }

        if (dragFrom >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            int insertAtDrop = anim.insertAt;
            if (insertAtDrop < 0) insertAtDrop = 0;
            if (insertAtDrop >= static_cast<int>(n)) insertAtDrop = static_cast<int>(n) - 1;
            if (insertAtDrop != dragFrom) {
                SkillListReorder(ids, tgt, anim.y, anim.vy, n, dragFrom, insertAtDrop);
                s_orderDirty = true;
            }
            anim.dragId = 0;
            dragFrom = -1;
            for (uint32_t i = 0; i < n; ++i) anim.idAt[i] = ids[i];
            anim.n = n;
        }

        float dt = ImGui::GetIO().DeltaTime;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.033f) dt = 0.033f;

        const ImVec2 mouse = ImGui::GetMousePos();

        SkillListHeader(col);

        const ImVec2 bodyOrigin = ImGui::GetCursorScreenPos();
        const float lo = 0.f;
        const float hiY = static_cast<float>(n - 1) * rowH;
        if (dragFrom >= 0) {
            const float raw = mouse.y - bodyOrigin.y - anim.grabOff;
            anim.y[dragFrom] = SkillListRubber(raw, lo, hiY);
            anim.vy[dragFrom] = 0.f;
        }
        const int insertAt = SkillListInsertAt(anim, dragFrom, n, rowH);
        const float liftTarget = (dragFrom >= 0) ? 1.f : 0.f;
        anim.lift += (liftTarget - anim.lift) * (1.f - expf(-18.f * dt));
        if (fabsf(liftTarget - anim.lift) < 0.003f) anim.lift = liftTarget;
        if (anim.lift <= 0.003f) anim.liftId = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (static_cast<int>(i) == dragFrom) continue;
            SkillListSpring(anim.y[i], anim.vy[i], SkillListTargetY(i, dragFrom, insertAt, rowH),
                            dt);
        }

        ImGui::Dummy(ImVec2(avail, static_cast<float>(n) * rowH));
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const float rnd = AppDpi_Px(4.f);
        uint32_t removeAt = n;

        auto drawRow = [&](uint32_t i) {
            const float y = bodyOrigin.y + anim.y[i];
            ImGui::SetCursorScreenPos(ImVec2(bodyOrigin.x, y));
            ImGui::PushID(ids[i]);
            const bool lifted = (ids[i] == anim.liftId && anim.lift > 0.02f) ||
                                (static_cast<int>(i) == dragFrom);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float innerH = ImGui::GetFrameHeight();
            const float expand = anim.lift * AppDpi_Px(lifted ? 3.f : 0.f);
            ImVec2 rmin(bodyOrigin.x - expand, y - expand * 0.35f);
            ImVec2 rmax(bodyOrigin.x + avail + expand, y + innerH + expand * 0.55f);
            if (lifted) {
                const float sh = anim.lift;
                for (int s = 3; s >= 1; --s) {
                    const float o = sh * static_cast<float>(s) * AppDpi_Px(2.2f);
                    const int alpha = static_cast<int>(14.f * static_cast<float>(s) * sh);
                    dl->AddRectFilled(ImVec2(rmin.x, rmin.y + o), ImVec2(rmax.x, rmax.y + o),
                                      IM_COL32(0, 0, 0, alpha), rnd);
                }
                dl->AddRectFilled(rmin, rmax, ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.55f + 0.4f * sh),
                                  rnd);
                dl->AddRect(rmin, rmax, ImGui::GetColorU32(ImGuiCol_HeaderActive), rnd, 0,
                            AppDpi_Px(1.2f + 0.8f * sh));
            } else {
                const bool rowHot =
                    dragFrom < 0 &&
                    ImGui::IsMouseHoveringRect(ImVec2(bodyOrigin.x, y),
                                               ImVec2(bodyOrigin.x + avail, y + innerH), false);
                const ImU32 bg = ImGui::GetColorU32(
                    rowHot ? ImGuiCol_HeaderHovered
                           : ((i % 2u) ? ImGuiCol_TableRowBgAlt : ImGuiCol_TableRowBg),
                    rowHot ? 1.f : (1.f - 0.2f * anim.lift));
                dl->AddRectFilled(ImVec2(bodyOrigin.x, y),
                                  ImVec2(bodyOrigin.x + avail, y + innerH), bg);
            }

            const char* name = xcat::AutoSkillSkillName(bin, ids[i]);
            char nameBuf[96]{};
            if (name && name[0]) {
                snprintf(nameBuf, sizeof(nameBuf), "%s", name);
            } else {
                snprintf(nameBuf, sizeof(nameBuf), "%d", ids[i]);
            }
            const int mx = xcat::AutoSkillMaxObserved(bin, ids[i]);
            const int hiLv = mx > 0 ? mx : xcat::kAutoSkillTargetMax;
            const bool followMax = tgt[i] <= 0;

            auto tryBeginDrag = [&]() {
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }
                if (anim.dragId != 0) return;
                if (!ImGui::IsItemActive()) return;
                if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, AppDpi_Px(2.f))) return;
                anim.dragId = ids[i];
                anim.liftId = ids[i];
                anim.insertAt = static_cast<int>(i);
                anim.grabOff = ImGui::GetMousePos().y - (bodyOrigin.y + anim.y[i]);
                if (anim.lift < 0.4f) anim.lift = 0.4f;
            };

            auto at = [&](float x) {
                ImGui::SetCursorScreenPos(ImVec2(bodyOrigin.x + x, y));
            };

            at(col.xGrip);
            DrawSkillDragGrip(ImVec2(col.wGrip, ImGui::GetFrameHeight()));
            tryBeginDrag();
            int rank = static_cast<int>(i) + 1;
            if (dragFrom >= 0) {
                if (static_cast<int>(i) == dragFrom) {
                    rank = insertAt + 1;
                } else {
                    rank = static_cast<int>(SkillListTargetY(i, dragFrom, insertAt, 1.f) + 0.1f) + 1;
                }
            }
            at(col.xIdx);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d", rank);
            at(col.xName);
            {
                const ImVec2 np = ImGui::GetCursorScreenPos();
                ImGui::PushClipRect(np, ImVec2(np.x + col.wName, np.y + innerH), true);
                ImGui::Selectable(nameBuf, lifted, 0, ImVec2(col.wName, 0.f));
                ImGui::PopClipRect();
            }
            tryBeginDrag();
            const bool freeze = anim.dragId != 0;
            if (freeze) ImGui::BeginDisabled();
            int shown = followMax ? hiLv : tgt[i];
            if (shown < 1) shown = 1;
            if (shown > hiLv) shown = hiLv;
            at(col.xTgt);
            ImGui::SetNextItemWidth(col.wTgt);
            if (xcat::ui::DragIntClamped("##tgt", &shown, 1, hiLv, "%d")) {
                tgt[i] = shown;
                s_pendingPersist = true;
            }
            at(col.xMax);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("/%d", hiLv);
            at(col.xDel);
            if (ImGui::SmallButton("x")) removeAt = i;
            if (freeze) ImGui::EndDisabled();
            ImGui::PopID();
        };

        const ImVec2 clipMax(bodyOrigin.x + avail, bodyOrigin.y + static_cast<float>(n) * rowH);
        int liftIndex = -1;
        ImGui::PushClipRect(bodyOrigin, clipMax, true);
        for (uint32_t i = 0; i < n; ++i) {
            if (ids[i] == anim.liftId || static_cast<int>(i) == dragFrom) {
                liftIndex = static_cast<int>(i);
                continue;
            }
            drawRow(i);
        }
        ImGui::PopClipRect();
        if (liftIndex >= 0) drawRow(static_cast<uint32_t>(liftIndex));

        ImGui::SetCursorScreenPos(after);
        if (removeAt < n) {
            removeRow(ids, tgt, anim.y, anim.vy, n, removeAt);
            anim.dragId = 0;
            anim.n = n;
            for (uint32_t i = 0; i < xcat::kAutoSkillOrderMax; ++i) {
                anim.idAt[i] = (i < n) ? ids[i] : 0;
            }
            s_pendingPersist = true;
        }
        ImGui::Dummy(ImVec2(0.f, AppDpi_Px(4.f)));
        if (anim.dragId == 0) {
            int addId = 0;
            if (SkillListPickAdd(bin, job, ids, n, &addId)) appendSkill(addId);
        }
        ImGui::PopID();
    };

    auto drawEnableRow = [&](const char* id, bool* on, bool canOn, const char* idleHint) {
        if (!canOn && *on) {
            *on = false;
            s_pendingPersist = true;
        }
        ImGui::BeginDisabled(!canOn);
        if (xcat::ui::OptionCheckbox(id, on)) persist();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!canOn) {
            ImGui::TextDisabled("%s", idleHint);
        } else if (*on) {
            ImGui::TextDisabled("已开");
        } else {
            ImGui::TextDisabled("已关");
        }
    };

    int j1[8]{};
    uint32_t nj1 = 0;
    xcat::AutoSkillListJob1(j1, &nj1);
    int j2[16]{};
    uint32_t nj2 = 0;
    xcat::AutoSkillListJob2(job1, j2, &nj2);

    {
        xcat::ui::CardGuard card("##tab_auto_skill", "自动加技能点");
        if (xcat::ui::OptionCheckbox("自动加技能点##ask_master", &enabled)) persist();
        const bool masterHover = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::SameLine();
        if (!enabled) {
            ImGui::TextDisabled("已关，可先选职业和顺序");
        } else if (!en1 && !en2) {
            ImGui::TextDisabled("已开 · 再打开下面的 1 转 / 2 转");
        } else {
            ImGui::TextDisabled("已开");
        }
        if (masterHover) {
            ImGui::SetTooltip(
                "总闸。关掉后 1 转 / 2 转技能点都不加，勾选会留着。\n"
                "1 转 / 2 转技能点分池，互不影响。这不是属性加点。");
        }
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
        DrawPrepHintBlueWrapped("从上到下按顺序加技能点。按住左边竖条拖动，即可调整顺序。");
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(
            "初心者不加。已转职后按 1/2 转技能点分池加。冒险家五职，不含双刀，不加 3 转书。"
            "改顺序后需已注入当前 DLL。");
        ImGui::PopStyleColor();
    }

    CardGap();
    {
        char title1[80]{};
        const char* jl1 = (job1 > 0) ? xcat::AutoSkillJobLabel(bin, job1) : nullptr;
        if (jl1 && jl1[0]) {
            snprintf(title1, sizeof(title1), "1 转 · %s", jl1);
        } else {
            snprintf(title1, sizeof(title1), "1 转");
        }
        xcat::ui::CardGuard card("##tab_auto_skill_j1", title1);
        drawEnableRow("1 转技能点##ask_j1", &en1, job1 > 0 && n1 > 0, "先选职业");
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.25f));
        drawJobCombo("职业", "##ask_job1", &job1, j1, nj1, true, "恢复默认##j1", 1u);
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
        drawOrder("j1", order1, tgt1, n1, s_anim1, job1, 1u);
    }

    CardGap();
    {
        char title2[80]{};
        const char* jl2 = (job2 > 0) ? xcat::AutoSkillJobLabel(bin, job2) : nullptr;
        if (jl2 && jl2[0]) {
            snprintf(title2, sizeof(title2), "2 转 · %s", jl2);
        } else {
            snprintf(title2, sizeof(title2), "2 转");
        }
        xcat::ui::CardGuard card("##tab_auto_skill_j2", title2);
        drawEnableRow("2 转技能点##ask_j2", &en2, job2 > 0 && n2 > 0,
                      nj2 == 0 ? "先选 1 转" : "先选职业");
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.25f));
        if (nj2 == 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("职业");
            ImGui::SameLine();
            ImGui::BeginDisabled(true);
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##ask_job2", "先选 1 转")) ImGui::EndCombo();
            ImGui::EndDisabled();
        } else {
            drawJobCombo("职业", "##ask_job2", &job2, j2, nj2, false, "恢复默认##j2", 2u);
        }
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
        drawOrder("j2", order2, tgt2, n2, s_anim2, job2, 2u);
    }

    if (s_orderDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_pendingPersist = true;
        s_orderDirty = false;
    }
    if (s_pendingPersist) persist();

    if (s_saveFailed) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [auto_skill] 失败");
    }
}

void DrawAutoStatTab(LaunchUiState& ui) {
    static bool enabled = xcat::kAutoStatDefaultEnabled != 0;
    static int str = 0;
    static int dex = 0;
    static int intel = 0;
    static int luk = 0;
    static uint64_t tick = 0;
    static bool loaded = false;
    static std::string s_loadedBin;
    static bool s_saveFailed = false;
    static uint64_t s_lastPollMs = 0;

    auto loadUi = [&]() {
        enabled = xcat::kAutoStatDefaultEnabled != 0;
        str = dex = intel = luk = 0;
        tick = 0;
        if (ui.prefsBinDir.empty()) {
            loaded = true;
            return;
        }
        xcat::AutoStatConfig ast{};
        if (xcat::ReadAutoStat(ui.prefsBinDir.c_str(), ast)) {
            enabled = ast.enabled != 0;
            str = static_cast<int>(ast.str);
            dex = static_cast<int>(ast.dex);
            intel = static_cast<int>(ast.intel);
            luk = static_cast<int>(ast.luk);
            tick = ast.writeTickMs;
        }
        loaded = true;
    };

    auto persist = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::AutoStatConfig cfg{};
        (void)xcat::ReadAutoStat(ui.prefsBinDir.c_str(), cfg);
        cfg.enabled = enabled ? 1u : 0u;
        cfg.str = str < 0 ? 0u : static_cast<uint32_t>(str);
        cfg.dex = dex < 0 ? 0u : static_cast<uint32_t>(dex);
        cfg.intel = intel < 0 ? 0u : static_cast<uint32_t>(intel);
        cfg.luk = luk < 0 ? 0u : static_cast<uint32_t>(luk);
        xcat::AutoStatNormalize(cfg);
        cfg.writeTickMs = GetTickCount64();
        if (xcat::WriteAutoStat(ui.prefsBinDir.c_str(), cfg)) {
            tick = cfg.writeTickMs;
            s_saveFailed = false;
            xcat::log::Ok("App", "已下发 auto_stat：开=%d 力量=%u 敏捷=%u 智力=%u 幸运=%u",
                          cfg.enabled ? 1 : 0, cfg.str, cfg.dex, cfg.intel, cfg.luk);
        } else {
            s_saveFailed = true;
            xcat::log::Warn("App", "写入 user.ini [auto_stat] 失败");
        }
    };

    if (ui.prefsBinDir.empty()) {
        xcat::ui::CardGuard card("##tab_auto_stat", "自动加点");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 user.ini [auto_stat]。");
        return;
    }

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadUi();
        s_saveFailed = false;
        s_lastPollMs = GetTickCount64();
    } else if (!loaded) {
        loadUi();
        s_lastPollMs = GetTickCount64();
    } else if (!ImGui::IsAnyItemActive() && !s_saveFailed) {
        const uint64_t now = GetTickCount64();
        if (now - s_lastPollMs >= 400ull) {
            s_lastPollMs = now;
            xcat::AutoStatConfig disk{};
            if (xcat::ReadAutoStat(ui.prefsBinDir.c_str(), disk) && disk.writeTickMs != tick) {
                loadUi();
            }
        }
    }

    {
        xcat::ui::CardGuard card("##tab_auto_stat", "自动加点");
        const int ratioSum = str + dex + intel + luk;
        const bool canEnable = ratioSum == static_cast<int>(xcat::kAutoStatRatioSum);
        DrawPrepHintBlue("先分配好点数才允许勾选！！！");
        ImGui::BeginDisabled(!enabled && !canEnable);
        if (xcat::ui::OptionCheckbox("自动加点", &enabled)) persist();
        ImGui::EndDisabled();
        const bool autoStatHover = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::SameLine();
        if (canEnable) {
            ImGui::TextDisabled("配比 %d/%u", ratioSum, xcat::kAutoStatRatioSum);
        } else {
            ImGui::Text("配比 %d/%u", ratioSum, xcat::kAutoStatRatioSum);
        }
        if (autoStatHover) {
            ImGui::SetTooltip(
                "默认关闭。勾上后把身上剩余 AP 加完（不限 5 点）。\n"
                "从勾上那一刻起，每 5 点按配比分，不追身上已有属性。\n"
                "力量/敏捷/智力/幸运 合计必须为 5。关闭时不读属性、不发包。");
        }

        auto clampStat = [&](int* v, int others) {
            const int maxThis = static_cast<int>(xcat::kAutoStatRatioSum) - others;
            if (*v < 0) *v = 0;
            if (*v > maxThis) *v = maxThis < 0 ? 0 : maxThis;
        };
        auto drawStat = [&](const char* label, const char* id, int* v, int others) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(AppDpi_Px(40.f));
            const int maxThis = (std::max)(0, static_cast<int>(xcat::kAutoStatRatioSum) - others);
            if (xcat::ui::DragIntClamped(id, v, 0, maxThis)) {
                clampStat(v, others);
                persist();
            }
        };
        drawStat("力量", "##asStr", &str, dex + intel + luk);
        ImGui::SameLine(0.f, ui::Gap());
        drawStat("敏捷", "##asDex", &dex, str + intel + luk);
        ImGui::SameLine(0.f, ui::Gap());
        drawStat("智力", "##asInt", &intel, str + dex + luk);
        ImGui::SameLine(0.f, ui::Gap());
        drawStat("幸运", "##asLuk", &luk, str + dex + intel);

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.4f));
        ImGui::TextDisabled(
            "仅 1 转后生效，0 转不加。从开启起每 5 点按配比分，不追已有属性。");
    }

    if (s_saveFailed) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [auto_stat] 失败");
    }
}

void DrawCharBootTab(LaunchUiState& ui) {
    static int farmMap = static_cast<int>(xcat::kCharBootDefaultFarmMap);
    static int hangupMap = static_cast<int>(xcat::kCharBootDefaultHangupMap);
    static int departKind = static_cast<int>(xcat::kCharBootDepartLevel);
    static int levelMin = static_cast<int>(xcat::kCharBootDefaultLevelMin);
    static int mesoMin = static_cast<int>(xcat::kCharBootDefaultMesoMin);
    static bool autoCreateChar = false;
    static uint64_t tick = 0;
    static bool loaded = false;
    static std::string s_loadedBin;
    static bool s_saveFailed = false;
    static uint64_t s_lastPollMs = 0;
    static xcat::CharBootStatus s_st{};
    static std::string s_farmName;
    static std::string s_hangupName;

    auto loadUi = [&]() {
        farmMap = static_cast<int>(xcat::kCharBootDefaultFarmMap);
        hangupMap = static_cast<int>(xcat::kCharBootDefaultHangupMap);
        departKind = static_cast<int>(xcat::kCharBootDepartLevel);
        levelMin = static_cast<int>(xcat::kCharBootDefaultLevelMin);
        mesoMin = static_cast<int>(xcat::kCharBootDefaultMesoMin);
        autoCreateChar = false;
        tick = 0;
        if (ui.prefsBinDir.empty()) {
            loaded = true;
            return;
        }
        xcat::CharBootConfig cfg{};
        if (xcat::ReadCharBoot(ui.prefsBinDir.c_str(), cfg)) {
            farmMap = static_cast<int>(cfg.farmMap);
            hangupMap = static_cast<int>(cfg.hangupMap);
            departKind = static_cast<int>(cfg.departKind);
            levelMin = static_cast<int>(cfg.levelMin);
            mesoMin = static_cast<int>(cfg.mesoMin);
            autoCreateChar = cfg.autoCreateChar != 0;
            tick = cfg.writeTickMs;
        }
        (void)xcat::ReadCharBootStatus(ui.prefsBinDir.c_str(), s_st);
        loaded = true;
    };

    auto persist = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::CharBootConfig cfg{};
        (void)xcat::ReadCharBoot(ui.prefsBinDir.c_str(), cfg);
        cfg.farmMap = farmMap > 0 ? static_cast<uint32_t>(farmMap) : xcat::kCharBootDefaultFarmMap;
        cfg.hangupMap =
            hangupMap > 0 ? static_cast<uint32_t>(hangupMap) : xcat::kCharBootDefaultHangupMap;
        cfg.departKind = departKind == static_cast<int>(xcat::kCharBootDepartMeso)
                             ? xcat::kCharBootDepartMeso
                             : xcat::kCharBootDepartLevel;
        cfg.levelMin = levelMin < 0 ? 0u : static_cast<uint32_t>(levelMin);
        cfg.mesoMin = mesoMin < 0 ? 0u : static_cast<uint32_t>(mesoMin);
        cfg.requireInt20 = 0;
        cfg.autoCreateChar = autoCreateChar ? 1u : 0u;
        xcat::CharBootNormalize(cfg);
        cfg.writeTickMs = GetTickCount64();
        if (xcat::WriteCharBoot(ui.prefsBinDir.c_str(), cfg)) {
            tick = cfg.writeTickMs;
            farmMap = static_cast<int>(cfg.farmMap);
            hangupMap = static_cast<int>(cfg.hangupMap);
            levelMin = static_cast<int>(cfg.levelMin);
            mesoMin = static_cast<int>(cfg.mesoMin);
            s_saveFailed = false;
        } else {
            s_saveFailed = true;
        }
    };

    auto writeManual = [&](uint32_t kind) -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::CharBootConfig cfg{};
        (void)xcat::ReadCharBoot(ui.prefsBinDir.c_str(), cfg);
        cfg.farmMap = farmMap > 0 ? static_cast<uint32_t>(farmMap) : cfg.farmMap;
        cfg.hangupMap = hangupMap > 0 ? static_cast<uint32_t>(hangupMap) : cfg.hangupMap;
        cfg.departKind = departKind == static_cast<int>(xcat::kCharBootDepartMeso)
                             ? xcat::kCharBootDepartMeso
                             : xcat::kCharBootDepartLevel;
        cfg.levelMin = levelMin < 0 ? 0u : static_cast<uint32_t>(levelMin);
        cfg.mesoMin = mesoMin < 0 ? 0u : static_cast<uint32_t>(mesoMin);
        cfg.requireInt20 = 0;
        cfg.autoCreateChar = autoCreateChar ? 1u : 0u;
        cfg.manualKind = kind;
        cfg.manualSeq = cfg.manualSeq == 0 ? 1u : cfg.manualSeq + 1u;
        xcat::CharBootNormalize(cfg);
        cfg.writeTickMs = GetTickCount64();
        if (!xcat::WriteCharBoot(ui.prefsBinDir.c_str(), cfg)) {
            notify::PushLocal(/*Warning*/ 2, "char-boot-cmd", "一键起号失败", "写入命令失败",
                              3500);
            return false;
        }
        tick = cfg.writeTickMs;
        return true;
    };

    auto fillCurrentMap = [&](int* dest, const char* id) {
        (void)id;
        xcat::PayloadStatus st{};
        if (ui.prefsBinDir.empty() || !xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) ||
            !xcat::PayloadStatusHeartbeatFresh(st, GetTickCount64(), 5000) || st.playReady == 0 ||
            st.mapId == 0) {
            notify::PushLocal(/*Warning*/ 2, "char-boot-map", "未进图",
                              "心跳不新鲜或未进图，不能填入当前图", 3500);
            return;
        }
        *dest = static_cast<int>(st.mapId);
        persist();
    };

    if (ui.prefsBinDir.empty()) {
        xcat::ui::CardGuard card("##tab_char_boot", "一键起号");
        ImGui::TextWrapped("未定位 XCat_data，无法读写 user.ini [char_boot]。");
        return;
    }

    if (s_loadedBin != ui.prefsBinDir) {
        s_loadedBin = ui.prefsBinDir;
        loadUi();
        s_saveFailed = false;
        s_lastPollMs = GetTickCount64();
    } else if (!loaded) {
        loadUi();
        s_lastPollMs = GetTickCount64();
    } else if (!ImGui::IsAnyItemActive() && !s_saveFailed) {
        const uint64_t now = GetTickCount64();
        if (now - s_lastPollMs >= 400ull) {
            s_lastPollMs = now;
            xcat::CharBootConfig disk{};
            if (xcat::ReadCharBoot(ui.prefsBinDir.c_str(), disk) && disk.writeTickMs != tick) {
                loadUi();
            }
            (void)xcat::ReadCharBootStatus(ui.prefsBinDir.c_str(), s_st);
        }
    }

    const bool busy = xcat::CharBootStateIsBusy(s_st.state);
    xcat::PayloadControl core{};
    if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), core)) {
        xcat::PayloadControlSetDefaults(core);
    }

    {
        xcat::ui::CardGuard card("##tab_char_boot", "一键起号");
        ImGui::TextDisabled("v1 只做法师 1 转。开始后会打开无敌和自动打怪，先赶到刷级图再出刀。");
        ImGui::TextDisabled("加点建议 0/0/5/0。加技能选法师；8 级技能点要到 10 级才会加。");

        if (busy) {
            if (ImGui::Button("停止", ImVec2(AppDpi_Px(120.f), ui::BtnH()))) {
                if (writeManual(xcat::kCharBootManualStop)) {
                    strncpy_s(s_st.state, "Idle", _TRUNCATE);
                    s_st.message[0] = 0;
                    strncpy_s(s_st.lastWhy, "user_stop", _TRUNCATE);
                }
            }
        } else {
            if (ImGui::Button("开始", ImVec2(AppDpi_Px(120.f), ui::BtnH())))
                (void)writeManual(xcat::kCharBootManualStart);
        }

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.4f));
        ImGui::TextUnformatted("起号刷级图");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(110.f));
        if (NativeInputIntClamped("##cb_farm", farmMap, 1, 999999999)) persist();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", LookupFarmMapDisp(ui.prefsBinDir.c_str(),
                                                    std::to_string(farmMap).c_str(), s_farmName));
        if (farmMap == static_cast<int>(xcat::kCharBootDefaultFarmMap)) {
            ImGui::SameLine();
            ImGui::TextDisabled("嫩寶狩獵場Ⅰ");
        }
        if (ImGui::Button("填入当前图##cb_farm")) fillCurrentMap(&farmMap, "farm");

        ImGui::TextUnformatted("转职后挂机图");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(110.f));
        if (NativeInputIntClamped("##cb_hangup", hangupMap, 1, 999999999)) persist();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", LookupFarmMapDisp(ui.prefsBinDir.c_str(),
                                                    std::to_string(hangupMap).c_str(),
                                                    s_hangupName));
        if (ImGui::Button("填入当前图##cb_hangup")) fillCurrentMap(&hangupMap, "hangup");

        if (ImGui::RadioButton("等级", departKind == 0)) {
            departKind = 0;
            persist();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("金币", departKind == 1)) {
            departKind = 1;
            persist();
        }
        if (departKind == 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(AppDpi_Px(70.f));
            if (NativeInputIntClamped("##cb_lv", levelMin, static_cast<int>(xcat::kCharBootLevelMinLo),
                                      static_cast<int>(xcat::kCharBootLevelMinHi)))
                persist();
        } else {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(AppDpi_Px(90.f));
            if (NativeInputIntClamped("##cb_meso", mesoMin, static_cast<int>(xcat::kCharBootMesoMinLo),
                                      2000000000))
                persist();
        }

        if (xcat::ui::OptionCheckbox("自动创建角色", &autoCreateChar)) persist();
        ImGui::SameLine();
        ImGui::TextDisabled("仅空号、默认 1 槽、随机英文名");

        if (departKind == 1 && farmMap >= 40000 && farmMap <= 40002) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.2f, 1.f), "嫩寶几乎不掉钱");
        }
        if (hangupMap > 0 &&
            xcat::IsMapInfoTown(ui.prefsBinDir.c_str(), static_cast<int>(hangupMap))) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.2f, 1.f),
                               "挂机图像城镇，开始时会拒绝");
        }
        if (autoCreateChar && core.charSlot != 1) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.2f, 1.f),
                               "空号建成只有 1 槽，与当前 charSlot 对不上");
        }
        if (autoCreateChar && core.autoEnter == 0) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.2f, 1.f),
                               "自动进游戏未开，勾了建角也不会建");
        }

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.4f));
        ImGui::TextDisabled("态 %s  lv %d  INT %d  meso %lld  map %u  ready %u",
                            s_st.state[0] ? s_st.state : "Idle", (int)s_st.level, (int)s_st.intel,
                            static_cast<long long>(s_st.meso), s_st.mapId, s_st.ready);
        if (s_st.message[0]) ImGui::TextUnformatted(s_st.message);
        if (s_st.lastWhy[0]) {
            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "上次：%s", s_st.lastWhy);
        }
    }

    if (s_saveFailed) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [char_boot] 失败");
    }
}

}  // namespace

bool TriggerManualRejoin(const std::string& prefsBinDir, bool requireInjected, std::string* outErr) {
    auto fail = [&](const char* msg) {
        if (outErr) *outErr = msg ? msg : "失败";
        return false;
    };
    if (prefsBinDir.empty()) return fail("无数据目录");
    if (requireInjected) {
        const RuntimeLeds leds = QueryRuntimeLeds(prefsBinDir.c_str());
        if (leds.gamePid == 0) return fail("未注入");
    }
    xcat::PayloadControl c{};
    if (!xcat::ReadPayloadControl(prefsBinDir.c_str(), c)) {
        xcat::PayloadControlSetDefaults(c);
    }
    c.manualRejoinSeq = c.manualRejoinSeq == 0 ? 1u : c.manualRejoinSeq + 1u;
    if (c.manualRejoinSeq == 0) c.manualRejoinSeq = 1u;
    c.writeTickMs = GetTickCount64();
    if (!xcat::WritePayloadControl(prefsBinDir.c_str(), c)) return fail("写盘失败");
    xcat::log::Ok("App", "随机换频 seq=%u", c.manualRejoinSeq);
    return true;
}

static std::string sMobGatherLoadedBin;
static uint64_t sMobGatherLastTick = 0;
static bool sMobGatherSaveFailed = false;
static bool sMobGatherAutoCombat = false;

static void MobGatherApplyDisk(const xcat::PayloadControl& c) {
    gUiMobGather = c.mobGather != 0;
    gUiMobGatherStrategy = (int)xcat::ClampMobGatherStrategy(c.mobGatherStrategy);
    gUiMobGatherLandOnArrive = c.mobGatherLandOnArrive != 0;
    gUiMobGatherHopPx = (int)xcat::ClampMobGatherHopPx(c.mobGatherHopPx);
    gUiMobGatherSpeedPct = (int)c.mobGatherSpeedPct;
    gUiMobGatherAntiJitter = c.mobGatherAntiJitter != 0;
    gUiMobGatherMax = (int)xcat::ClampMobGatherMax(
        c.mobGatherMax ? c.mobGatherMax : xcat::kMobGatherMaxDefault);
    gUiMobGatherFarInFlight = (int)xcat::ClampMobGatherFarInFlight(c.mobGatherFarInFlight);
    gUiMobGatherRadiusPx = (int)xcat::ClampMobGatherRadiusPx(
        c.mobGatherRadiusPx ? c.mobGatherRadiusPx : xcat::kMobGatherRadiusDefaultPx);
    gUiMobGatherLayerYPx = (int)xcat::ClampMobGatherLayerYPx(c.mobGatherLayerYPx);
    gUiMobGatherDyLimPx = (int)xcat::ClampMobGatherDyLimPx(c.mobGatherDyLimPx);
    gUiMobGatherWalkDx = (int)xcat::ClampMobGatherWalkDx(c.mobGatherWalkDx);
    gUiMobGatherFeetExemptPx = (int)xcat::ClampMobGatherFeetExemptPx(c.mobGatherFeetExemptPx);
    gUiMobGatherHoldMs = (int)xcat::ClampMobGatherHoldMs(
        c.mobGatherHoldMs ? c.mobGatherHoldMs : xcat::kMobGatherHoldMsDefault);
    gUiMobGatherIntervalMs = (int)xcat::ClampMobGatherIntervalMs(
        c.mobGatherIntervalMs ? c.mobGatherIntervalMs : xcat::kMobGatherIntervalDefaultMs);
    gUiMobGatherIgnoreQuiet = c.mobGatherIgnoreQuiet != 0;
    gUiMobGatherQuietDelayMs = (int)xcat::ClampMobGatherQuietDelayMs(c.mobGatherQuietDelayMs);
    gUiMobGatherStandOffCustom = c.mobGatherStandOffCustom != 0;
    gUiMobGatherStandOffX = (int)xcat::ClampMobGatherStandOffX(c.mobGatherStandOffX);
    gUiMobGatherStandOffY = (int)xcat::ClampMobGatherStandOffY(c.mobGatherStandOffY);
    gUiMobGatherAimJitter = (int)xcat::ClampMobGatherAimJitter(c.mobGatherAimJitterPx);
    gUiMobGatherStickCreep = (int)c.mobGatherStickCreepPx;
    gUiMobGatherStickStillV = (int)c.mobGatherStickStillV;
    gUiMobGatherCruiseR = (int)c.mobGatherCruiseR;
    gUiMobGatherStationR = (int)c.mobGatherStationR;
    gUiMobGatherMaxCmd = (int)c.mobGatherMaxCmd;
    gUiMobGatherKp = (int)c.mobGatherKp;
    gUiMobGatherDispClampOn = c.mobGatherDispClampOn != 0;
    gUiMobGatherDispCapPx = (int)xcat::ClampMobGatherDispCapPx(c.mobGatherDispCapPx);
    gUiMobGatherDead = (int)c.mobGatherDead;
    gUiMobGatherGravity = (int)c.mobGatherGravity;
    gUiMobGatherCruiseV = (int)c.mobGatherCruiseV;
    gUiMobGatherStationV = (int)c.mobGatherStationV;
    gUiMobGatherHoldV = (int)c.mobGatherHoldV;
    gUiMobGatherSettleErr = (int)c.mobGatherSettleErr;
    gUiMobGatherKpSettle = (int)c.mobGatherKpSettle;
    gUiMobGatherBrakeMs = (int)c.mobGatherBrakeMs;
    gUiMobGatherCoastVy = (int)c.mobGatherCoastVy;
    gUiMobGatherAimMs = (int)c.mobGatherAimMs;
    sMobGatherAutoCombat = c.simpleCombat != 0;
    gUiMobGatherSoftRelogin = c.mobGatherSoftRelogin != 0;
    gUiMobGatherSoftReloginSec = (int)xcat::ClampMobGatherSoftReloginSec(
        c.mobGatherSoftReloginSec ? c.mobGatherSoftReloginSec
                                  : xcat::kMobGatherSoftReloginSecDefault);
    gUiMobGatherHangupFires = (int)xcat::ClampMobGatherHangupFires(c.mobGatherHangupFires);
    gUiMobGatherHangupFiresOn = c.mobGatherHangupFiresOn != 0;
    gUiMobGatherHangupUnbindF5 = c.mobGatherHangupUnbindF5 != 0;
    gUiMobGatherClearRelogin = c.mobGatherClearRelogin != 0;
    gUiMobGatherSeekCluster = c.mobGatherSeekCluster != 0;
    gUiMobGatherPatrolFar = c.mobGatherPatrolFar != 0;
    gUiMobGatherAntiReport = false;
    gUiMobGatherHomeReturn = c.mobGatherHomeReturn != 0;
    gUiMobGatherHomeX = (int)xcat::ClampMobGatherStandOffX(c.mobGatherHomeX);
    gUiMobGatherHomeY = (int)xcat::ClampMobGatherStandOffY(c.mobGatherHomeY);
    gUiMobGatherHomeMapId = c.mobGatherHomeMapId;
    gUiMobGatherHomeValid = c.mobGatherHomeValid != 0;
    gUiMobGatherHomeHasMap = c.mobGatherHomeHasMap != 0;
    gUiMobGatherApplyCtrl = c.mobGatherApplyCtrl != 0;
    gUiAttackAccelClearBusy = c.attackAccelClearBusy != 0;
    gUiAttackAccelSkipPrepare = c.attackAccelSkipPrepare != 0;
    gUiAttackAccelCutLayer = c.attackAccelCutLayer != 0;
    gUiCombatTickMs = (int)xcat::ClampSimpleCombatTickMs(
        c.simpleCombatTickMs ? c.simpleCombatTickMs : xcat::kSimpleCombatTickDefaultMs);
    gUiSkipAccMissOn = c.simpleCombatSkipAccMiss != 0;
    gUiSkipAccMissN = (int)xcat::ClampCombatSkipAccMissN(
        c.simpleCombatSkipAccMissN ? c.simpleCombatSkipAccMissN
                                   : xcat::kCombatSkipAccMissNDefault);
    sMobGatherLastTick = c.writeTickMs;
}

static void MobGatherLoadFromDisk(LaunchUiState& ui) {
    if (ui.prefsBinDir.empty()) {
        sMobGatherLastTick = 0;
        return;
    }
    xcat::PayloadControl c{};
    if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
        xcat::PayloadControlSetDefaults(c);
    }
    MobGatherApplyDisk(c);
}

static bool MobGatherSaveUi(LaunchUiState& ui) {
    if (ui.prefsBinDir.empty()) return false;
    xcat::PayloadControl c{};
    (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
    c.mobGather = gUiMobGather ? 1u : 0u;
    c.mobGatherStrategy = xcat::ClampMobGatherStrategy(
        static_cast<uint32_t>(gUiMobGatherStrategy < 0 ? 0 : gUiMobGatherStrategy));
    c.mobGatherLandOnArrive = gUiMobGatherLandOnArrive ? 1u : 0u;
    c.mobGatherHopPx = xcat::ClampMobGatherHopPx(
        static_cast<uint32_t>(gUiMobGatherHopPx < 0 ? 0 : gUiMobGatherHopPx));
    c.mobGatherSpeedPct = MobGatherUiU32(gUiMobGatherSpeedPct);
    c.mobGatherAntiJitter = gUiMobGatherAntiJitter ? 1u : 0u;
    c.mobGatherMax = xcat::ClampMobGatherMax(
        static_cast<uint32_t>(gUiMobGatherMax < 0 ? 0 : gUiMobGatherMax));
    c.mobGatherFarInFlight = xcat::ClampMobGatherFarInFlight(
        static_cast<uint32_t>(gUiMobGatherFarInFlight < 0 ? 0 : gUiMobGatherFarInFlight));
    c.mobGatherRadiusPx = xcat::ClampMobGatherRadiusPx(
        static_cast<uint32_t>(gUiMobGatherRadiusPx < 0 ? 0 : gUiMobGatherRadiusPx));
    c.mobGatherLayerYPx = xcat::ClampMobGatherLayerYPx(
        static_cast<uint32_t>(gUiMobGatherLayerYPx < 0 ? 0 : gUiMobGatherLayerYPx));
    c.mobGatherDyLimPx = xcat::ClampMobGatherDyLimPx(
        static_cast<uint32_t>(gUiMobGatherDyLimPx < 0 ? 0 : gUiMobGatherDyLimPx));
    c.mobGatherWalkDx = xcat::ClampMobGatherWalkDx(
        static_cast<uint32_t>(gUiMobGatherWalkDx < 0 ? 0 : gUiMobGatherWalkDx));
    c.mobGatherFeetExemptPx = xcat::ClampMobGatherFeetExemptPx(
        static_cast<uint32_t>(gUiMobGatherFeetExemptPx < 0 ? 0 : gUiMobGatherFeetExemptPx));
    c.mobGatherHoldMs = xcat::ClampMobGatherHoldMs(
        static_cast<uint32_t>(gUiMobGatherHoldMs < 0 ? 0 : gUiMobGatherHoldMs));
    c.mobGatherIntervalMs = xcat::ClampMobGatherIntervalMs(
        static_cast<uint32_t>(gUiMobGatherIntervalMs < 0 ? 0 : gUiMobGatherIntervalMs));
    c.mobGatherIgnoreQuiet = gUiMobGatherIgnoreQuiet ? 1u : 0u;
    c.mobGatherQuietDelayMs = xcat::ClampMobGatherQuietDelayMs(
        static_cast<uint32_t>(gUiMobGatherQuietDelayMs < 0 ? 0 : gUiMobGatherQuietDelayMs));
    c.mobGatherStandOffCustom = gUiMobGatherStandOffCustom ? 1u : 0u;
    c.mobGatherStandOffX = xcat::ClampMobGatherStandOffX(gUiMobGatherStandOffX);
    c.mobGatherStandOffY = xcat::ClampMobGatherStandOffY(gUiMobGatherStandOffY);
    c.mobGatherAimJitterPx = xcat::ClampMobGatherAimJitter(
        static_cast<uint32_t>(gUiMobGatherAimJitter < 0 ? 0 : gUiMobGatherAimJitter));
    c.mobGatherStickCreepPx = MobGatherUiU32(gUiMobGatherStickCreep);
    c.mobGatherStickStillV = MobGatherUiU32(gUiMobGatherStickStillV);
    c.mobGatherCruiseR = MobGatherUiU32(gUiMobGatherCruiseR);
    c.mobGatherStationR = MobGatherUiU32(gUiMobGatherStationR);
    c.mobGatherMaxCmd = MobGatherUiU32(gUiMobGatherMaxCmd);
    c.mobGatherKp = MobGatherUiU32(gUiMobGatherKp);
    c.mobGatherDispClampOn = gUiMobGatherDispClampOn ? 1u : 0u;
    c.mobGatherDispCapPx =
        xcat::ClampMobGatherDispCapPx(MobGatherUiU32(gUiMobGatherDispCapPx));
    c.mobGatherDead = MobGatherUiU32(gUiMobGatherDead);
    c.mobGatherGravity = MobGatherUiU32(gUiMobGatherGravity);
    c.mobGatherCruiseV = MobGatherUiU32(gUiMobGatherCruiseV);
    c.mobGatherStationV = MobGatherUiU32(gUiMobGatherStationV);
    c.mobGatherHoldV = MobGatherUiU32(gUiMobGatherHoldV);
    c.mobGatherSettleErr = MobGatherUiU32(gUiMobGatherSettleErr);
    c.mobGatherKpSettle = MobGatherUiU32(gUiMobGatherKpSettle);
    c.mobGatherBrakeMs = MobGatherUiU32(gUiMobGatherBrakeMs);
    c.mobGatherCoastVy = MobGatherUiU32(gUiMobGatherCoastVy);
    c.mobGatherAimMs = MobGatherUiU32(gUiMobGatherAimMs);
    c.mobGatherSoftRelogin = gUiMobGatherSoftRelogin ? 1u : 0u;
    c.mobGatherSoftReloginSec = xcat::ClampMobGatherSoftReloginSec(
        static_cast<uint32_t>(gUiMobGatherSoftReloginSec < 0 ? 0 : gUiMobGatherSoftReloginSec));
    c.mobGatherHangupFires = xcat::ClampMobGatherHangupFires(
        static_cast<uint32_t>(gUiMobGatherHangupFires < 0 ? 0 : gUiMobGatherHangupFires));
    c.mobGatherHangupFiresOn = gUiMobGatherHangupFiresOn ? 1u : 0u;
    c.mobGatherHangupUnbindF5 = gUiMobGatherHangupUnbindF5 ? 1u : 0u;
    c.gatherTabUnlocked = WorkspaceGatherTabUnlocked() ? 1u : 0u;
    c.mobGatherClearRelogin = gUiMobGatherClearRelogin ? 1u : 0u;
    c.mobGatherSeekCluster = gUiMobGatherSeekCluster ? 1u : 0u;
    c.mobGatherPatrolFar = gUiMobGatherPatrolFar ? 1u : 0u;
    c.mobGatherAntiReport = 0u;
    c.mobGatherHomeReturn = gUiMobGatherHomeReturn ? 1u : 0u;
    c.mobGatherHomeX = xcat::ClampMobGatherStandOffX(gUiMobGatherHomeX);
    c.mobGatherHomeY = xcat::ClampMobGatherStandOffY(gUiMobGatherHomeY);
    c.mobGatherHomeMapId = gUiMobGatherHomeMapId;
    c.mobGatherHomeValid = gUiMobGatherHomeValid ? 1u : 0u;
    c.mobGatherHomeHasMap = gUiMobGatherHomeHasMap ? 1u : 0u;
    c.mobGatherApplyCtrl = gUiMobGatherApplyCtrl ? 1u : 0u;
    c.attackAccelClearBusy = gUiAttackAccelClearBusy ? 1u : 0u;
    c.attackAccelSkipPrepare = gUiAttackAccelSkipPrepare ? 1u : 0u;
    c.attackAccelCutLayer = gUiAttackAccelCutLayer ? 1u : 0u;
    c.simpleCombatTickMs = xcat::ClampSimpleCombatTickMs(
        static_cast<uint32_t>(gUiCombatTickMs < 0 ? 0 : gUiCombatTickMs));
    gUiCombatTickMs = (int)c.simpleCombatTickMs;
    c.simpleCombatSkipAccMiss = gUiSkipAccMissOn ? 1u : 0u;
    c.simpleCombatSkipAccMissN = xcat::ClampCombatSkipAccMissN(
        static_cast<uint32_t>(gUiSkipAccMissN < 1 ? 1 : gUiSkipAccMissN));
    gUiSkipAccMissN = (int)c.simpleCombatSkipAccMissN;
    c.secAttackIntercept = 0;
    c.secAttackTextHook = 0;
    c.writeTickMs = GetTickCount64();
    // 断连/软重连风暴期 DLL 侧也在读写 user.ini，UpdateIniFile 抢锁或瞬时读会偶发失败。
    // 单次失败就交给 MobGatherTrySaveOrRevert 把整卡 UI 从盘上旧值刷回 —— 刚勾的「申请控制权」
    // 等开关当场被抹回 0（BIN 见 applyCtrl 先 1 后 0）。这里短重试吸收瞬时冲突，别让一次抖动
    // 毁掉用户刚落的勾选。
    bool wrote = false;
    for (int i = 0; i < 4 && !wrote; ++i) {
        c.writeTickMs = GetTickCount64();
        wrote = xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c);
        if (!wrote) Sleep(40);
    }
    if (!wrote) return false;
    // 必须回读真实 tick：WritePayloadControl 可能把 writeTickMs 单调 +1，
    // 若这里记自己的 c.writeTickMs，下一帧 MobGatherSyncFromDisk 见盘上 tick 不等
    // → 判定「盘被外部改过」把 gUi 从盘冲回旧值（吸怪 TAB 勾选被写回 0 的根因）。
    xcat::PayloadControl verify{};
    if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), verify))
        sMobGatherLastTick = verify.writeTickMs;
    else
        sMobGatherLastTick = c.writeTickMs;
    return true;
}

static void MobGatherTrySaveOrRevert(LaunchUiState& ui) {
    if (MobGatherSaveUi(ui)) {
        sMobGatherSaveFailed = false;
        return;
    }
    sMobGatherSaveFailed = true;
    MobGatherLoadFromDisk(ui);
}

static void MobGatherSyncFromDisk(LaunchUiState& ui) {
    if (sMobGatherLoadedBin != ui.prefsBinDir) {
        sMobGatherLoadedBin = ui.prefsBinDir;
        MobGatherLoadFromDisk(ui);
        sMobGatherSaveFailed = false;
    } else if (!ui.prefsBinDir.empty() && !ImGui::IsAnyItemActive() && !sMobGatherSaveFailed) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk) &&
            disk.writeTickMs != sMobGatherLastTick) {
            MobGatherApplyDisk(disk);
        }
    }
}

static void MobGatherDragU(LaunchUiState& ui, const char* label, const char* id, int* v, int lo,
                          int hi, float speed, const char* unit, const char* tip) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.f, ui::Gap() * 0.45f);
    ImGui::SetNextItemWidth(AppDpi_Px(80.f));
    char hid[64]{};
    snprintf(hid, sizeof(hid), "##mg_%s", id);
    if (ImGui::DragInt(hid, v, speed, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        MobGatherTrySaveOrRevert(ui);
    }
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip);
    if (unit && unit[0]) {
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted(unit);
    }
}

static void MobGatherDragFree(LaunchUiState& ui, const char* label, const char* id, int* v,
                             float speed, const char* unit, const char* tip) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.f, ui::Gap() * 0.45f);
    ImGui::SetNextItemWidth(AppDpi_Px(80.f));
    char hid[64]{};
    snprintf(hid, sizeof(hid), "##mg_%s", id);
    if (ImGui::DragInt(hid, v, speed, 0, 0, "%d")) {
        MobGatherTrySaveOrRevert(ui);
    }
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip);
    if (unit && unit[0]) {
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted(unit);
    }
}

static const char* SecAttackPktOpcodeName(int opcode) {
    switch (opcode) {
        case 50:
            return "近战";
        case 51:
            return "射击";
        case 52:
            return "魔法";
        case 53:
            return "身体";
        case 191:
            return "召唤";
        default:
            return nullptr;
    }
}

void DrawMobGatherType20ProbeCard(LaunchUiState& ui) {
    xcat::ui::CardGuard card("##tab_gather_type20", "type20 观测");
    ImGui::TextDisabled("只读原生 SecurityClient 字典 peakKey，不出刀闸。约 0.5s 刷新。");
    ImGui::TextDisabled("BIN：XCat_data/logs/sec_attack.log  ·  grep PROBE");
    xcat::PayloadStatus st{};
    const bool fresh = !ui.prefsBinDir.empty() &&
                       xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) &&
                       xcat::PayloadStatusHeartbeatFresh(st, GetTickCount64(), 5000);
    if (!fresh) {
        ImGui::TextDisabled("未读到载荷状态（注入后进图）");
        return;
    }
    if (st.playReady == 0) {
        ImGui::TextDisabled("未进图");
        return;
    }
    if (st.secAttackOk == 0) {
        ImGui::TextDisabled("字典未绑上（进图打几下再看）");
        return;
    }

    const int peak = (int)st.secAttackPeak;
    const int pct = (int)st.secAttackPct;
    ImVec4 peakCol = ImVec4(0.35f, 0.85f, 0.45f, 1.f);
    if (peak >= 1900)
        peakCol = ImVec4(1.f, 0.35f, 0.35f, 1.f);
    else if (peak >= 1500)
        peakCol = ImVec4(1.f, 0.75f, 0.2f, 1.f);
    ImGui::TextUnformatted("peakKey");
    ImGui::SameLine();
    ImGui::TextColored(peakCol, "%d / 2000 (%d%%)", peak, pct);
    ImGui::SameLine();
    ImGui::TextDisabled("窗内最高 %d", (int)st.secAttackWindowPeak);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "type20 按两本字典里某一个键的当前计数是否超过 2000。\n"
            "不是出刀次数，也不是包合计。原生约 60s 清窗；落地也会把字典置 0。");
    }

    const char* pktName = SecAttackPktOpcodeName((int)st.secAttackPktPeakId);
    if (pktName) {
        ImGui::Text("攻包单键  %d  opcode=%d %s    合计 %d", (int)st.secAttackPktPeak,
                    (int)st.secAttackPktPeakId, pktName, (int)st.secAttackPktSum);
    } else {
        ImGui::Text("攻包单键  %d  opcode=%d    合计 %d", (int)st.secAttackPktPeak,
                    (int)st.secAttackPktPeakId, (int)st.secAttackPktSum);
    }
    ImGui::Text("技能单键  %d  skillId=%d    合计 %d", (int)st.secAttackSkillPeak,
                (int)st.secAttackSkillPeakId, (int)st.secAttackSkillSum);

    if (st.hangupFiresNeed > 0) {
        ImGui::Text("出刀闸    %u / %u    （对照，不是同一把尺）", st.hangupFires,
                    st.hangupFiresNeed);
    } else {
        ImGui::TextDisabled("出刀闸    关    本轮计数 %u", st.hangupFires);
    }

    ImGui::TextDisabled("detectTime %d", (int)st.secAttackDetectTime);
}

void DrawMobGatherTab(LaunchUiState& ui) {
    if (!gGatherTabUnlocked) {
        ImGui::TextDisabled("未解锁");
        return;
    }
    MobGatherSyncFromDisk(ui);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, ui::Gap() * 0.85f));

    {
        xcat::ui::CardGuard card("##tab_gather_accel", "快攻");
        {
            char tip[320]{};
            snprintf(tip, sizeof(tip),
                     "打怪状态机心跳（%u–%u ms，默认 %u）。\n"
                     "与是否开攻击加速无关；越短出刀机会越多。\n"
                     "下限 %u ms；过短更吃 CPU/主线程。",
                     (unsigned)xcat::kSimpleCombatTickMinMs,
                     (unsigned)xcat::kSimpleCombatTickMaxMs,
                     (unsigned)xcat::kSimpleCombatTickDefaultMs,
                     (unsigned)xcat::kSimpleCombatTickMinMs);
            MobGatherDragU(ui, "TICK值", "combat_tick", &gUiCombatTickMs,
                           (int)xcat::kSimpleCombatTickMinMs, (int)xcat::kSimpleCombatTickMaxMs,
                           1.f, "ms", tip);
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::TextDisabled("全局心跳 · 非仅加速");
        }
        if (xcat::ui::OptionCheckbox("攻击无CD", &gUiAttackAccelClearBusy))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "周期写 LocalUser ActionBusy=+0x118=-1，\n"
                "清掉引擎「动作未完吞键」忙锁，出刀可更快再按。\n"
                "产品名「攻击无CD」；不是技能服端 CD。\n"
                "出刀频率看「首页 → 挂机 → 出刀间隔」。\n"
                "进图落地约 0.4s 内暂停写入，降低脱同步。\n"
                "本 TAB 未解锁时不可用，也会强制关掉下发。");
        }
        {
            if (xcat::ui::OptionCheckbox("不打MISS怪", &gUiSkipAccMissOn))
                MobGatherTrySaveOrRevert(ui);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "打进攻击盒但 ACC 不够、飘 MISS 时换下一只。盒外空刀不算。\n"
                    "连续 N 次（默认 1）才换。\n"
                    "不预判 ACC/EVA——客户端 CheckPDamageMiss 吃随机数，乱调会搅 RNG。\n"
                    "免疫等其它 0 伤也可能被当成 MISS。\n"
                    "生效核对：combat.log 出现 acc_miss 与 switch reason=skip_acc_miss");
            }
            ImGui::BeginDisabled(!gUiSkipAccMissOn);
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (ImGui::DragInt("##skip_acc_miss_n", &gUiSkipAccMissN, 1,
                               (int)xcat::kCombatSkipAccMissNMin,
                               (int)xcat::kCombatSkipAccMissNMax)) {
                gUiSkipAccMissN = (int)xcat::ClampCombatSkipAccMissN(
                    static_cast<uint32_t>(gUiSkipAccMissN < 1 ? 1 : gUiSkipAccMissN));
                MobGatherTrySaveOrRevert(ui);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "连续 ACC MISS 次数（%u–%u，默认 %u）。满次数即禁锁换下一只。",
                    (unsigned)xcat::kCombatSkipAccMissNMin,
                    (unsigned)xcat::kCombatSkipAccMissNMax,
                    (unsigned)xcat::kCombatSkipAccMissNDefault);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("连续MISS换下一只");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "进盒 ACC 不够才算。连续飘字满 N 次后 8 秒内不打这只。");
            }
        }
        if (xcat::ui::OptionCheckbox("出刀软重连", &gUiMobGatherHangupFiresOn))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认开。本勾选只开累计出刀闸，与同卡「主动软重连」秒数闸互不绑架。\n"
                "调用出刀成功即 +1。到阈值必须先拆清 FLAG，卖装/赶路不得插队。落地清零。\n"
                "两勾都开则先到先拆。首页「软重连试连」必须开。上限 1900。");
        }
        ImGui::BeginDisabled(!gUiMobGatherHangupFiresOn);
        MobGatherDragU(ui, "出刀", "hangup_fires", &gUiMobGatherHangupFires,
                       (int)xcat::kMobGatherHangupFiresMin, (int)xcat::kMobGatherHangupFiresMax,
                       10.f, "刀",
                       "落地后累计出刀达此主动软重连。与 combat.log「fire id=」同拍 +1（不看忙位/命中）。换怪不清。\n"
                       "默认 1700，上限 1900。须勾上方「出刀软重连」。与秒数闸先到先拆。");
        ImGui::EndDisabled();
        {
            const bool forceByTeleport =
                (gUiApproachMode == 3 && sMobGatherAutoCombat && !gUiMobGatherHangupUnbindF5);
            bool shownSoftRelogin = gUiMobGatherSoftRelogin || forceByTeleport;
            ImGui::BeginDisabled(forceByTeleport);
            if (xcat::ui::OptionCheckbox("主动软重连", &shownSoftRelogin)) {
                if (!forceByTeleport) {
                    gUiMobGatherSoftRelogin = shownSoftRelogin;
                    MobGatherTrySaveOrRevert(ui);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "默认关。本勾选只开秒数闸。第一刀起表，满 X 秒后拆会话。\n"
                    "同卡「出刀软重连」是累计出刀闸，可单独勾选。两勾都开则先到先拆。\n"
                    "首页「软重连试连」必须开，否则不会拆会话。\n"
                    "核心：勾了且出过刀必须 hangup 清加速 FLAG（配合攻击加速规避检测）。\n"
                    "会话脏了换图/赶路不重置倒计时。出刀后关 F5 仍走完这一轮。没出过刀才不计时，满包可直接卖。\n"
                    "追怪选「瞬移找怪」且 F5 开着时默认强制开秒数闸（切走瞬移恢复勾选，不改落盘）。\n"
                    "调试 TAB「打怪节奏」下「解除主动软重连绑定F5」打开后不再强制，只跟本勾选。\n"
                    "F5 开打后到点必拆。遇人（勾了换频）改为软重连进新频，不回原频，并算一次 hangup。\n"
                    "自动卖装 / 补给赶路 / Travel / 测谎途中暂停倒计时，回挂机图再继续。\n"
                    "不点確認、不清登录态。落盘 user.ini。");
            }
            ImGui::BeginDisabled(!gUiMobGatherSoftRelogin && !forceByTeleport);
            ImGui::SameLine();
            ImGui::TextDisabled("间隔");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(AppDpi_Px(56.f));
            if (xcat::ui::DragIntClamped("##hangup_soft_sec", &gUiMobGatherSoftReloginSec,
                                         static_cast<int>(xcat::kMobGatherSoftReloginSecMin),
                                         static_cast<int>(xcat::kMobGatherSoftReloginSecMax))) {
                gUiMobGatherSoftReloginSec = static_cast<int>(xcat::ClampMobGatherSoftReloginSec(
                    static_cast<uint32_t>(gUiMobGatherSoftReloginSec < 0
                                              ? 0
                                              : gUiMobGatherSoftReloginSec)));
                MobGatherTrySaveOrRevert(ui);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "满这么多秒后触发。拖动松手或双击填写后失焦才生效。默认 20，范围 10–3600。\n"
                    "只随同排「主动软重连」勾选。出刀闸在同卡「出刀软重连」，可单独勾。");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("秒");
            ImGui::EndDisabled();
        }
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_switch", "开关");
        if (xcat::ui::OptionCheckbox("吸怪", &gUiMobGather)) MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "把这台客户端正在模拟的怪吸到角色朝向面前的落点，方便打。\n"
                "不是防抢。追怪选「站桩输出」不会自动开本项。落盘 user.ini。");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(48.f));
        {
            int strat = gUiMobGatherStrategy;
            if (strat < 0 || strat > 1) strat = 0;
            const char* items[] = {"A", "B"};
            if (ImGui::Combo("##gather_strat", &strat, items, IM_ARRAYSIZE(items))) {
                gUiMobGatherStrategy = strat;
                MobGatherTrySaveOrRevert(ui);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "吸怪策略。\n"
                    "A = IMPACT：卸台后 SetImpactNext 吸到面前空点悬停（现有）。\n"
                    "B = 卸台后一帧把怪坐标写到面前落点，不挂台。\n"
                    "切换会放掉当前吸住的怪。默认 A。落盘 user.ini。");
            }
        }
        {
            xcat::PayloadStatus st{};
            const bool fresh = !ui.prefsBinDir.empty() &&
                               xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) &&
                               xcat::PayloadStatusHeartbeatFresh(st, GetTickCount64(), 5000);
            if (!fresh || st.playReady == 0 || st.secAttackOk == 0) {
                ImGui::TextDisabled("攻包窗  未读到（进图后刷新）");
            } else {
                ImGui::Text("peakKey  %d / 2000 (%d%%)    窗内最高 %d",
                            (int)st.secAttackPeak, (int)st.secAttackPct, (int)st.secAttackWindowPeak);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "窗内最高跟本局出刀同一拍起算：软重连落地清零观测高水位，原生 60s 到期也会清。\n"
                        "只读 SecurityClient 本地窗；服端若自己数线上攻包，这里看不到。");
                }
                ImGui::TextDisabled("包合计 %d / 最高 %d    技能合计 %d / 最高 %d",
                                    (int)st.secAttackPktSum, (int)st.secAttackWindowPktSum,
                                    (int)st.secAttackSkillSum, (int)st.secAttackWindowSkillSum);
            }
        }
        if (xcat::ui::OptionCheckbox("申请控制权", &gUiMobGatherApplyCtrl))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关。勾上后吸怪持有期每秒对半径内非 Active 怪走官方 ApplyControl。\n"
                "客户端只能申请；服端批 280 才变你控。不改 0xE8、不造包、不谎报优先级。\n"
                "贴脸更值钱。狂申帮不上忙，还可能踢。落盘 user.ini。");
        }
        MobGatherDragU(ui, "延时启动", "qdelay", &gUiMobGatherQuietDelayMs,
                       (int)xcat::kMobGatherQuietDelayMsMin, (int)xcat::kMobGatherQuietDelayMsMax,
                       50.f, "ms",
                       "作用整模块：开吸怪或软重连 hold 结束进图后，再等这么久才收怪。0=立刻。\n"
                       "与「落地也吸」无关。hold 中硬停并清钟。拖动或双击填。只防爆钳。\n"
                       "BIN：tick qdelay= / skip why=quiet_delay left=。");
        MobGatherDragU(ui, "同时", "max", &gUiMobGatherMax, (int)xcat::kMobGatherMaxMin,
                       (int)xcat::kMobGatherMaxMax, 1.f, "只",
                       "同时吸住的上限，默认 64。BIN：tick max=。调低立刻丢掉多出来的。\n"
                       "BIN 里 pushed= 变小通常是活怪打完了，不是这个闸。");
        MobGatherDragU(ui, "在途", "farin", &gUiMobGatherFarInFlight,
                       (int)xcat::kMobGatherFarInFlightMin, (int)xcat::kMobGatherFarInFlightMax, 1.f,
                       "只",
                       "巡航圈外同时往回飞的上限。0=不限。脚边不占这格。\n"
                       "BIN：tick skipFar= / farAdm=。远拉太多容易掐线。");
        ImGui::TextDisabled("默认关 · 落盘");
        ImGui::TextDisabled("吸速 / 防抖 / 作动器在「调试 → 吸怪飞控」");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_home", "归位");
        if (xcat::ui::OptionCheckbox("软重连后返回原位", &gUiMobGatherHomeReturn)) {
            if (gUiMobGatherHomeReturn) {
                gUiMobGatherSeekCluster = false;
                gUiMobGatherPatrolFar = false;
            }
            MobGatherTrySaveOrRevert(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关。没记过点不飞（F5 或点「记录人物坐标」才生效；再按 F5 会覆盖）。\n"
                "与「先飞到最密堆再吸」互斥，两勾不能同时开。\n"
                "F5 每次开/关打怪都会刷新记录，或点「记录人物坐标」（需站在台上；更大 Y = 更高）。\n"
                "也可微调 X/Y。坐标落盘 user.ini：关启动器、游戏死了再开都还在。\n"
                "软重连回图、或游戏重开后进到这张图，离记录点够远就飞回去，挂台站稳再吸。\n"
                "没点过记录（没有图号）不飞。人在别的图就等进对的图再飞。");
        }
        ImGui::BeginDisabled(!gUiMobGatherHomeReturn);
        if (ImGui::Button("记录人物坐标##mg_home_rec")) {
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warning*/ 2, "gather-home", "记录失败", "无数据目录", 3000);
            } else {
                const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
                if (leds.gamePid == 0) {
                    notify::PushLocal(/*Warning*/ 2, "gather-home", "记录失败", "需已注入游戏",
                                     3000);
                } else {
                    uint32_t seq = xcat::ReadMobGatherHomeRecordSeq(ui.prefsBinDir.c_str());
                    seq = seq == 0 ? 1u : seq + 1u;
                    if (seq == 0) seq = 1u;
                    if (xcat::WriteMobGatherHomeRecordSeq(ui.prefsBinDir.c_str(), seq)) {
                        xcat::log::Ok("App", "mobGatherHomeRecordSeq=%u", seq);
                        notify::PushLocal(/*Info*/ 1, "gather-home", "已下发记录",
                                         "人物需站在台上；成功后 X/Y 会更新", 4000);
                    } else {
                        notify::PushLocal(/*Warning*/ 2, "gather-home", "记录失败",
                                         "写指令失败", 3000);
                    }
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "把当前人物 AbsPos 和图号写入原位。必须已注入、人站在台上。\n"
                "F5 开/关打怪都会刷新（须站在台上）。落盘 user.ini。");
        }
        {
            const int ox = gUiMobGatherHomeX;
            const int oy = gUiMobGatherHomeY;
            MobGatherDragU(ui, "原位X", "homex", &gUiMobGatherHomeX,
                           (int)xcat::kMobGatherStandOffXMin, (int)xcat::kMobGatherStandOffXMax,
                           1.f, "px",
                           "AbsPos X。F5 或点「记录人物坐标」会覆盖。拖动或双击填。只防爆钳 ±30000。");
            MobGatherDragU(ui, "原位Y", "homey", &gUiMobGatherHomeY,
                           (int)xcat::kMobGatherStandOffYMin, (int)xcat::kMobGatherStandOffYMax,
                           1.f, "px",
                           "AbsPos Y。更大 = 更高。F5 或点「记录人物坐标」会覆盖。拖动或双击填。只防爆钳 ±30000。");
            if (!gUiMobGatherHomeValid &&
                (gUiMobGatherHomeX != ox || gUiMobGatherHomeY != oy)) {
                gUiMobGatherHomeValid = true;
                MobGatherTrySaveOrRevert(ui);
            }
        }
        if (gUiMobGatherHomeValid && gUiMobGatherHomeHasMap) {
            char mapKey[16];
            snprintf(mapKey, sizeof(mapKey), "%d", gUiMobGatherHomeMapId);
            std::string mapDisp;
            const char* name = LookupFarmMapDisp(ui.prefsBinDir.c_str(), mapKey, mapDisp);
            ImGui::TextDisabled("已记录 (%d, %d) %s · 再点按钮会覆盖", gUiMobGatherHomeX,
                                gUiMobGatherHomeY, name);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("图号：%d", gUiMobGatherHomeMapId);
            }
        } else if (gUiMobGatherHomeValid) {
            ImGui::TextDisabled("有坐标无图号 · 点「记录人物坐标」才会归位");
        } else {
            ImGui::TextDisabled("尚未记录 · 站到挂机点后点「记录人物坐标」");
        }
        ImGui::EndDisabled();
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_aim", "落点");
        if (xcat::ui::OptionCheckbox("自定义落点", &gUiMobGatherStandOffCustom))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "怪悬停在自定义空点，不贴台。跟首页空中贴怪站距各存各的。\n"
                "不勾 = 近战默认 X=%d、Y=%d。拖动或双击填数。AbsPos：更大 Y = 更高。\n"
                "BIN：tick off=X,Y。",
                (int)xcat::kMobGatherStandOffXDefault, (int)xcat::kMobGatherStandOffYDefault);
        }
        ImGui::BeginDisabled(!gUiMobGatherStandOffCustom);
        MobGatherDragU(ui, "X", "offx", &gUiMobGatherStandOffX, (int)xcat::kMobGatherStandOffXMin,
                       (int)xcat::kMobGatherStandOffXMax, 1.f, "px",
                       "相对朝向的水平偏移。正=面前，负=背后。拖动或双击填。只防爆钳 ±30000。\n"
                       "BIN：tick off=。");
        MobGatherDragU(ui, "Y", "offy", &gUiMobGatherStandOffY, (int)xcat::kMobGatherStandOffYMin,
                       (int)xcat::kMobGatherStandOffYMax, 1.f, "px",
                       "相对人 AbsPos 的垂直偏移。正数 = 更高。拖动或双击填。只防爆钳 ±30000。\n"
                       "BIN：tick off=。");
        ImGui::EndDisabled();
        MobGatherDragU(ui, "抖动", "spread", &gUiMobGatherAimJitter,
                       (int)xcat::kMobGatherAimJitterMin, (int)xcat::kMobGatherAimJitterMax, 1.f,
                       "px",
                       "每只怪在落点周围用 oid 算出固定偏移，避免全叠一点。默认 24。\n"
                       "0=叠在同一点。越大越散。不是每帧乱抖。只防爆钳 30000。\n"
                       "BIN：tick spread=。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_dispclamp", "防断");
        MobGatherDragU(ui, "远怪接力", "hoppx", &gUiMobGatherHopPx, 0,
                       (int)xcat::kMobGatherHopPxMax, 5.f, "px/跳",
                       "远怪分段接力拉（防断主力）：距离超过该值的怪不一次拉到位，每跳最多拉\n"
                       "这么远、到中转点停 0.45 秒让服务器落账，再拉下一跳。实测服务器按\n"
                       "「单次连续拉取总距」掐线：≤1024px 零断连、≥1253px 必掐；默认 950 留余量。\n"
                       "0=关（直拉老行为）。1~199 会被抬到 200。改小更稳、远怪到得更慢。\n"
                       "BIN：MobFhBan hop start/next。");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("远怪逐跳接过来，绕开服务器约 1200px 单次拉距红线；0=关。");
        ImGui::PopTextWrapPos();
        if (xcat::ui::OptionCheckbox("位移夹速", &gUiMobGatherDispClampOn))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "把被拽怪每帧位移夹到不超过右边的上限（默认开）。\n"
                "客户端每帧比对怪移动位移，超过「怪速+10」就走异常上报路径，\n"
                "服务器攒够即踢下线（第二波吸怪断连的扳机）。夹住就永不触发。\n"
                "BIN：MobFhBan impact 的 disp/cap/clamp。");
        }
        ImGui::BeginDisabled(!gUiMobGatherDispClampOn);
        MobGatherDragFree(ui, "位移上限", "dispcap", &gUiMobGatherDispCapPx, 1.f, "px/帧",
                          "每帧位移上限（默认 48，约 1600 px/s）。防爆钳 8–400。\n"
                          "还断连就调小；不断连可逐步调大换吸得更快。\n"
                          "看 BIN 日志 MobFhBan impact 的 disp= 真实值来标定。");
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("夹的是怪的每帧位移，不是吸速%%；上限越小怪飞得越慢但越稳。");
        ImGui::PopTextWrapPos();
        if (xcat::ui::OptionCheckbox("到站落地", &gUiMobGatherLandOnArrive))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "策略 A 专用：怪吸到面前站距后就松手，交给游戏自身重力自然落到脚下踏板，\n"
                "不再逐帧把它吊在空中（怀疑「一直空中态」被服务器判异常踢线）。\n"
                "玩家走远后会自动重新吸拉。落点用下面的聚拢站距 X/Y 自己调。\n"
                "BIN：MobFhBan land-on-arrive；落地怪主循环打 why=landed。");
        }
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("到站落地=不再悬停、让怪掉到脚边踏板；落点靠聚拢站距调。");
        ImGui::PopTextWrapPos();
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_relogin", "重连");
        if (xcat::ui::OptionCheckbox("清怪重连", &gUiMobGatherClearRelogin))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关。独立于「快攻」卡「主动软重连」。必须先开吸怪才会拆会话。\n"
                "「同时」=N 时吸到第 N 只就冻结（同时=1 只吸 1 只，死了不补下一只）。\n"
                "这批死光再 CloseSession。超时溜走、落地清空名单不拆。首页「软重连试连」必须开。");
        }
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_range", "收怪");
        MobGatherDragU(ui, "半径", "radius", &gUiMobGatherRadiusPx, (int)xcat::kMobGatherRadiusMinPx,
                       (int)xcat::kMobGatherRadiusMaxPx, 50.f, "px",
                       "新收半径，默认 1000（实机 9.4min 零断连）。只防爆钳 30000。\n"
                       "服端按离怪原位总位移掐线，>~1150 易断；远怪请开「远怪自动巡点」。\n"
                       "已吸住的飞出仍维持，直到超时。BIN：tick r=。");
        MobGatherDragU(ui, "超时", "hold", &gUiMobGatherHoldMs, (int)xcat::kMobGatherHoldMsMin,
                       (int)xcat::kMobGatherHoldMsMax, 100.f, "ms",
                       "白名单维持超时。人一直在吸会续期。BIN：tick hold=。");
        MobGatherDragU(ui, "间隔", "iv", &gUiMobGatherIntervalMs, (int)xcat::kMobGatherIntervalMinMs,
                       (int)xcat::kMobGatherIntervalMaxMs, 10.f, "ms",
                       "新收一批的间隔，默认 40。已吸住的怪仍跟瞄准 VTOL，不会按这个停下来。\n"
                       "加大=少扫图、少进泵。BIN：tick iv=。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_gate", "闸门");
        MobGatherDragU(ui, "高度闸", "dylim", &gUiMobGatherDyLimPx, (int)xcat::kMobGatherDyLimPxMin,
                       (int)xcat::kMobGatherDyLimPxMax, 10.f, "px",
                       "新收竖直上限 |mobY-人Y|，默认 1200。AbsPos 更大 Y = 更高。用户自填。\n"
                       "站着吸时：距人 hypot 超过「脚边」且 |dY| 超过这个数 → 不新 Arm。已吸住的继续。\n"
                       "0=任意高度差都不新收。只防爆钳 30000。不是「竖层」。\n"
                       "调试 TAB「高度闸扫描」测极限；停扫恢复本滑条。\n"
                       "BIN：tick skipDy= / dyLim=。");
        MobGatherDragU(ui, "履历", "walkdx", &gUiMobGatherWalkDx, (int)xcat::kMobGatherWalkDxMin,
                       (int)xcat::kMobGatherWalkDxMax, 1.f, "px",
                       "履历闸横移。新 oid 相对第一次见到的 Ap，|dx| 小于此不 Arm。默认 96。用户自填。\n"
                       "0=不挡横移。调小=更容易收补刷；调大=等巡逻走开。只防爆钳 30000。\n"
                       "脚边以内不走这闸。BIN：dHome= / dHomeMax= / walkDx=。");
        MobGatherDragU(ui, "脚边", "feet", &gUiMobGatherFeetExemptPx,
                       (int)xcat::kMobGatherFeetExemptPxMin, (int)xcat::kMobGatherFeetExemptPxMax,
                       10.f, "px",
                       "距人 hypot ≤此不走履历闸和高度闸，照吸。默认 320。用户自填。\n"
                       "0=不开豁免。只防爆钳 30000。BIN：tick feet=。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_land", "落地");
        if (xcat::ui::OptionCheckbox("落地也吸", &gUiMobGatherIgnoreQuiet))
            MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关：落地静默会清白名单并 skip land_quiet。\n"
                "勾上：落地静默期也可以吸。换图 / 断线 / 软重连 hold 仍停。\n"
                "「延时启动」在开关卡，作用整模块，不跟本勾绑。");
        }
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_seek", "寻簇");
        if (xcat::ui::OptionCheckbox("先飞到最密堆再吸", &gUiMobGatherSeekCluster)) {
            if (gUiMobGatherSeekCluster) gUiMobGatherHomeReturn = false;
            MobGatherTrySaveOrRevert(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关。关着就是原来的站立吸怪（高度闸、履历闸、14 秒照旧）。\n"
                "勾上后：人先飞到当前层最密那堆（同层 |dY|≤竖层），到了挂台站稳再吸。\n"
                "不跨层俯冲。寻路/落地静默期间冻 14 秒钟。F6 / 赶路占旋翼时不抢。\n"
                "与「软重连后返回原位」互斥。不绑「群怪优先」。落盘 user.ini。");
        }
        if (xcat::ui::OptionCheckbox("远怪自动巡点", &gUiMobGatherPatrolFar)) {
            if (gUiMobGatherPatrolFar) gUiMobGatherHomeReturn = false;
            MobGatherTrySaveOrRevert(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "默认关。上面寻簇的跨层/全图版：手上没持怪时，不限竖层找全图最密那堆，\n"
                "人飞过去挂台站稳就地吸；这批清完再飞下一堆，循环巡图。\n"
                "为什么要人过去：服务器按「离怪原位总位移超约 1200px」掐线，远怪拉不得\n"
                "（夹速/接力等移动整形全试过没用），聚拢半径建议 ≤1100 只吸脚边，\n"
                "远怪靠这勾自动巡过去。单开本勾即可，不用同时勾上面。\n"
                "与「软重连后返回原位」互斥。BIN：seek_cluster latch patrol=1。");
        }
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("巡点=清完脚边就飞去下一堆；配合半径≤1100 用，远怪不再隔空硬拉。");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled();
        gUiMobGatherAntiReport = false;
        (void)xcat::ui::OptionCheckbox("防举报（已停用）", &gUiMobGatherAntiReport);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "已停用。BIN 证伪：翻分支只压客户端举报，服端自己量位移掐线，开了照断。\n"
                "代码硬关，勾了也不会打 GameAssembly .text。模块留着方便以后回滚。\n"
                "抗断请用策略 A + 夹速/小半径，或远怪自动巡点。");
        }
        MobGatherDragU(ui, "竖层", "layery", &gUiMobGatherLayerYPx,
                       (int)xcat::kMobGatherLayerYPxMin, (int)xcat::kMobGatherLayerYPxMax, 10.f,
                       "px",
                       "寻簇同层窗 |dY|，默认 200。AbsPos 更大 Y = 更高。用户自填。\n"
                       "只收人脚边这一层的堆；贴台 snap 跨层也用这个。\n"
                       "0=只认同一高度。太大=跨层俯冲。关寻簇时仍影响归位贴台。\n"
                       "只防爆钳 30000。BIN：tick layerY= / seek_cluster latch layerY=。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_gather_reset", "默认");
        if (ImGui::Button("恢复默认##mg_biz_reset")) {
            gUiMobGatherMax = (int)xcat::kMobGatherMaxDefault;
            gUiMobGatherFarInFlight = (int)xcat::kMobGatherFarInFlightDefault;
            gUiMobGatherRadiusPx = (int)xcat::kMobGatherRadiusDefaultPx;
            gUiMobGatherLayerYPx = (int)xcat::kMobGatherLayerYPxDefault;
            gUiMobGatherDyLimPx = (int)xcat::kMobGatherDyLimPxDefault;
            gUiMobGatherWalkDx = (int)xcat::kMobGatherWalkDxDefault;
            gUiMobGatherFeetExemptPx = (int)xcat::kMobGatherFeetExemptPxDefault;
            gUiMobGatherHoldMs = (int)xcat::kMobGatherHoldMsDefault;
            gUiMobGatherIntervalMs = (int)xcat::kMobGatherIntervalDefaultMs;
            gUiMobGatherQuietDelayMs = (int)xcat::kMobGatherQuietDelayMsDefault;
            gUiMobGatherStandOffX = (int)xcat::kMobGatherStandOffXDefault;
            gUiMobGatherStandOffY = (int)xcat::kMobGatherStandOffYDefault;
            gUiMobGatherAimJitter = (int)xcat::kMobGatherAimJitterDefault;
            gUiMobGatherHangupFires = (int)xcat::kMobGatherHangupFiresDefault;
            gUiMobGatherStrategy = (int)xcat::kMobGatherStrategyDefault;
            gUiMobGatherDispClampOn = xcat::kMobGatherDispClampOnDefault != 0;
            gUiMobGatherDispCapPx = (int)xcat::kMobGatherDispCapPxDefault;
            gUiMobGatherLandOnArrive = xcat::kMobGatherLandOnArriveDefault != 0;
            gUiMobGatherHopPx = (int)xcat::kMobGatherHopPxDefault;
            gUiMobGatherAntiReport = false;
            MobGatherTrySaveOrRevert(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "业务旋钮回实测稳组合：策略 A、半径 1000、位移夹速开/48、到站落地关。\n"
                "「吸怪」总开关本身不动。飞控默认在「调试 → 吸怪飞控」。");
        }
    }
    CardGap();
    DrawMobGatherType20ProbeCard(ui);
    ImGui::PopStyleVar();
}

void DrawMobGatherDyLimScanCard(LaunchUiState& ui) {
    EnsureGatherUnlockLoaded();
    if (!gGatherTabUnlocked) return;
    xcat::ui::CardGuard card("##tab_dbg_gather_dylim", "高度闸扫描");
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled(
        "一键测竖直极限：自动开吸怪、关寻簇飞、半径 6000、冻结 14 秒软重连/清怪重连。"
        "闸从 1 每秒 +100 到 2000。掐线看 x.jsonl：dylim_ramp KICK dyLim=。不写 user.ini。"
        "停扫把闸改回「吸怪 快攻」TAB「高度闸」，不是写死 1200。");
    ImGui::PopTextWrapPos();
    const uint32_t rampSeq =
        ui.prefsBinDir.empty() ? 0u : xcat::ReadMobGatherDyRampSeq(ui.prefsBinDir.c_str());
    ImGui::Text("seq=%u  %s", rampSeq, rampSeq ? "扫描中/待停" : "闲置（闸=面板高度闸）");
    if (ImGui::Button("一键极限扫描##mg_dylim_go")) {
        if (ui.prefsBinDir.empty()) {
            notify::PushLocal(/*Warning*/ 2, "gather-dylim", "下发失败", "无数据目录", 3000);
        } else {
            const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
            if (leds.gamePid == 0) {
                notify::PushLocal(/*Warning*/ 2, "gather-dylim", "下发失败", "需已注入游戏",
                                 3000);
            } else {
                uint32_t seq = xcat::ReadMobGatherDyRampSeq(ui.prefsBinDir.c_str());
                seq = seq == 0 ? 1u : seq + 1u;
                if (seq == 0) seq = 1u;
                if (xcat::WriteMobGatherDyRampSeq(ui.prefsBinDir.c_str(), seq)) {
                    xcat::log::Ok("App", "mobGatherDyRampSeq=%u（一键扫描，不写 user.ini）", seq);
                    notify::PushLocal(/*Info*/ 1, "gather-dylim", "已开始一键扫描",
                                     "不用先开吸怪；掐线看 KICK dyLim", 4000);
                } else {
                    notify::PushLocal(/*Warning*/ 2, "gather-dylim", "下发失败",
                                     "写会话 seq 失败", 3000);
                }
            }
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "点一下就测。不用先勾吸怪、不用关寻簇、不用改半径。\n"
            "站上层即可。不落盘。");
    }
    ImGui::SameLine(0.f, ui::Gap());
    if (ImGui::Button("停止并恢复面板高度闸##mg_dylim_stop")) {
        if (ui.prefsBinDir.empty()) {
            notify::PushLocal(/*Warning*/ 2, "gather-dylim", "停止失败", "无数据目录", 3000);
        } else if (xcat::WriteMobGatherDyRampSeq(ui.prefsBinDir.c_str(), 0)) {
            xcat::log::Ok("App", "mobGatherDyRampSeq=0（恢复面板高度闸）");
            notify::PushLocal(/*Info*/ 1, "gather-dylim", "已停止", "高度闸恢复为面板值", 3000);
        } else {
            notify::PushLocal(/*Warning*/ 2, "gather-dylim", "停止失败", "写会话 seq 失败",
                             3000);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "停止扫描并把竖直闸改回「吸怪 快攻」TAB「高度闸」。\n"
            "开始扫描不会改半径、不落盘。");
    }
}

void DrawMobGatherFlyDebugCards(LaunchUiState& ui) {
    EnsureGatherUnlockLoaded();
    if (!gGatherTabUnlocked) return;
    MobGatherSyncFromDisk(ui);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, ui::Gap() * 0.85f));
    {
        xcat::ui::CardGuard card("##tab_dbg_gather_fly", "吸怪飞控");
        ImGui::TextDisabled("怪侧 VTOL。人飞最密堆走 F5 滑翔（「吸怪 快攻」TAB 勾选），本卡只拧怪侧。");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("吸速");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(80.f));
        if (ImGui::DragInt("##mg_speed", &gUiMobGatherSpeedPct, 5, 0, 0, "%d%%")) {
            MobGatherTrySaveOrRevert(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "与 F5 滑翔同一套：到位软钉、进站收油、按真实间隔配平。\n"
                "100%% = 巡航档 px/s（下面「档速」可改）。默认满火力档；≥5X 抄 F5 死拍。");
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.5f);
        ImGui::TextDisabled("= %.0f px/s",
                            (float)gUiMobGatherCruiseV * (float)gUiMobGatherSpeedPct / 100.f);
        const int presets[] = {100, 200, 300, 500};
        for (int i = 0; i < 4; ++i) {
            const int p = presets[i];
            char btnId[48]{};
            snprintf(btnId, sizeof(btnId), "%dX##mg_p%d", p / 100, p);
            if (i > 0) ImGui::SameLine(0.f, ui::Gap() * 0.4f);
            if (ImGui::Button(btnId)) {
                gUiMobGatherSpeedPct = p;
                MobGatherTrySaveOrRevert(ui);
            }
        }
        if (xcat::ui::OptionCheckbox("防抖", &gUiMobGatherAntiJitter)) MobGatherTrySaveOrRevert(ui);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "人微颤时钉住吸点（默认开）。\n"
                "开：人速度 <「静止阈」且位移 ≤「钉住」时怪不跟着晃。\n"
                "关：每拍跟角色朝向面前的站距落点。");
        }
        ImGui::BeginDisabled(!gUiMobGatherAntiJitter);
        MobGatherDragFree(ui, "钉住", "creep", &gUiMobGatherStickCreep, 1.f, "px",
                          "人位移不超过这个数就钉住吸点（默认 8）。");
        MobGatherDragFree(ui, "静止阈", "stillv", &gUiMobGatherStickStillV, 1.f, "px/s",
                          "人合速达到这个数就当在飞，每拍重算落点（默认 50）。");
        ImGui::EndDisabled();
        MobGatherDragFree(ui, "瞄准", "aim", &gUiMobGatherAimMs, 1.f, "ms",
                          "已吸住的怪刷新瞄准点的间隔，默认 17。跟新收间隔无关。\n"
                          "加大=少算落点；太小 CPU 更忙。BIN：worker aim=。");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_gather_act", "作动器");
        MobGatherDragFree(ui, "Kp", "kp", &gUiMobGatherKp, 1.f, "",
                          "比例增益（默认 7）。越大咬得越狠，也更容易抖。");
        MobGatherDragFree(ui, "死区", "dead", &gUiMobGatherDead, 1.f, "px",
                          "误差小于这个数不再加力（默认 6）。");
        MobGatherDragFree(ui, "顶速", "maxcmd", &gUiMobGatherMaxCmd, 50.f, "px/s",
                          "单轴速度顶格（默认 4800）。≥5X 死拍也吃这个顶。");
        MobGatherDragFree(ui, "巡航圈", "cruise", &gUiMobGatherCruiseR, 5.f, "px",
                          "距落点超过这个数用巡航档（默认 140）。档速见下面「巡航档」。");
        MobGatherDragFree(ui, "进站圈", "station", &gUiMobGatherStationR, 1.f, "px",
                          "距落点超过这个数用进站档（默认 28）。再近用悬停档。档速见下面。");
        MobGatherDragFree(ui, "重力", "grav", &gUiMobGatherGravity, 1.f, "",
                          "每物理步重力补偿（默认 60）。BIN：gLoss=。0 = 不补。");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("只改怪侧 VTOL，不接 F5 旋翼。乱拧会抖或穿台。");
        ImGui::PopTextWrapPos();
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_gather_tier", "档速");
        MobGatherDragFree(ui, "巡航档", "crv", &gUiMobGatherCruiseV, 10.f, "px/s",
                          "距落点超过「巡航圈」时的 1X 合速（默认 620）。再乘吸速%。");
        MobGatherDragFree(ui, "进站档", "stv", &gUiMobGatherStationV, 10.f, "px/s",
                          "进了巡航圈、还在进站圈外的 1X 合速（默认 480）。再乘吸速%。");
        MobGatherDragFree(ui, "悬停档", "hov", &gUiMobGatherHoldV, 10.f, "px/s",
                          "进了进站圈后的 1X 合速（默认 360）。再乘吸速%。≥5X 死拍改吃顶速。");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("1X 数字 × 吸速%%。上面「吸速」旁的 px/s 跟巡航档走。");
        ImGui::PopTextWrapPos();
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_gather_settle", "到位");
        MobGatherDragFree(ui, "到位圈", "serr", &gUiMobGatherSettleErr, 1.f, "px",
                          "防抖开且误差小于这个数，改用到位 Kp 软钉（默认 16）。");
        MobGatherDragFree(ui, "到位Kp", "kps", &gUiMobGatherKpSettle, 1.f, "",
                          "到位软钉的比例增益（默认 10）。比巡航 Kp 更狠、行程更短。");
        MobGatherDragFree(ui, "刹车", "brk", &gUiMobGatherBrakeMs, 5.f, "ms",
                          "≥5X 死拍对站点预刹时间（默认 150）。越大越早收油。");
        MobGatherDragFree(ui, "下滑切断", "coast", &gUiMobGatherCoastVy, 5.f, "px/s",
                          "Y 已在死区内但还在往下掉、合速超过这个数就切 Vy=0（默认 80）。0=不切。");
        if (ImGui::Button("恢复默认##mg_fly_reset")) {
            gUiMobGatherSpeedPct = (int)xcat::kMobGatherSpeedPctDefault;
            gUiMobGatherAntiJitter = xcat::kMobGatherAntiJitterDefault != 0;
            gUiMobGatherStickCreep = (int)xcat::kMobGatherStickCreepDefault;
            gUiMobGatherStickStillV = (int)xcat::kMobGatherStickStillVDefault;
            gUiMobGatherCruiseR = (int)xcat::kMobGatherCruiseRDefault;
            gUiMobGatherStationR = (int)xcat::kMobGatherStationRDefault;
            gUiMobGatherMaxCmd = (int)xcat::kMobGatherMaxCmdDefault;
            gUiMobGatherKp = (int)xcat::kMobGatherKpDefault;
            gUiMobGatherDead = (int)xcat::kMobGatherDeadDefault;
            gUiMobGatherGravity = (int)xcat::kMobGatherGravityDefault;
            gUiMobGatherCruiseV = (int)xcat::kMobGatherCruiseVDefault;
            gUiMobGatherStationV = (int)xcat::kMobGatherStationVDefault;
            gUiMobGatherHoldV = (int)xcat::kMobGatherHoldVDefault;
            gUiMobGatherSettleErr = (int)xcat::kMobGatherSettleErrDefault;
            gUiMobGatherKpSettle = (int)xcat::kMobGatherKpSettleDefault;
            gUiMobGatherBrakeMs = (int)xcat::kMobGatherBrakeMsDefault;
            gUiMobGatherCoastVy = (int)xcat::kMobGatherCoastVyDefault;
            gUiMobGatherAimMs = (int)xcat::kMobGatherAimMsDefault;
            MobGatherTrySaveOrRevert(ui);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("飞控旋钮回默认。吸怪开关不动。");
        }
    }
    ImGui::PopStyleVar();
}

void DrawWorkspaceTabContent(AppWindow& /*app*/, LaunchUiState& ui, int tabIndex) {
    switch (static_cast<WorkspaceTab>(tabIndex)) {
    case WorkspaceTab::Launch:
        DrawLaunchTab(ui);
        break;
    case WorkspaceTab::Home:
        DrawHomeTab(ui);
        break;
    case WorkspaceTab::HangupSchedule:
        DrawHangupScheduleTab(ui);
        break;
    case WorkspaceTab::MultiSkill:
        DrawMultiSkillTab(ui);
        break;
    case WorkspaceTab::Relogin:
        DrawReloginTab(ui);
        break;
    case WorkspaceTab::TimedKeys:
        DrawTimedKeysTab(ui);
        break;
    case WorkspaceTab::Buffs:
        DrawBuffsTab(ui);
        break;
    case WorkspaceTab::Travel:
        DrawTravelTab(ui);
        break;
    case WorkspaceTab::Beta:
        DrawBetaTab(ui);
        break;
    case WorkspaceTab::Debug:
        DrawDebugTab(ui);
        break;
    case WorkspaceTab::MobGather:
        DrawMobGatherTab(ui);
        break;
    case WorkspaceTab::AutoStat:
        DrawAutoStatTab(ui);
        break;
    case WorkspaceTab::AutoSkill:
        DrawAutoSkillCard(ui);
        break;
    case WorkspaceTab::CharBoot:
        DrawCharBootTab(ui);
        break;
    case WorkspaceTab::AutoSell:
        DrawAutoSellTab(ui);
        break;
    default:
        ImGui::TextDisabled("未知 TAB");
        break;
    }
}

}  // namespace xcat::app
