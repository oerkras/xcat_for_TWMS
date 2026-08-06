#include "workspace_tabs.h"

#include "app_dpi.h"
#include "app_notify.h"
#include "app_sound.h"
#include "app_theme.h"
#include "app_window.h"
#include "attach_inject.h"
#include "hangup_schedule.h"
#include "imgui_shell.h"
#include "launch_panel.h"
#include "lie_ai_pump.h"
#include "log_upload.h"
#include "log_upload_ui.h"
#include "runtime_leds.h"
#include "tdr_tune.h"
#include "update_client.h"

#include "msc_webview_login.h"
#include "process_util.h"
#include "xcat_buffs.h"
#include "xcat_imgui_basic.h"
#include "xcat_log.h"
#include "xcat_multiskill_select.h"
#include "xcat_payload_control.h"
#include "xcat_payload_status.h"
#include "xcat_anchor_lamps.h"
#include "xcat_pet_loot.h"
#include "xcat_sellbag.h"
#include "xcat_auto_supply.h"
#include "xcat_item_catalog.h"
#include "xcat_map_names.h"
#include "xcat_skill_names.h"
#include "xcat_timed_keys.h"
#include "xcat_version.h"
#include "xcat_world_names.h"
#include "xcat_worlds_cache.h"

#include "imgui.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace xcat::app {
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
    if (mapId > 0) {
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
        "对齐枫星布局；无敌/自动喝药接 [core]；"
        "宠吸 [pet_loot]；定时按键 [timed_keys]；BUFF [buffs] → payload");
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
}

void CardGap() { ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.55f)); }

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
            ImGui::SetTooltip("%s", snap.message.c_str());
        }
    } else if (!snap.message.empty()) {
        ImGui::TextWrapped("%s", snap.message.c_str());
    }
    ImGui::PopID();
}

void DrawLaunchTab(LaunchUiState& ui) {
    {
        xcat::ui::CardGuard card("##tab_launch_account", "启动 / 注入");
        const float wrapX = ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float rowW = ImGui::GetContentRegionAvail().x;
        const float halfW = (std::max)(1.f, (rowW - gap) * 0.5f);
        const float btnH = ui::BtnH();

        ImGui::TextUnformatted("启动模式");
        {
            const auto cur = attach_inject::GetLaunchMode();
            int modeIdx = 0;
            if (cur == attach_inject::LaunchMode::OneClickLogin) modeIdx = 1;
            else if (cur == attach_inject::LaunchMode::GamaPassAuto) modeIdx = 2;
            const char* items[] = {"手动启动并注入（推荐）", "gamania (HK)", "GAMA PASS自动登录"};
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::Combo("##launch_mode", &modeIdx, items, 3)) {
                LaunchPanel_ArmStrategyPrep(ui, 7000);
                if (modeIdx == 0) {
                    attach_inject::SetLaunchMode(attach_inject::LaunchMode::AttachWatch);
                    ui.pendingAutoLaunch = true;
                    ui.status = "已切换：手动启动并注入 — 约 7 秒后自动开始监视";
                } else if (modeIdx == 1) {
                    if (attach_inject::IsWatching()) attach_inject::StopWatch();
                    attach_inject::SetLaunchMode(attach_inject::LaunchMode::OneClickLogin);
                    if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
                        msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
                    }
                    ui.pendingAutoLaunch = false;
                    ui.status = "已切换：gamania (HK) — 约 7 秒后可点启动（防误触）";
                } else {
                    if (attach_inject::IsWatching()) attach_inject::StopWatch();
                    attach_inject::SetLaunchMode(attach_inject::LaunchMode::GamaPassAuto);
                    msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
                    // 无人值守闭环：切到本模式后约 7s 自动换票（与冷启一致）。
                    // 准备窗内仍可点一次「取消自动换票」打断。
                    ui.pendingAutoLaunch = true;
                    ui.status = "已切换：GAMA PASS自动登录 — 约 7 秒后自动换票（可再点取消）";
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "手动启动并注入：自行拉起游戏客户端后注入。\n"
                    "gamania (HK)：账密走 HTTP 换票 → 官方启动链 → 注入。\n"
                    "GAMA PASS自动登录：优先 Chrome++/Chrome 点选换票（无需账密）；"
                    "只用 Edge 请先卸 Google Chrome。"
                    "切模式/冷启约 7 秒自动换票；准备中再点一次可取消。"
                    "挂机时段到点、守护干净重拉也会自动一键。\n"
                    "写入 XCat_data/state/launch_mode.txt 与程序目录 launch_mode.txt");
            }
        }

        const auto launchMode = attach_inject::GetLaunchMode();
        const bool attachMode = attach_inject::IsAttachWatchMode(launchMode);
        const bool httpOneClick = (launchMode == attach_inject::LaunchMode::OneClickLogin);
        const bool gamaPassMode = (launchMode == attach_inject::LaunchMode::GamaPassAuto);
        const unsigned strategyPrepLeft = LaunchPanel_StrategyPrepLeftSec(ui);

        if (attachMode) {
            ImGui::TextDisabled("取票：本模式不换票（请手动开游戏后监视注入）");
        } else if (httpOneClick) {
            if (msc::weblogin::GetAuthStrategy() == msc::weblogin::AuthStrategy::GamaPassAuto) {
                msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::HttpFirst);
            }
            ImGui::TextDisabled("取票：HTTP（账密换票）");
            ImGui::TextUnformatted("验证码UI");
            {
                int mode =
                    (msc::weblogin::GetCaptchaUiMode() == msc::weblogin::CaptchaUiMode::NoBrowser)
                        ? 1
                        : 0;
                const char* items[] = {"开浏览器（默认）", "不开浏览器"};
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::Combo("##captcha_ui", &mode, items, 2)) {
                    msc::weblogin::SetCaptchaUiMode(
                        mode == 1 ? msc::weblogin::CaptchaUiMode::NoBrowser
                                  : msc::weblogin::CaptchaUiMode::OpenBrowser);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "遇验证码/二次验证时：\n"
                        "开浏览器：自动打开官网；\n"
                        "不开浏览器：仅提示，自行处理。\n"
                        "写入程序目录 captcha_ui.txt");
                }
            }
        } else {
            if (msc::weblogin::GetAuthStrategy() != msc::weblogin::AuthStrategy::GamaPassAuto) {
                msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
            }
            ImGui::TextDisabled("取票：GAMA PASS自动登录（仅浏览器会话，不使用账密）");
        }

        if (attachMode) {
            ImGui::PushTextWrapPos(wrapX);
            ImGui::TextUnformatted(
                "启动 XCat 后会自动开始监视；请用官方启动器/客户端打开游戏，发现进程后自动注入。");
            ImGui::PopTextWrapPos();

            const bool watching = attach_inject::IsWatching();
            const bool injBusy = attach_inject::IsInjectBusy();
            const bool autoPending = ui.pendingAutoLaunch;
            // 自动待办中允许点一次取消；无待办时准备窗仍防误触。
            const bool prepBlocksStart =
                strategyPrepLeft > 0 && !watching && !autoPending;
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

            if (autoPending && strategyPrepLeft > 0) {
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f),
                                   "准备中：%u 秒后自动监视（再点可取消）", strategyPrepLeft);
            } else if (prepBlocksStart) {
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f),
                                   "准备中：%u 秒后可开始监视（防误触）", strategyPrepLeft);
            }

            ImGui::TextDisabled("%s", attach_inject::StatusBrief().c_str());
        } else if (httpOneClick) {
            ImGui::PushTextWrapPos(wrapX);
            ImGui::TextUnformatted(
                "粘贴账号串（邮箱-密码-…，横线个数不限；只取前两项）。HTTP 换票成功后自动开游戏并注入。");
            ImGui::PopTextWrapPos();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##account", ui.accountLine, sizeof(ui.accountLine),
                                          ImVec2(-1, btnH * 3.6f),
                                      ImGuiInputTextFlags_AllowTabInput)) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            LaunchPanel_FormatAccountForUi(ui);
        }

            if (ImGui::Button("保存账号", ImVec2(halfW, btnH))) {
                sound::UiClick();
            LaunchPanel_SaveAccount(ui);
                ui.status = "已保存到程序目录 account.txt";
        }
            ImGui::SameLine(0.f, gap);
        const bool busy = msc::weblogin::IsBusy();
            const bool startBlocked = busy || strategyPrepLeft > 0;
            if (startBlocked) ImGui::BeginDisabled();
            if (ImGui::Button("一键启动游戏", ImVec2(halfW, btnH))) {
            LaunchPanel_StartOneClick(ui);
        }
            if (startBlocked) ImGui::EndDisabled();
            if (strategyPrepLeft > 0) {
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f),
                                   "准备中：%u 秒后可启动（防误触）", strategyPrepLeft);
            }

            ImGui::TextDisabled("HTTP 换票：遇验证码可按策略打开官网浏览器");
        } else if (gamaPassMode) {
            ImGui::PushTextWrapPos(wrapX);
            ImGui::TextUnformatted(
                "无需填写账号密码。"
                "优先使用 Chrome++ / Google Chrome，其次才是 Edge。"
                "只用 Edge 的用户请先卸载 Google Chrome，否则会绑到空的 Chrome 会话。"
                "请在将使用的浏览器里登录 GAMA PASS（accounts 选号页勾选记住）；"
                "一键前若该浏览器日常窗口已开且冲突，请先关掉（不会自动结束进程）。"
                "本程序自动点选账号/昵称换票，不会改写登录数据、不会调用 refresh。"
                "切换到本模式或冷启（已是本模式）约 7 秒后自动换票并启动游戏；"
                "准备中可再点一次「取消自动换票」打断，之后需手动启动。"
                "挂机到点 / 守护干净重拉也会自动一键（无人值守）。");
            ImGui::PopTextWrapPos();

            {
                int nickSlot = msc::weblogin::GetGamaPassNickSlot();
                const bool busy = msc::weblogin::IsBusy();
                ImGui::Spacing();
                const ImVec2 boxPad = ImGui::GetStyle().FramePadding;
                const float boxH = btnH * 2.15f + boxPad.y * 2.f;
                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                     ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::BeginChild("##gp_nick_box", ImVec2(-1.f, boxH),
                                  ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
                ImGui::TextUnformatted("游戏昵称槽");
                ImGui::SameLine();
                ImGui::TextDisabled("（多昵称时选第几个 · 1 起）");
                if (busy) ImGui::BeginDisabled();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                   ImVec2(boxPad.x * 1.6f, boxPad.y * 1.35f));
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputInt("##gp_nick_slot", &nickSlot, 1, 1)) {
                    if (nickSlot < 1) nickSlot = 1;
                    if (nickSlot > 16) nickSlot = 16;
                    msc::weblogin::SetGamaPassNickSlot(nickSlot);
                    ui.status = "GAMA PASS 昵称槽已设为 " + std::to_string(nickSlot) +
                                "（SelectGameAccount 第 N 个可选项，跳过「建立暱稱」）";
                }
                ImGui::PopStyleVar();
                if (busy) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "游戏昵称选择页（SelectGameAccount）的序号，从 1 起。\n"
                        "有多个昵称时填 1 / 2 / …；超过列表长度则取最后一个。\n"
                        "换票进行中不可改（本轮已锁定）。\n"
                        "写入程序目录 gamapass_nick_slot.txt");
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            const bool busy = msc::weblogin::IsBusy();
            const bool autoPending = ui.pendingAutoLaunch;
            if (busy) ImGui::BeginDisabled();
            const char* gpLabel =
                autoPending ? "取消自动换票" : "GAMA PASS 启动游戏";
            if (ImGui::Button(gpLabel, ImVec2(-1.f, btnH))) {
                if (autoPending) {
                    sound::UiClick();
                    LaunchPanel_CancelPendingAutoLaunch(ui);
                    ui.status = "已取消自动换票 — 需要时再点「GAMA PASS 启动游戏」";
                    xcat::log::Info("App", "user cancelled pending GamaPass auto-launch");
                } else {
                    msc::weblogin::SetAuthStrategy(msc::weblogin::AuthStrategy::GamaPassAuto);
                    LaunchPanel_StartOneClick(ui);
                }
            }
            if (busy) ImGui::EndDisabled();
            if (autoPending && strategyPrepLeft > 0) {
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f),
                                   "准备中：%u 秒后自动换票（再点可取消）", strategyPrepLeft);
            } else if (autoPending) {
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f),
                                   "即将自动换票…（再点可取消）");
            }

            ImGui::TextDisabled("GAMA PASS：默认浏览器 CDP 点选");
        }

        const UpdateSnapshot snap = GetUpdateSnapshot();
        if (snap.latestBuildId > 0 || !snap.message.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
            if (snap.latestBuildId > 0 && !snap.message.empty()) {
                ImGui::Text("最新 build %u · %s", snap.latestBuildId, snap.message.c_str());
            } else if (snap.latestBuildId > 0) {
                ImGui::TextDisabled("最新 build %u", snap.latestBuildId);
            } else {
                ImGui::TextUnformatted(snap.message.c_str());
            }
            ImGui::PopTextWrapPos();
        }

        if (!ui.status.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(ui.status.c_str());
            ImGui::PopTextWrapPos();
        }
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
// 跳过 Prepare / 砍动画倒计时：首页落盘；控件在实验 TAB 独立卡片。
static bool gUiAttackAccelSkipPrepare = false;  // 与 PayloadControl 默认一致
static bool gUiAttackAccelCutLayer = false;
static bool gUiAttackAccelBooster = false;
// 打怪 TICK：首页落盘；控件在调试 TAB。
static int gUiCombatTickMs = (int)xcat::kSimpleCombatTickDefaultMs;
// 出刀按键 hold：控件在调试 TAB，与 TICK 同卡片共用一次读盘。
static int gUiAttackHoldMs = (int)xcat::kAttackHoldDefaultMs;
// attack_rpc：仅实验 TAB；与 payload core 同步。
static bool gUiAttackRpc = false;
static int gUiAttackRpcMobs = (int)xcat::kAttackRpcMobsDefault;
static int gUiAttackRpcIntervalMs = (int)xcat::kAttackRpcIntervalDefaultMs;
static int gUiAttackRpcDamage = (int)xcat::kAttackRpcDamageDefault;

void DrawHomeTab(LaunchUiState& ui) {
    // 首页卡片顺序：挂机 → 拾物 → 攻击加速 → 打怪设置 → 卖背包（低内存守护在调试 TAB）
    static bool autoEnter = true;  // 默认开：1 雪吉拉 / 槽1
    static int charSlot = 1;
    static int worldId = xcat::kDefaultWorldId;
    static char worldName[64]{"雪吉拉"};
    static bool autoLie = false;
    static bool invincible = true;  // 与 PayloadControl 默认一致
    static bool attackAccel = false;
    static bool finalAttackForce = false;
    static bool skillMaxLevel = false;
    static bool fly = false;
    static bool hpPotion = true;
    static bool mpPotion = true;
    static bool petSummon = true;
    static bool petSummonRequireFood = true;
    static int hpThresholdPct = 50;
    static int mpThresholdPct = 30;
    static bool autoCombat = false;
    static bool smartInterval = false;
    static int attackMs = (int)xcat::kSimpleCombatAttackIntervalDefaultMs;
    static int clusterWeight = 0;  // 0/1：群怪优先（沿用 clusterWeight 落盘）
    static int teleportMinDx = 220;
    static int teleportStandOff = (int)xcat::kCombatTeleportStandOffDefault;
    static int teleportCooldownMs = (int)xcat::kCombatTeleportCooldownDefaultMs;
    static int mobScanIntervalMs = (int)xcat::kMobScanIntervalDefaultMs;
    static int crossLayerFillGateMs = (int)xcat::kCombatCrossLayerFillGateDefaultMs;
    static int fillBudgetPx = (int)xcat::kCombatFillBudgetPxDefault;
    static int oneshotMaxHp = (int)xcat::kCombatOneshotMaxHpDefault;
    static int oneshotMinBumps = (int)xcat::kCombatOneshotMinBumpsDefault;
    static int oneshotMinFires = (int)xcat::kCombatOneshotMinFiresDefault;
    static int oneshotMinLagMs = (int)xcat::kCombatOneshotMinLagMsDefault;
    static int oneshotFoxFillGapMs = (int)xcat::kCombatOneshotFoxFillGapDefaultMs;
    static int pumpCongestion = (int)xcat::kPumpCongestionDefault;
    static bool autoSell = false;
    static int sellEquipTrigger = 0;
    static bool refillHp = false;
    static bool refillMp = false;
    static bool refillCustom = false;
    static bool refillFeed = true;
    static char refillHpName[64]{};
    static char refillMpName[64]{};
    static char refillCustomName[64]{};
    static char refillFeedName[64]{};
    static char refillHpCode[24]{};
    static char refillMpCode[24]{};
    static char refillCustomCode[24]{};
    static char refillFeedCode[24]{};
    static int refillHpBuyTo = 0;
    static int refillMpBuyTo = 300;
    static int refillCustomBuyTo = 100;
    static int refillFeedBuyTo = 100;
    static bool rechargeStars = false;
    static uint64_t asupTick = 0;
    static bool asupLoaded = false;
    static bool watchdog = true;
    static int noExpSec = static_cast<int>(xcat::kWatchdogNoExpSecDefault);
    static int cooldownSec = static_cast<int>(xcat::kWatchdogCooldownSecDefault);
    static bool coreLoaded = false;
    static uint64_t lastSeenTick = 0;

    if (!ui.prefsBinDir.empty()) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
            if (!coreLoaded || disk.writeTickMs != lastSeenTick) {
                autoLie = disk.autoLie != 0;
                invincible = disk.invuln != 0;
                attackAccel = disk.attackAccel != 0;
                finalAttackForce = disk.finalAttackForce != 0;
                skillMaxLevel = disk.skillMaxLevel != 0;
                gUiAttackAccelCutLayer = disk.attackAccelCutLayer != 0;
                gUiAttackAccelSkipPrepare = disk.attackAccelSkipPrepare != 0;
                gUiAttackAccelBooster = disk.attackAccelBooster != 0;
                fly = disk.fly != 0;
                autoEnter = disk.autoEnter != 0;
                hpPotion = disk.hpPotion != 0;
                mpPotion = disk.mpPotion != 0;
                petSummon = disk.petSummon != 0;
                petSummonRequireFood = disk.petSummonRequireFood != 0;
                autoCombat = disk.simpleCombat != 0;
                smartInterval = disk.simpleCombatSmartInterval != 0;
                attackMs = (int)xcat::ClampSimpleCombatAttackIntervalMs(
                    disk.simpleCombatAttackIntervalMs
                        ? disk.simpleCombatAttackIntervalMs
                        : xcat::kSimpleCombatAttackIntervalDefaultMs);
                gUiCombatTickMs = (int)xcat::ClampSimpleCombatTickMs(
                    disk.simpleCombatTickMs ? disk.simpleCombatTickMs
                                           : xcat::kSimpleCombatTickDefaultMs);
                clusterWeight = disk.clusterWeight != 0 ? 1 : 0;
                // 贴怪瞬移已强制开；LiveStep / attack_rpc 仍默认关，仅实验 TAB 可勾。
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
                teleportMinDx = (int)xcat::ClampCombatTeleportMinDx(
                    disk.simpleCombatTeleportMinDx ? disk.simpleCombatTeleportMinDx
                                                   : xcat::kCombatTeleportMinDxDefault);
                teleportStandOff = (int)xcat::ClampCombatTeleportStandOff(
                    disk.simpleCombatTeleportStandOff ? disk.simpleCombatTeleportStandOff
                                                      : xcat::kCombatTeleportStandOffDefault);
                teleportCooldownMs = (int)xcat::ClampCombatTeleportCooldownMs(
                    disk.simpleCombatTeleportCooldownMs ? disk.simpleCombatTeleportCooldownMs
                                                          : xcat::kCombatTeleportCooldownDefaultMs);
                mobScanIntervalMs = (int)xcat::ClampMobScanIntervalMs(
                    disk.mobScanIntervalMs ? disk.mobScanIntervalMs
                                           : xcat::kMobScanIntervalDefaultMs);
                // 0=关门控，合法；勿用 ? : 把 0 洗成默认。
                crossLayerFillGateMs =
                    (int)xcat::ClampCombatCrossLayerFillGateMs(disk.simpleCombatCrossLayerFillGateMs);
                fillBudgetPx = (int)xcat::ClampCombatFillBudgetPx(disk.simpleCombatFillBudgetPx);
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
                pumpCongestion = (int)xcat::ClampPumpCongestion(disk.pumpCongestionThreshold);
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
                // 不从 core.autoSell* 灌 UI：真源 [auto_supply]
                lastSeenTick = disk.writeTickMs;
                coreLoaded = true;
            }
        } else if (!coreLoaded) {
            coreLoaded = true;
        }
        if (!asupLoaded) {
            xcat::AutoSupplyConfig as{};
            if (!xcat::ReadAutoSupply(ui.prefsBinDir.c_str(), as))
                xcat::AutoSupplySetDefaults(as);
            autoSell = (as.enabled != 0) || (as.autoSellOnBagFullEnabled != 0);
            sellEquipTrigger = as.sellFreeSlotsAtOrBelow;
            refillHp = as.refillHpEnabled != 0;
            refillMp = as.refillMpEnabled != 0;
            refillCustom = as.refillCustomEnabled != 0;
            refillFeed = as.refillFeedEnabled != 0;
            strncpy_s(refillHpName, as.refillHpName, _TRUNCATE);
            strncpy_s(refillMpName, as.refillMpName, _TRUNCATE);
            strncpy_s(refillCustomName, as.refillCustomName, _TRUNCATE);
            strncpy_s(refillFeedName, as.refillFeedName, _TRUNCATE);
            strncpy_s(refillHpCode, as.refillHpCode, _TRUNCATE);
            strncpy_s(refillMpCode, as.refillMpCode, _TRUNCATE);
            strncpy_s(refillCustomCode, as.refillCustomCode, _TRUNCATE);
            strncpy_s(refillFeedCode, as.refillFeedCode, _TRUNCATE);
            refillHpBuyTo = as.refillHpBuyTo;
            refillMpBuyTo = as.refillMpBuyTo;
            refillCustomBuyTo = as.refillCustomBuyTo;
            refillFeedBuyTo = as.refillFeedBuyTo;
            rechargeStars = as.rechargeStarsEnabled != 0;
            asupTick = as.writeTickMs;
            asupLoaded = true;
        }
    }

    auto persistCore = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.autoLie = autoLie ? 1u : 0u;
        c.invuln = invincible ? 1u : 0u;
        c.attackAccel = attackAccel ? 1u : 0u;
        c.finalAttackForce = finalAttackForce ? 1u : 0u;
        c.skillMaxLevel = skillMaxLevel ? 1u : 0u;
        c.attackAccelCutLayer = gUiAttackAccelCutLayer ? 1u : 0u;
        c.attackAccelSkipPrepare = gUiAttackAccelSkipPrepare ? 1u : 0u;
        c.attackAccelBooster = gUiAttackAccelBooster ? 1u : 0u;
        c.attackSameFrameBurst = xcat::kAttackSameFrameBurstDefault;
        c.fly = fly ? 1u : 0u;
        // 策略入口已隐藏：始终跟随飞；间隔只在调试 TAB 改，这里不覆盖 flyHopCdMs
        c.flyMode = xcat::kFlyModeFollow;
        c.autoEnter = autoEnter ? 1u : 0u;
        c.hpPotion = hpPotion ? 1u : 0u;
        c.mpPotion = mpPotion ? 1u : 0u;
        c.petSummon = petSummon ? 1u : 0u;
        c.petSummonRequireFood = petSummonRequireFood ? 1u : 0u;
        c.simpleCombat = autoCombat ? 1u : 0u;
        c.simpleCombatSmartInterval = smartInterval ? 1u : 0u;
        c.simpleCombatAttackIntervalMs =
            xcat::ClampSimpleCombatAttackIntervalMs(static_cast<uint32_t>(attackMs));
        c.simpleCombatTickMs = xcat::ClampSimpleCombatTickMs(
            static_cast<uint32_t>(gUiCombatTickMs < 0 ? 0 : gUiCombatTickMs));
        c.clusterWeight = clusterWeight ? 1u : 0u;
        c.simpleCombatTeleport = 1u;  // 面板无入口，始终开
        c.simpleCombatLiveStep = gUiCombatLiveStep ? 1u : 0u;
        c.attackRpc = gUiAttackRpc ? 1u : 0u;
        c.attackRpcMobs = xcat::ClampAttackRpcMobs(
            static_cast<uint32_t>(gUiAttackRpcMobs < 0 ? 0 : gUiAttackRpcMobs));
        c.attackRpcIntervalMs = xcat::ClampAttackRpcIntervalMs(
            static_cast<uint32_t>(gUiAttackRpcIntervalMs < 0 ? 0 : gUiAttackRpcIntervalMs));
        c.attackRpcDamage = xcat::ClampAttackRpcDamage(
            static_cast<uint32_t>(gUiAttackRpcDamage < 0 ? 0 : gUiAttackRpcDamage));
        c.simpleCombatTeleportMinDx =
            xcat::ClampCombatTeleportMinDx(static_cast<uint32_t>(teleportMinDx < 0 ? 0 : teleportMinDx));
        c.simpleCombatTeleportStandOff = xcat::ClampCombatTeleportStandOff(
            static_cast<uint32_t>(teleportStandOff < 0 ? 0 : teleportStandOff));
        c.simpleCombatTeleportCooldownMs = xcat::ClampCombatTeleportCooldownMs(
            static_cast<uint32_t>(teleportCooldownMs < 0 ? 0 : teleportCooldownMs));
        c.mobScanIntervalMs = xcat::ClampMobScanIntervalMs(
            static_cast<uint32_t>(mobScanIntervalMs < 0 ? 0 : mobScanIntervalMs));
        c.simpleCombatCrossLayerFillGateMs = xcat::ClampCombatCrossLayerFillGateMs(
            static_cast<uint32_t>(crossLayerFillGateMs < 0 ? 0 : crossLayerFillGateMs));
        c.simpleCombatFillBudgetPx =
            xcat::ClampCombatFillBudgetPx(static_cast<uint32_t>(fillBudgetPx < 0 ? 0 : fillBudgetPx));
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
            static_cast<uint32_t>(pumpCongestion < 0 ? 0 : pumpCongestion));
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
        // 补给开关/店图不写 core：真源 [auto_supply]（persistAsup）。
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            lastSeenTick = c.writeTickMs;
            xcat::log::Ok("App",
                          "已下发 core：测谎=%d 无敌=%d 自动进=%d 加血=%d@%d 加蓝=%d@%d 召宠=%d "
                          "有粮才召=%d 打怪=%d 间隔=%d 贴怪瞬移=1 LiveStep=%d 分区=%d 槽=%d 守护=%d "
                          "瞬移CD=%d 读怪=%d 终极一击=%d 技能满级=%d 加速=%d",
                          autoLie ? 1 : 0, invincible ? 1 : 0, autoEnter ? 1 : 0,
                          hpPotion ? 1 : 0, hpThresholdPct, mpPotion ? 1 : 0, mpThresholdPct,
                          petSummon ? 1 : 0, petSummonRequireFood ? 1 : 0, autoCombat ? 1 : 0,
                          attackMs, gUiCombatLiveStep ? 1 : 0, worldId, charSlot,
                          watchdog ? 1 : 0, teleportCooldownMs, mobScanIntervalMs,
                          finalAttackForce ? 1 : 0, skillMaxLevel ? 1 : 0, attackAccel ? 1 : 0);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] 失败");
        }
    };

    auto persistAsup = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::AutoSupplyConfig as{};
        (void)xcat::ReadAutoSupply(ui.prefsBinDir.c_str(), as);
        as.enabled = autoSell ? 1u : 0u;
        as.autoSellOnBagFullEnabled = autoSell ? 1u : 0u;
        as.shopMapName[0] = '\0';    // 经典版固定自动寻店，不暴露手填店图
        as.returnMapName[0] = '\0';  // 挂机图由打怪/补给行程自动记录，不手填
        as.sellFreeSlotsAtOrBelow = sellEquipTrigger < 0 ? 0 : sellEquipTrigger;
        as.refillHpEnabled = refillHp ? 1u : 0u;
        as.refillMpEnabled = refillMp ? 1u : 0u;
        as.refillCustomEnabled = refillCustom ? 1u : 0u;
        as.refillFeedEnabled = refillFeed ? 1u : 0u;
        strncpy_s(as.refillHpName, refillHpName, _TRUNCATE);
        strncpy_s(as.refillMpName, refillMpName, _TRUNCATE);
        strncpy_s(as.refillCustomName, refillCustomName, _TRUNCATE);
        strncpy_s(as.refillFeedName, refillFeedName, _TRUNCATE);
        strncpy_s(as.refillHpCode, refillHpCode, _TRUNCATE);
        strncpy_s(as.refillMpCode, refillMpCode, _TRUNCATE);
        strncpy_s(as.refillCustomCode, refillCustomCode, _TRUNCATE);
        strncpy_s(as.refillFeedCode, refillFeedCode, _TRUNCATE);
        as.refillHpBuyTo = refillHpBuyTo;
        as.refillMpBuyTo = refillMpBuyTo;
        as.refillCustomBuyTo = refillCustomBuyTo;
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
        }
        as.writeTickMs = GetTickCount64();
        if (!xcat::WriteAutoSupply(ui.prefsBinDir.c_str(), as)) return false;
        asupTick = as.writeTickMs;
        return true;
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
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", lieErr.c_str());
            }
        }
        // 对齐枫星：无敌 + 随机换频同行（攻击加速已挪到「打怪设置」上方独立卡）
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
        ImGui::TextDisabled("(F6 · 跟随飞)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "跟随飞：武装后按冷却跟鼠标连跳（不钉台）。\n"
                "Ctrl/Shift 暂停。间隔在「调试」TAB 调整。");
        }
        if (xcat::ui::OptionCheckbox("自动打怪", &autoCombat)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("游戏内 F5 切换；注入后勾选即时下发");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F5");
        ImGui::SameLine(0.f, ui::Gap() * 1.2f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("瞬移冷却");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(72.f));
        if (ImGui::DragInt("##tp_cd", &teleportCooldownMs, 1,
                           (int)xcat::kCombatTeleportCooldownMinMs,
                           (int)xcat::kCombatTeleportCooldownMaxMs)) {
            teleportCooldownMs = (int)xcat::ClampCombatTeleportCooldownMs(
                static_cast<uint32_t>(teleportCooldownMs < 0 ? 0 : teleportCooldownMs));
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "贴怪瞬移最小间隔（%u–%u ms），按你机器/网络自行评估。\n"
                "调太低（接近 %u）瞬移+出刀会灌爆游戏主线程、拉高 GC 频率，\n"
                "有概率触发游戏「GC」致命弹窗卡死；偏保守更稳。",
                (unsigned)xcat::kCombatTeleportCooldownMinMs,
                (unsigned)xcat::kCombatTeleportCooldownMaxMs,
                (unsigned)xcat::kCombatTeleportCooldownMinMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::TextDisabled("ms");
        ImGui::SameLine(0.f, ui::Gap() * 1.2f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("跨层门控");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(72.f));
        if (ImGui::DragInt("##tp_cross_gate", &crossLayerFillGateMs, 1,
                           (int)xcat::kCombatCrossLayerFillGateMinMs,
                           (int)xcat::kCombatCrossLayerFillGateMaxMs)) {
            crossLayerFillGateMs = (int)xcat::ClampCombatCrossLayerFillGateMs(
                static_cast<uint32_t>(crossLayerFillGateMs < 0 ? 0 : crossLayerFillGateMs));
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "跨层贴稳后额外等待（%u–%u ms），与「瞬移冷却」独立。\n"
                "切段中间跳不武装。0=关闭。偏大则追怪像爬楼梯；偏小跨层连跳更猛。",
                (unsigned)xcat::kCombatCrossLayerFillGateMinMs,
                (unsigned)xcat::kCombatCrossLayerFillGateMaxMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::TextDisabled("ms");
        ImGui::SameLine(0.f, ui::Gap() * 1.2f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("位移预算");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(80.f));
        if (ImGui::DragInt("##tp_dist_budget", &fillBudgetPx, 50, (int)xcat::kCombatFillBudgetPxMin,
                           (int)xcat::kCombatFillBudgetPxMax)) {
            fillBudgetPx = (int)xcat::ClampCombatFillBudgetPx(
                static_cast<uint32_t>(fillBudgetPx < 0 ? 0 : fillBudgetPx));
            persistCore();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("10 秒内瞬移总位移上限（0–%u px）。0=关闭（默认）。\n"
                              "超额则留在原地等额度回来，不弃怪。\n"
                              "怀疑被服端按移动速率判定时才开；调小会明显拖慢追怪。",
                              (unsigned)xcat::kCombatFillBudgetPxMax);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::TextDisabled("px/10s");

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
                "数值越小越快看见新刷怪/尸体消失，抢怪更灵敏；过低更吃 CPU。\n"
                "未开打怪时仍用较慢闲置扫描，不受此值影响。",
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

        if (xcat::ui::OptionCheckbox("自动召唤宠物", &petSummon)) persistCore();
        ImGui::SameLine();
        ImGui::BeginDisabled(!petSummon);
        if (xcat::ui::OptionCheckbox("有粮才召", &petSummonRequireFood)) persistCore();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("喂食交游戏(≈50)");

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
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_pickup", "拾物");
        // 0=关 1=脚下 2=宠吸（3200×2400）3=人物直吸（官方 Send，不靠宠；单选互斥）
        enum : int { kLootOff = 0, kLootFoot = 1, kLootPet = 2, kLootChar = 3 };
        static bool petLootLoaded = false;
        static int lootMode = kLootOff;
        static bool pickupBlacklist = false;
        static int lootIntervalMs = static_cast<int>(xcat::kPetLootIntervalDefaultMs);
        static int lootBurstPerTick = static_cast<int>(xcat::kPetLootBurstDefault);
        static char blacklistKw[256]{};
        static uint64_t petLootTick = 0;
        static bool petLootSaveFailed = false;

        auto modeFromFlags = [](bool pet, bool foot, bool mapVac, bool charVac) -> int {
            // 宠吸 > 人物直吸 > 脚边（与 PetLootNormalize 一致）
            if (pet || mapVac) return kLootPet;
            if (charVac) return kLootChar;
            if (foot) return kLootFoot;
            return kLootOff;
        };

        auto parseBlacklistToCfg = [&](xcat::PetLootConfig& cfg) {
            // 开关只控 skipFilterEnabled；关键词始终落盘，避免取消勾选把输入框规则冲掉。
            cfg.skipFilterEnabled = pickupBlacklist ? 1u : 0u;
            cfg.skipRuleCount = 0;
            if (!blacklistKw[0]) return;
            char buf[256]{};
            strncpy_s(buf, blacklistKw, _TRUNCATE);
            char* ctx = nullptr;
            for (char* tok = strtok_s(buf, ",;| \t", &ctx); tok;
                 tok = strtok_s(nullptr, ",;| \t", &ctx)) {
                while (*tok == ' ' || *tok == '\t') ++tok;
                if (!*tok) continue;
                if (cfg.skipRuleCount >= (uint32_t)xcat::kPetLootMaxSkipRules) break;
                xcat::PetLootSkipRule& r = cfg.skipRules[cfg.skipRuleCount++];
                r = {};
                r.enabled = 1;
                strncpy_s(r.nameKey, tok, _TRUNCATE);
                char* end = nullptr;
                const unsigned long v = strtoul(tok, &end, 10);
                if (end && *end == '\0' && v > 0 && v < 0x7FFFFFFFul) r.itemId = (uint32_t)v;
            }
        };

        auto persistPetLoot = [&]() {
            petLootSaveFailed = false;
            if (ui.prefsBinDir.empty()) return;
            const bool footLoot = (lootMode == kLootFoot);
            const bool petLoot = (lootMode == kLootPet);
            const bool charLoot = (lootMode == kLootChar);
            xcat::PetLootConfig cfg{};
            (void)xcat::ReadPetLoot(ui.prefsBinDir.c_str(), cfg);
            cfg.enabled = petLoot ? 1u : 0u;
            cfg.footEnabled = footLoot ? 1u : 0u;
            cfg.charVacEnabled = charLoot ? 1u : 0u;
            // 宠吸固定近图真空；关宠时清 mapVacuum，避免旧 ini 残留误导
            cfg.mapVacuumEnabled = petLoot ? 1u : 0u;
            cfg.intervalMs = xcat::PetLootClampIntervalMs(
                lootIntervalMs > 0 ? static_cast<uint32_t>(lootIntervalMs) : 0u);
            lootIntervalMs = static_cast<int>(cfg.intervalMs);
            cfg.burstPerTick = xcat::PetLootClampBurstPerTick(
                lootBurstPerTick > 0 ? static_cast<uint32_t>(lootBurstPerTick) : 0u);
            lootBurstPerTick = static_cast<int>(cfg.burstPerTick);
            parseBlacklistToCfg(cfg);
            // WritePetLoot 内部用新 tick 落盘，必须回写到 petLootTick，否则下帧误判
            // disk≠ui → 清空 blacklistKw 再从（可能已被旧逻辑写空的）规则重建。
            cfg.writeTickMs = GetTickCount64();
            if (xcat::WritePetLoot(ui.prefsBinDir.c_str(), cfg)) {
                petLootTick = cfg.writeTickMs;
                xcat::log::Ok("App",
                              "已下发 pet_loot：脚边=%d 宠吸=%d 人物=%d 间隔=%ums 连吸=%u 黑名单=%d "
                              "rules=%u",
                              footLoot ? 1 : 0, petLoot ? 1 : 0, charLoot ? 1 : 0, cfg.intervalMs,
                              cfg.burstPerTick, pickupBlacklist ? 1 : 0, cfg.skipRuleCount);
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
                body = "已切换为宠吸（3200×2400 .rdata 真空）。";
            } else if (next == kLootChar) {
                kind = 2;
                body = "已切换为人物直吸（官方送包，1500×1500，不靠宠物）。";
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
                    const bool diskChar = disk.charVacEnabled != 0;
                    const int activeModes =
                        (diskPet || diskMap ? 1 : 0) + (diskChar ? 1 : 0) + (diskFoot ? 1 : 0);
                    const bool conflict = activeModes > 1;
                    lootMode = modeFromFlags(diskPet, diskFoot, diskMap, diskChar);
                    lootIntervalMs = static_cast<int>(
                        xcat::PetLootClampIntervalMs(disk.intervalMs));
                    lootBurstPerTick = static_cast<int>(
                        xcat::PetLootClampBurstPerTick(disk.burstPerTick));
                    pickupBlacklist = disk.skipFilterEnabled != 0;
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
                        if (off) blacklistKw[off++] = ',';
                        memcpy(blacklistKw + off, piece, strlen(piece));
                        off += strlen(piece);
                        blacklistKw[off] = '\0';
                    }
                    petLootTick = disk.writeTickMs;
                    petLootLoaded = true;
                    // 冲突、或旧「小盒宠吸 / 近图缺 enabled」→ 统一落成宠吸=近图
                    if (conflict || (diskPet && !diskMap) || (diskMap && !diskPet)) {
                        if (conflict && wasLoaded) {
                            notify::PushLocal(/*Warning*/ 2, "petloot-mutex-disk", "拾物互斥",
                                             "配置里多种拾物模式冲突，已按优先级保留一种。", 5000);
                        }
                        persistPetLoot();
                    }
                }
            } else if (!petLootLoaded) {
                petLootLoaded = true;
            }
        }

        {
            const int prevMode = lootMode;
            bool changed = false;
            if (xcat::ui::OptionRadioButton("关闭", &lootMode, kLootOff)) changed = true;
            ImGui::SameLine();
            if (xcat::ui::OptionRadioButton("脚下拾取", &lootMode, kLootFoot)) changed = true;
            ImGui::SameLine();
            if (xcat::ui::OptionRadioButton("宠吸", &lootMode, kLootPet)) changed = true;
            ImGui::SameLine();
            if (xcat::ui::OptionRadioButton("人物直吸", &lootMode, kLootChar)) changed = true;
            if (changed) {
                notifyLootMode(prevMode, lootMode);
                persistPetLoot();
            }
        }
        {
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
                               static_cast<int>(xcat::kPetLootBurstMax)))
                persistPetLoot();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "每拍连调官方吸物次数（%u–%u，默认 %u）。\n"
                    "越大同秒吸越多，发包更密，打怪同开时尖峰更高。",
                    (unsigned)xcat::kPetLootBurstMin, (unsigned)xcat::kPetLootBurstMax,
                    (unsigned)xcat::kPetLootBurstDefault);
            }
            ImGui::SameLine(0.f, ui::Gap() * 0.35f);
            ImGui::TextUnformatted("次/拍");
        }
        if (xcat::ui::OptionCheckbox("启用拾取黑名单", &pickupBlacklist)) persistPetLoot();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##bl",
                                     "itemId/关键词（逗号分隔；金币填 2147483647；脚边+宠+人物共用）",
                                     blacklistKw, sizeof(blacklistKw))) {
            // debounce on deactivate / checkbox
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistPetLoot();
        if (petLootSaveFailed)
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "保存 user.ini [pet_loot] 失败");
    }
    CardGap();
    {
        // 一行：启用 + 间隔；跳过动作等待随启用隐含开启，无单独入口
        xcat::ui::CardGuard card("##tab_home_attack_accel", "攻击加速");
        if (xcat::ui::OptionCheckbox("启用", &attackAccel)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "开启后自动跳过动作等待（清忙锁）并抬动作攻速。\n"
                "与是否开「自动打怪」无关——走路/落地也会生效。\n"
                "出刀频率看右侧间隔；过短易空砍/踢号。\n"
                "进图落地约 0.4s 内暂停写入，降低脱同步。");
        }
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("间隔");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(64.f));
        if (ImGui::DragInt("##atk_ms", &attackMs, 1,
                           (int)xcat::kSimpleCombatAttackIntervalMinMs, 10000))
            persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "自动出刀间隔（%u–10000 ms，默认 %ums）。\n"
                "下限 %u ms（与 hold 地板一致）；过短易空砍/踢号。",
                (unsigned)xcat::kSimpleCombatAttackIntervalMinMs,
                (unsigned)xcat::kSimpleCombatAttackIntervalDefaultMs,
                (unsigned)xcat::kSimpleCombatAttackIntervalMinMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("ms");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_final_attack", "终极一击");
        if (xcat::ui::OptionCheckbox("普攻必出终极一击", &finalAttackForce)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "已学终极攻击（如狂战士終極之劍/斧）：Prop→100，并强制注册出刀。\n"
                "数据面改 SkillLevelData / FinalAttack 结构体，不改 GameAssembly 代码。\n"
                "需已学习对应武器的终极技能；关掉后会尽量还原原 Prop。\n"
                "服端若校验伤害/技能，以服为准。");
        }
        ImGui::TextDisabled("狂战士劍/斧 · Prop100 + 强制注册出刀");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_skill_max", "技能满级");
        if (xcat::ui::OptionCheckbox("已学技能按满级生效", &skillMaxLevel)) persistCore();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "A：SkillRecord/Ex 已学技能（≥1）等级写成满级。\n"
                "B：Hook GetSkillLevel 作 fallback（抗同步打回）。\n"
                "日志 SkillMax src=dict|hook|dict+hook。\n"
                "关掉还原字典原等级并卸钩；服端结算以服为准。");
        }
        ImGui::TextDisabled("dict + GetSkillLevel hook · src 见日志");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_combat", "打怪设置");

        if (xcat::ui::OptionCheckbox("智能间隔", &smartInterval)) persistCore();
        ImGui::SameLine();
        ImGui::TextDisabled("在攻击间隔附近 ±40ms 抖动");

        ImGui::Spacing();
        ImGui::TextUnformatted("贴怪位移");
        ImGui::TextDisabled("贴怪瞬移默认开（无面板开关）；fill+Doing 落怪侧");
        ImGui::Spacing();

        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("站位偏移");
            ImGui::SameLine(0.f, ui::Gap());
            ImGui::SetNextItemWidth(AppDpi_Px(72.f));
            if (ImGui::DragInt("##tp_off", &teleportStandOff, 1, 12, 200)) persistCore();
            ImGui::SameLine(0.f, ui::Gap() * 0.45f);
            ImGui::TextDisabled("px · 落在怪左/右侧，禁怪心");
            // 触发距离已不再参与选靶/贴怪门控，保留落盘字段但不展示，避免语义误导。
            (void)teleportMinDx;
        }

        ImGui::Spacing();
        {
            bool clusterPri = clusterWeight != 0;
            if (xcat::ui::OptionCheckbox("群怪优先", &clusterPri)) {
                clusterWeight = clusterPri ? 1 : 0;
                persistCore();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("同层先打堆；关=纯最近");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "开：同 zMass 轮次内，优先周围 150px 内活怪更多的目标\n"
                    "关：只按距离/hop 选最近可打怪\n"
                    "仍保持同层优先，不会为了群怪先跨层");
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
                    "开着攻击加速时更有用：命中后提前切下一只，少站着干等。\n"
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

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("拥堵让路阈值");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##pump_congestion", &pumpCongestion, 1,
                           (int)xcat::kPumpCongestionMin, (int)xcat::kPumpCongestionMax)) {
            pumpCongestion = (int)xcat::ClampPumpCongestion(
                static_cast<uint32_t>(pumpCongestion < 0 ? 0 : pumpCongestion));
            persistCore();
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

        static bool pickupPriority = false;
        xcat::ui::OptionCheckbox("拾取优先", &pickupPriority);
        ImGui::TextDisabled("粘怪 / 低血撤 / 限定区间：Classic 锚点确认后再接");
    }
    CardGap();
    {
        // 布局对齐对照仓 xcat_for_fengxing「卖背包 / 自动补给」卡片分区与按钮几何；
        // 产品语义：经典版手动卖 + 自动回城卖/补给（Il2Cpp 买卖）。
        xcat::ui::CardGuard card("##tab_home_sell", "卖背包 / 自动补给");
        ImGui::TextWrapped(
            "上面填「不卖名单」；下面分手动卖、自动回城卖。两边共用同一份名单。");

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
            std::istringstream in(text ? text : "");
            std::string token;
            while (in >> token &&
                   cfg.keepRuleCount < static_cast<uint32_t>(xcat::kSellbagMaxKeepRules)) {
                auto& r = cfg.keepRules[cfg.keepRuleCount++];
                r.enabled = 1;
                r.targetMask = xcat::kSellbagBagAll;
                strncpy_s(r.nameKey, token.c_str(), _TRUNCATE);
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

        // —— 不卖名单（对照仓 DrawSellbagKeepRulesEditor）——
        ImGui::TextUnformatted("不卖名单");
        ImGui::SameLine();
        ImGui::TextDisabled("（手动卖 / 自动卖共用）");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##sellbag_keep_rules_quick",
                                 "不卖关键词，空格分隔：披風 卷軸 藥水", keepRulesBuf,
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
                    std::istringstream in(keepRulesBuf);
                    std::string token;
                    while (in >> token) {
                        SellbagKeepHitPreview h{};
                        h.key = token;
                        std::vector<std::string> codes;
                        h.hit = xcat::ItemCatalogCollectCodesByNameContains(pack, token.c_str(),
                                                                            codes, 1);
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
        ImGui::TextUnformatted("手动卖出");
        ImGui::TextDisabled("需先打开 NPC 商店，再点下方按钮。");

        const bool busy = [&]() {
            if (ui.prefsBinDir.empty()) return false;
            xcat::PayloadStatus st{};
            return xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) && st.sellbagBusy != 0;
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

        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.25f));

        // —— 自动回城卖 / 补给（对照仓 DrawNativeHomeTab 同区）——
        ImGui::TextUnformatted("自动回城卖 / 补给");
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
                "自动寻最近可卖店（离线种子）\n"
                "进城后先卖装备，再顺便卖其他，并补 1 张回城卷\n"
                "可选充飞镖 / 补红/蓝/自定义/饲料：店内有则买、没有就跳过\n"
                "到店后会自动对话开店，并尝试点选「商店」菜单\n"
                "不卖名单与上方手动卖共用");
        }
        {
            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
            if (NativeInputIntClamped("##home_asup_equip_x", sellEquipTrigger, 0, 300)) {
                persistAsup();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("装备栏件数>=X 触发(0为包满自动卖)");
            ImGui::EndGroup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "只看装备栏已用件数，其他栏不参与触发\n"
                    "· 0 = 装备栏满装才卖\n"
                    "· 例如 X=20 = 装备达到 20 件就回城\n"
                    "拖动或双击输入后，松开/点别处（失焦）才保存生效\n"
                    "进城后卖装备，并顺便卖其他栏；需勾选「自动卖」");
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

        // 单行：勾选 + 补到数量 + 物品名（剩余宽度）+ 匹配态（对照仓 drawRefillRow）
        auto drawRefillRow = [&](const char* label, const char* idSuffix, bool* en, char* nameBuf,
                                 size_t nameSz, char* codeBuf, size_t codeSz, int* buyTo) {
            const float fs = ImGui::GetFontSize();
            const float rowGap = ui::Gap();
            char chkId[64]{};
            snprintf(chkId, sizeof(chkId), "%s##home_asup_%s", label, idSuffix);
            if (ImGui::Checkbox(chkId, en)) persistAsup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "可选项。物品名须与离线表中文名完全一致（非 CODE）\n"
                    "失焦后反查 CODE 缓存；先卖后买，钱不够时红/蓝/自定义/饲料按费用比例分配");
            }
            ImGui::SameLine(0.f, rowGap * 0.55f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("补到");
            ImGui::SameLine(0.f, rowGap * 0.35f);
            ImGui::SetNextItemWidth(fs * 3.2f);
            char qtyId[64]{};
            snprintf(qtyId, sizeof(qtyId), "##home_asup_%s_qty", idSuffix);
            if (NativeInputIntClamped(qtyId, *buyTo, 0, 9999)) persistAsup();

            const char* matchLabel = nullptr;
            float matchW = 0.f;
            if (nameBuf[0]) {
                matchLabel = codeBuf[0] ? "已匹配" : "未匹配";
                matchW = ImGui::CalcTextSize(matchLabel).x + rowGap * 0.45f;
            }

            ImGui::SameLine(0.f, rowGap * 0.45f);
            char nameId[64]{};
            snprintf(nameId, sizeof(nameId), "##home_asup_%s_name", idSuffix);
            const float nameW = (std::max)(fs * 4.f, ImGui::GetContentRegionAvail().x - matchW);
            ImGui::SetNextItemWidth(nameW);
            ImGui::InputTextWithHint(nameId, "精确中文物品名", nameBuf, (int)nameSz);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (!resolveRefillCode(nameBuf, codeBuf, codeSz)) codeBuf[0] = '\0';
                persistAsup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && codeBuf[0]) {
                ImGui::SetTooltip("CODE %s", codeBuf);
            }

            if (matchLabel) {
                ImGui::SameLine(0.f, rowGap * 0.45f);
                ImGui::AlignTextToFramePadding();
                if (codeBuf[0]) {
                    ImGui::TextDisabled("%s", matchLabel);
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "%s", matchLabel);
                }
            }
        };

        auto drawFeedRefillRow = [&]() {
            const float fs = ImGui::GetFontSize();
            const float rowGap = ui::Gap();
            if (ImGui::Checkbox("自动补饲料##home_asup_feed", &refillFeed)) {
                strncpy_s(refillFeedName, xcat::kAutoSupplyDefaultRefillFeedName, _TRUNCATE);
                strncpy_s(refillFeedCode, xcat::kAutoSupplyDefaultRefillFeedCode, _TRUNCATE);
                persistAsup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "默认开启。持有量按「美味飼料+寵物食品」合计对照「补到」\n"
                    "店内优先买「美味飼料」，没有则买「寵物食品」，都没有会提示并跳过\n"
                    "先卖后买；钱不够时与红/蓝/自定义按费用比例分配");
            }
            ImGui::SameLine(0.f, rowGap * 0.55f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("补到");
            ImGui::SameLine(0.f, rowGap * 0.35f);
            ImGui::SetNextItemWidth(fs * 3.2f);
            if (NativeInputIntClamped("##home_asup_feed_qty", refillFeedBuyTo, 0, 9999)) {
                persistAsup();
            }

            char disp[96]{};
            snprintf(disp, sizeof(disp), "%s（次选%s）", xcat::kAutoSupplyDefaultRefillFeedName,
                     xcat::kAutoSupplyDefaultRefillFeedAltName);
            const char* fixedLabel = "固定";
            const float fixedW = ImGui::CalcTextSize(fixedLabel).x + rowGap * 0.45f;
            ImGui::SameLine(0.f, rowGap * 0.45f);
            ImGui::SetNextItemWidth((std::max)(fs * 4.f, ImGui::GetContentRegionAvail().x - fixedW));
            ImGui::BeginDisabled(true);
            ImGui::InputText("##home_asup_feed_name", disp, sizeof(disp),
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("CODE %s → 次选 %s", xcat::kAutoSupplyDefaultRefillFeedCode,
                                  xcat::kAutoSupplyDefaultRefillFeedAltCode);
            }
            ImGui::SameLine(0.f, rowGap * 0.45f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", fixedLabel);
        };

        drawRefillRow("自动补红", "hp", &refillHp, refillHpName, sizeof(refillHpName),
                      refillHpCode, sizeof(refillHpCode), &refillHpBuyTo);
        drawRefillRow("自动补蓝", "mp", &refillMp, refillMpName, sizeof(refillMpName),
                      refillMpCode, sizeof(refillMpCode), &refillMpBuyTo);
        drawRefillRow("自动补自定义物品", "custom", &refillCustom, refillCustomName,
                      sizeof(refillCustomName), refillCustomCode, sizeof(refillCustomCode),
                      &refillCustomBuyTo);
        drawFeedRefillRow();

        if (ImGui::Checkbox("卖装后自动充飞镖##home_asup_stars", &rechargeStars)) persistAsup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "卖装开店后扫描消耗栏手里剑，对可充值的飞镖执行充值（Charge）\n"
                "Charge 入口尚未钉死时会跳过并打日志\n"
                "需勾选「自动卖」或点「立即回城卖装一趟」才会进店");
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
            } else if (autoSell) {
                autoSell = false;  // Stop 已写 enabled=0；只同步本地勾选
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "立即停止进行中的自动动作（卖装/补给/超级赶路等）：\n"
                "· 关掉「自动卖」开关\n"
                "· 中止回城/超级赶路/开店/买卖\n"
                "· 取消待续回挂机图\n"
                "· 恢复战斗与飞行暂停\n"
                "若已在用回城卷换图，客户端仍会卸图完成（我方不再追加动作）");
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

    static bool detect = false;
    static bool stopCombat = true;
    static bool channelHop = true;
    static bool hideOthers = false;
    static std::string s_loadedBin;
    static uint64_t s_lastTick = 0;
    static bool s_saveFailed = false;

    auto loadUi = [&]() {
        if (ui.prefsBinDir.empty()) {
            detect = false;
            stopCombat = true;
            channelHop = true;
            hideOthers = false;
            s_lastTick = 0;
            return;
        }
        xcat::PayloadControl c{};
        if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
            xcat::PayloadControlSetDefaults(c);
        }
        detect = c.autoRelogin != 0;
        stopCombat = c.autoReloginStopCombat != 0;
        channelHop = c.autoReloginReconnect != 0;
        hideOthers = c.hideOtherPlayers != 0;
        s_lastTick = c.writeTickMs;
    };

    auto saveUi = [&]() -> bool {
        if (ui.prefsBinDir.empty()) return false;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.autoRelogin = detect ? 1u : 0u;
        c.autoReloginStopCombat = stopCombat ? 1u : 0u;
        c.autoReloginReconnect = channelHop ? 1u : 0u;
        c.hideOtherPlayers = hideOthers ? 1u : 0u;
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

    xcat::ui::CardGuard card("##tab_relogin", "有人来时怎么办");
    if (xcat::ui::OptionCheckbox("检测同图玩家", &detect)) trySaveOrRevert();
    ImGui::Separator();
    ImGui::TextUnformatted("处理流程");
    if (xcat::ui::OptionCheckbox("先停手", &stopCombat)) trySaveOrRevert();
    if (xcat::ui::OptionCheckbox("一直有人就换频", &channelHop)) trySaveOrRevert();
    ImGui::Separator();
    if (xcat::ui::OptionCheckbox("隐藏同图其他玩家", &hideOthers)) trySaveOrRevert();
    ImGui::TextDisabled("藏皮/伤字(DamageSkin)/技能特效；自己可见；不影响遇人人数检测。");
    ImGui::TextDisabled("同图 UserPool 远程人数；换频走直调发包（无菜单）。");
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
            if (it.key.empty() || it.key == "000000000") continue;
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
    if (!catalog.empty() && !ui.prefsBinDir.empty()) {
        xcat::PayloadStatus st{};
        if (xcat::ReadPayloadStatus(ui.prefsBinDir.c_str(), st) &&
            xcat::PayloadStatusHeartbeatFresh(st, GetTickCount64(), 5000) && st.mapId > 0 &&
            st.mapId != lastAutoSyncMapId) {
            const std::string prevKey =
                lastAutoSyncMapId ? padKey(std::to_string(lastAutoSyncMapId)) : std::string{};
            const bool follow =
                selected < 0 ||
                (!prevKey.empty() && selected < static_cast<int>(catalog.size()) &&
                 catalog[selected].key == prevKey);
            const std::string key = padKey(std::to_string(st.mapId));
            if (follow) {
                if (syncCatalogToMapKey(key)) lastAutoSyncMapId = st.mapId;
            } else {
                lastAutoSyncMapId = st.mapId;  // 只记边沿，不改手选
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
    static bool auctionTownBypass = false;
    static bool frameLock = false;
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
                frameLock = disk.frameLock != 0;
                frameLockFps = (int)xcat::ClampFrameLockFps(
                    disk.frameLockFps ? disk.frameLockFps : xcat::kFrameLockFpsDefault);
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
        c.frameLock = frameLock ? 1u : 0u;
        c.frameLockFps = xcat::ClampFrameLockFps(
            static_cast<uint32_t>(frameLockFps < 0 ? 0 : frameLockFps));
        frameLockFps = (int)c.frameLockFps;
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            dropSeenTick = c.writeTickMs;
            xcat::log::Ok("App",
                          "已下发 core：战斗中可丢物=%d 野外可开拍卖=%d 引擎帧率锁=%d fps=%u",
                          dropInCombat ? 1 : 0, auctionTownBypass ? 1 : 0, frameLock ? 1 : 0,
                          c.frameLockFps);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] drop/auction/frameLock 失败");
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
            dropSeenTick = c.writeTickMs;
            xcat::log::Ok("App", "已下发 core：attackRpc=%d mobs=%u ms=%u dmg=%u（实验）",
                          gUiAttackRpc ? 1 : 0, c.attackRpcMobs, c.attackRpcIntervalMs,
                          c.attackRpcDamage);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] attackRpc 失败");
        }
    };

    {
    xcat::ui::CardGuard card("##tab_beta", "实验功能");
        if (xcat::ui::OptionCheckbox("战斗中可丢物", &dropInCombat)) persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "清 LocalUser 警戒时间戳：战斗中可丢物，并抑制客户端警戒\n"
                "（打怪后警戒很快解除属预期）。仅客户端；服务端 Drop 权威不变。默认开。");
        }
        if (xcat::ui::OptionCheckbox("野外可开拍卖（仅客户端）", &auctionTownBypass))
            persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
            "静默绕过：开启后点游戏状态栏拍卖按钮，野外也会发迁移包(0x002E)\n"
            "机制：零.text，维持 MapDataInfo.IsTown=1 并清 Option&0x10\n"
            "仅客户端门控。服端常拒/断线(含 GlobalMarketTerminated)；\n"
            "若开着「守护模式」会把断线当踢线→5秒干净重拉（像被杀死）。\n"
            "挂机/守护期间建议关。默认关。\n"
            "开启期间其它读 IsTown/该 Option 位的逻辑也会受影响。");
        }
        ImGui::Separator();
        if (xcat::ui::OptionCheckbox("引擎帧率锁", &frameLock)) persistDrop();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "锁 Unity 主循环目标帧率（Application.targetFrameRate），并关闭引擎 vSync。\n"
                "不修改显示器硬件刷新率。用于高低配显示器对齐打怪节奏。\n"
                "预设 120 / 240 / 360 / 480；也可自定义（%u~%u）。默认关。\n"
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
                if (ImGui::Button(lab, ImVec2(48.f, 0))) {
                    frameLockFps = fps;
                    persistDrop();
                }
                if (sel) ImGui::PopStyleColor();
            };
            ImGui::TextUnformatted("预设");
            ImGui::SameLine();
            presetBtn(120);
            ImGui::SameLine();
            presetBtn(240);
            ImGui::SameLine();
            presetBtn(360);
            ImGui::SameLine();
            presetBtn(480);
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
        xcat::ui::CardGuard card("##tab_beta_skip_prepare", "跳过 Prepare");
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

        if (xcat::ui::OptionCheckbox("跳过 Prepare", &gUiAttackAccelSkipPrepare))
            persistSkipPrep();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：钉 LocalUser 虚表槽，拦住攻击类 PrepareActionLayer，\n"
                "出刀仍写忙锁，但不建攻击动作层（无抬手）；跳过时清忙锁并回 Idle。\n"
                "Idle(action=6) 永远透传；进图/换图落地约 1s 内不武装。\n"
                "勾选从关→开也会重开落地窗。与下方「砍动画倒计时」互斥（开跳过则不砍层）。\n"
                "不改攻击加速的清忙锁/攻速；关勾选即透传原 Prepare。\n"
                "禁止 GA .text；防漂移装钩（哈希/RVA/扫槽）。");
        }
        ImGui::TextDisabled("Idle 透传；落地后才跳过攻击 Prepare");
    }

    CardGap();
    {
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

        if (xcat::ui::OptionCheckbox("砍动画倒计时", &gUiAttackAccelCutLayer))
            persistCutLayer();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：周期把动作层 layer+0x14 倒计时置 0，\n"
                "逼动画帧尽快推进，减轻连挥堆叠（偏视觉）。\n"
                "不改攻击加速的清忙锁/攻速逻辑。\n"
                "可能空砍/皮抽；可单独开，不依赖攻击加速「启用」。\n"
                "会连带催快待机呼吸——更想「无动画」请用上方「跳过 Prepare」。\n"
                "与「跳过 Prepare」互斥（开跳过则不砍层）。");
        }
        ImGui::TextDisabled("默认关；与「跳过 Prepare」勿同时开");
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
                    gUiAttackAccelBooster = disk.attackAccelBooster != 0;
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
            c.attackAccelBooster = gUiAttackAccelBooster ? 1u : 0u;
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

        if (xcat::ui::OptionCheckbox("攻速槽 nBooster_", &gUiAttackAccelBooster))
            persistBooster();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "实验项（默认关）：写 SecondaryStat.nBooster_=-8，把攻速 degree 夹到最快的 2，\n"
                "攻击延迟 ×0.75；到期时间按游戏钟每拍续 60s，关勾选时原值奉还。\n"
                "与「启用」（清忙锁）完全独立，就是为了能分别开来做对照：\n"
                "实测「启用」开着时本项净收益为 0 —— 忙锁一清，引擎那道延迟闸就没了。\n"
                "它真正的用法是**替掉**「启用」：只开本项，不碰动作忙锁，约慢 5ms 但更干净。\n"
                "注意 -8 超出合法 booster 值域（正常只有 -1/-2），存在被识别的风险。");
        }
        ImGui::TextDisabled("对照用：与「启用」分开开关，可单独开");
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_beta_combat_exp", "打怪实验");
        // 贴怪瞬移产品常开；LiveStep 仍默认关。
        if (xcat::ui::OptionCheckbox("LiveStep 跟位", &gUiCombatLiveStep)) {
            if (ui.prefsBinDir.empty()) {
                // no-op
            } else {
                xcat::PayloadControl c{};
                (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
                c.simpleCombatTeleport = 1u;
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
                "开：怪挪了也在同层用瞬移冷却跟位；hop<80 才走短收态");
        }

        if (xcat::ui::OptionCheckbox("攻包伪造探针", &gUiAttackRpc)) persistAttackRpc();
        ImGui::SameLine();
        ImGui::TextDisabled("伪造攻击事件 Create(50)；服端自算伤（默认关）");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "实验项：伪造普通攻击出站包（事件），不是伪造伤害数字\n"
                "opcode=50；先 SetAttackAction+Collect，再 SendOutPacket\n"
                "成功标志：x.jsonl AttackRpc + SetAttackAction + forge BODY + normal ok\n"
                "单次勾选满 2 次 ok 自动关（防延后踢）；间隔建议 ≥800ms\n"
                "也可设环境变量 ATTACK_RPC=1");
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
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_token", "TOKEN");
        static char passBuf[64]{};
        static bool passLoaded = false;
        static std::string passLoadedFor;
        if (!ui.prefsBinDir.empty() && (!passLoaded || passLoadedFor != ui.prefsBinDir)) {
            const std::string cur = xcat::app::LoadOpsToken(ui.prefsBinDir);
            std::snprintf(passBuf, sizeof(passBuf), "%s", cur.c_str());
            passLoaded = true;
            passLoadedFor = ui.prefsBinDir;
        }
        ImGui::SetNextItemWidth(AppDpi_Px(220.f));
        ImGui::InputTextWithHint("##ops_token", "TOKEN", passBuf, sizeof(passBuf),
                                 ImGuiInputTextFlags_Password);
        const bool passCommit = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ImGui::Button("保存##ops_token") || passCommit) {
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warn*/ 2, "ops-token", "无法保存", "未定位数据目录", 3500);
            } else if (xcat::app::SaveOpsToken(ui.prefsBinDir, passBuf)) {
                const std::string norm = xcat::app::NormalizeOpsToken(passBuf);
                std::snprintf(passBuf, sizeof(passBuf), "%s", norm.c_str());
                notify::PushLocal(/*Ok*/ 1, "ops-token", "已保存", "", 2500);
            } else {
                notify::PushLocal(/*Danger*/ 3, "ops-token", "保存失败", "", 3500);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("清空##ops_token_clr")) {
            passBuf[0] = '\0';
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warn*/ 2, "ops-token", "无法清空", "未定位数据目录", 3500);
            } else if (xcat::app::SaveOpsToken(ui.prefsBinDir, "")) {
                notify::PushLocal(/*Ok*/ 1, "ops-token", "已清空", "", 2500);
            } else {
                notify::PushLocal(/*Danger*/ 3, "ops-token", "清空失败", "", 3500);
            }
        }
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_combat_tick", "打怪节奏");
        static bool tickDbgLoaded = false;
        static uint64_t tickDbgSeen = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!tickDbgLoaded || disk.writeTickMs != tickDbgSeen) {
                    gUiCombatTickMs = (int)xcat::ClampSimpleCombatTickMs(
                        disk.simpleCombatTickMs ? disk.simpleCombatTickMs
                                               : xcat::kSimpleCombatTickDefaultMs);
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

        auto persistTiming = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.simpleCombatTickMs = xcat::ClampSimpleCombatTickMs(
                static_cast<uint32_t>(gUiCombatTickMs < 0 ? 0 : gUiCombatTickMs));
            gUiCombatTickMs = (int)c.simpleCombatTickMs;
            c.simpleCombatAttackHoldMs = xcat::ClampAttackHoldMs(
                static_cast<uint32_t>(gUiAttackHoldMs < 0 ? 0 : gUiAttackHoldMs));
            gUiAttackHoldMs = (int)c.simpleCombatAttackHoldMs;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                tickDbgSeen = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：simpleCombatTickMs=%u hold=%u（调试）",
                              c.simpleCombatTickMs, c.simpleCombatAttackHoldMs);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] 打怪节奏参数失败");
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("TICK值");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_combat_tick_ms", &gUiCombatTickMs, 1,
                           (int)xcat::kSimpleCombatTickMinMs, (int)xcat::kSimpleCombatTickMaxMs))
            persistTiming();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "打怪状态机心跳（%u–%u ms，默认 %u）。\n"
                "与是否开攻击加速无关；越短出刀机会越多。\n"
                "下限 %u ms；过短更吃 CPU/主线程。",
                (unsigned)xcat::kSimpleCombatTickMinMs, (unsigned)xcat::kSimpleCombatTickMaxMs,
                (unsigned)xcat::kSimpleCombatTickDefaultMs, (unsigned)xcat::kSimpleCombatTickMinMs);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("ms");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("全局心跳 · 非仅加速");

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("按键hold");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_attack_hold_ms", &gUiAttackHoldMs, 1,
                           (int)xcat::kAttackHoldMinMs, (int)xcat::kAttackHoldMaxMs))
            persistTiming();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "出刀按键按下到松开的时长（%u–%u ms，默认 %u）。\n"
                "实际取 min(此值, 出刀间隔)：hold ≥ 间隔会把下一刀锁死。\n"
                "注意：开着「攻击加速」时走 Down+Up 同泵的 pulse 路径，hold=0，此项不参与；\n"
                "只有关掉加速、走异步松键时才有效。\n"
                "调太小可能个别刀不被引擎识别（看日志 whiff 是否变多）。",
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
        xcat::ui::CardGuard card("##tab_dbg_pump_drain", "主线程泵 Drain");
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
                    drainDbgSeen = disk.writeTickMs;
                    drainDbgLoaded = true;
                }
            } else if (!drainDbgLoaded) {
                drainDbgLoaded = true;
            }
        } else if (!drainDbgLoaded) {
            drainDbgLoaded = true;
        }

        auto persistDrain = [&]() {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.pumpDrainBudget = xcat::ClampPumpDrainBudget(
                static_cast<uint32_t>(pumpDrainBudget < 0 ? 0 : pumpDrainBudget));
            pumpDrainBudget = (int)c.pumpDrainBudget;
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                drainDbgSeen = c.writeTickMs;
                xcat::log::Ok("App", "已下发 core：pumpDrainBudget=%u（调试）",
                              c.pumpDrainBudget);
            } else {
                xcat::log::Warn("App", "写入 user.ini [core] pumpDrainBudget 失败");
            }
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("每 tick Drain");
        ImGui::SameLine(0.f, ui::Gap() * 0.45f);
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("##dbg_pump_drain", &pumpDrainBudget, 1,
                           (int)xcat::kPumpDrainBudgetMin, (int)xcat::kPumpDrainBudgetMax)) {
            pumpDrainBudget = (int)xcat::ClampPumpDrainBudget(
                static_cast<uint32_t>(pumpDrainBudget < 0 ? 0 : pumpDrainBudget));
            persistDrain();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "主线程泵每个宿主 tick 最多执行多少个排队 job（%u–%u，默认 %u=抽干整队）。\n"
                "队列共 %u 格。调小=单帧更轻、易 defer/跳刀；调大=吞吐更高、单帧可能更尖。\n"
                "与首页「拥堵让路阈值」无关：那是背压，这项是每 tick 清队上限。",
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
                           (int)xcat::kCombatTeleportMaxHopMin,
                           (int)xcat::kCombatTeleportMaxHopMax)) {
            teleportMaxHop = (int)xcat::ClampCombatTeleportMaxHop(
                static_cast<uint32_t>(teleportMaxHop < 0 ? 0 : teleportMaxHop));
            persistHop();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "贴怪单次瞬移最大距离（%u–%u px，默认 %u）。\n"
                "更远会分段贴近；过大易软断（lean_local_or_soft）。",
                (unsigned)xcat::kCombatTeleportMaxHopMin, (unsigned)xcat::kCombatTeleportMaxHopMax,
                (unsigned)xcat::kCombatTeleportMaxHopDefault);
        }
        ImGui::SameLine(0.f, ui::Gap() * 0.35f);
        ImGui::TextUnformatted("px");
        ImGui::SameLine(0.f, ui::Gap());
        ImGui::TextDisabled("建议 350–550 · 默认 %u（盖中→顶层）",
                            (unsigned)xcat::kCombatTeleportMaxHopDefault);
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
        static bool flyDbgLoaded = false;
        static uint64_t flyDbgTick = 0;
        if (!ui.prefsBinDir.empty()) {
            xcat::PayloadControl disk{};
            if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
                if (!flyDbgLoaded || disk.writeTickMs != flyDbgTick) {
                    flyHopCdMs = (int)xcat::ClampFlyHopCdMs(
                        disk.flyHopCdMs ? disk.flyHopCdMs : xcat::kFlyHopCdDefaultMs);
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
            c.flyMode = xcat::kFlyModeFollow;
            c.flyHopCdMs = xcat::ClampFlyHopCdMs(
                static_cast<uint32_t>(flyHopCdMs < 0 ? 0 : flyHopCdMs));
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) flyDbgTick = c.writeTickMs;
        };

        xcat::ui::CardGuard card("##tab_dbg_fly", "飞行调试");
        ImGui::TextUnformatted("跟随飞间隔");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(72.f));
        if (ImGui::DragInt("##dbg_fly_hop_cd", &flyHopCdMs, 1, (int)xcat::kFlyHopCdMinMs,
                           (int)xcat::kFlyHopCdMaxMs)) {
            flyHopCdMs = (int)xcat::ClampFlyHopCdMs(
                static_cast<uint32_t>(flyHopCdMs < 0 ? 0 : flyHopCdMs));
            persistFlyDbg();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("ms");
        ImGui::TextDisabled("首页仅保留飞行开关；策略固定跟随飞。范围 %u–%u，默认 %u",
                            xcat::kFlyHopCdMinMs, xcat::kFlyHopCdMaxMs,
                            xcat::kFlyHopCdDefaultMs);
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
        auto bumpSeq = [&](bool alarm) {
            if (ui.prefsBinDir.empty()) return;
            xcat::PayloadControl c{};
            (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
            c.autoLieDryRun = dryRun ? 1u : 0u;
            if (alarm) {
                ++c.autoLieAlarmTestSeq;
                if (c.autoLieAlarmTestSeq == 0) c.autoLieAlarmTestSeq = 1;
            } else {
                ++c.autoLieMouseSmokeSeq;
                if (c.autoLieMouseSmokeSeq == 0) c.autoLieMouseSmokeSeq = 1;
            }
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
            const float btnW = (std::max)(1.f, (rowW - gap * 3.f) * 0.25f);
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
                bumpSeq(true);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("需已注入：约 12 秒通知+Alarm 音效（每 3s），不答题。");
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("光标烟测", ImVec2(btnW, 0.f))) {
                bumpSeq(false);
                notify::PushLocal(/*Warning*/ 2, "lie-mouse-smoke", "光标烟测",
                                  "约 3 秒；仅游戏前台锁光标。需已注入。", 4500);
            }
        }
    }
    CardGap();
    {
        // 瞬移探针 / 踢号压测：从首页挪出，避免占用日常挂机界面
        auto bumpTpSeq = [&](const char* which) {
            if (ui.prefsBinDir.empty()) {
                notify::PushLocal(/*Warning*/ 2, "tp-dbg", "下发失败", "无数据目录", 3000);
                return;
            }
            const RuntimeLeds leds = QueryRuntimeLeds(ui.prefsBinDir.c_str());
            if (leds.gamePid == 0) {
                notify::PushLocal(/*Warning*/ 2, "tp-dbg", "下发失败", "需已注入游戏", 3000);
                return;
            }
            xcat::PayloadControl c{};
            if (!xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c)) {
                xcat::PayloadControlSetDefaults(c);
            }
            auto bump = [](uint32_t& seq) {
                seq = seq == 0 ? 1u : seq + 1u;
                if (seq == 0) seq = 1u;
            };
            const char* okTitle = "已下发";
            const char* okBody = "";
            const char* tag = "tp-dbg";
            if (strcmp(which, "test") == 0) {
                bump(c.teleportTestSeq);
                tag = "tp-test";
                okTitle = "测试贴怪瞬移已下发";
                okBody = "一跳到位后恢复走路；见 combat.log";
                xcat::log::Ok("App", "teleportTestSeq=%u", c.teleportTestSeq);
            } else if (strcmp(which, "native") == 0) {
                bump(c.teleportNativeTestSeq);
                tag = "tp-native";
                okTitle = "原生瞬移CALL已下发";
                okBody = "短距 WZ range；见 combat.log";
                xcat::log::Ok("App", "teleportNativeTestSeq=%u", c.teleportNativeTestSeq);
            } else if (strcmp(which, "wide") == 0) {
                bump(c.teleportKickStressSeq);
                tag = "tp-kick";
                okTitle = "踢号压测已下发（再点停止）";
                okBody = "随机贴怪 2000→50ms；见 combat.log kick_stress DONE";
                xcat::log::Ok("App", "teleportKickStressSeq=%u", c.teleportKickStressSeq);
            } else if (strcmp(which, "fine0") == 0) {
                bump(c.teleportKickStressFineSeq);
                tag = "tp-kick-fine";
                okTitle = "细扫压测已下发（再点停止）";
                okBody = "50→0ms/−5；mode=fine0-50";
                xcat::log::Ok("App", "teleportKickStressFineSeq=%u", c.teleportKickStressFineSeq);
            } else if (strcmp(which, "fine10") == 0) {
                bump(c.teleportKickStressFine10Seq);
                tag = "tp-kick-f10";
                okTitle = "细扫10已下发（再点停止）";
                okBody = "30→10ms/−5；mode=fine10-30";
                xcat::log::Ok("App", "teleportKickStressFine10Seq=%u",
                              c.teleportKickStressFine10Seq);
            } else if (strcmp(which, "local") == 0) {
                bump(c.teleportKickStressLocalSeq);
                tag = "tp-kick-loc";
                okTitle = "原地短跳已下发（再点停止）";
                okBody = "同台±120px 来回；mode=local0-50";
                xcat::log::Ok("App", "teleportKickStressLocalSeq=%u", c.teleportKickStressLocalSeq);
            } else {
                return;
            }
            c.writeTickMs = GetTickCount64();
            if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
                notify::PushLocal(/*Info*/ 0, tag, okTitle, okBody, 4500);
            } else {
                notify::PushLocal(/*Warning*/ 2, tag, "写盘失败", "user.ini", 3000);
            }
        };

        xcat::ui::CardGuard card("##tab_dbg_tp", "瞬移 / 踢号压测");
        ImGui::TextDisabled("结果看 bin/XCat_data/logs/combat.log · kick_stress / Teleport");
        {
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float rowW = ImGui::GetContentRegionAvail().x;
            const float btnW2 = (std::max)(1.f, (rowW - gap) * 0.5f);
            const float btnW4 = (std::max)(1.f, (rowW - gap * 3.f) * 0.25f);

            if (ImGui::Button("测试贴怪瞬移", ImVec2(btnW2, 0.f))) bumpTpSeq("test");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "随机瞬移到一只怪身边（fill+Doing）。\n"
                    "用首页「落点偏移」；不依赖贴怪开关。与 F11 同路径。");
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("原生CALL", ImVec2(btnW2, 0.f))) bumpTpSeq("native");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("短距探针：fill+Doing（~140px 自选落点）");
            }

            if (ImGui::Button("踢号压测", ImVec2(btnW4, 0.f))) bumpTpSeq("wide");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "随机贴怪 2000→50ms，每级−50 / 8跳；再点停止。\n"
                    "DONE last_ok_cd=…；压测中挂起 F5。");
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("细扫0-50", ImVec2(btnW4, 0.f))) bumpTpSeq("fine0");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("50→0ms/−5，每级12跳；mode=fine0-50");
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("细扫10-30", ImVec2(btnW4, 0.f))) bumpTpSeq("fine10");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("30→10ms/−5，每级12跳；mode=fine10-30");
            }
            ImGui::SameLine(0.f, gap);
            if (ImGui::Button("原地短跳", ImVec2(btnW4, 0.f))) bumpTpSeq("local");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "同台左右来回（排除远距）；50→0ms；mode=local0-50\n"
                    "再点同按钮停止。");
            }
        }
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
    default:
        ImGui::TextDisabled("未知 TAB");
        break;
    }
}

}  // namespace xcat::app
