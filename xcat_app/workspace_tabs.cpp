#include "workspace_tabs.h"

#include "app_dpi.h"
#include "app_theme.h"
#include "app_window.h"
#include "imgui_shell.h"
#include "launch_panel.h"
#include "log_upload_ui.h"

#include "msc_webview_login.h"
#include "process_util.h"
#include "xcat_imgui_basic.h"
#include "xcat_log.h"
#include "xcat_payload_control.h"
#include "xcat_version.h"

#include "imgui.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

namespace xcat::app {
namespace {

void DesignBanner() {
    ImGui::TextDisabled("对齐枫星布局；飞行/无敌已接 user.ini [core] → payload");
    ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.35f));
}

void CardGap() { ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.55f)); }

void DrawLaunchTab(LaunchUiState& ui) {
    {
        xcat::ui::CardGuard card("##tab_launch_account", "账号 / 一键启动");
        ImGui::TextUnformatted("粘贴账号串（邮箱----密码----…，只取前两项；自动按 ---- 换行）");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##account", ui.accountLine, sizeof(ui.accountLine),
                                      ImVec2(-1, ui::BtnH() * 3.6f),
                                      ImGuiInputTextFlags_AllowTabInput)) {
            // 粘贴后若仍是单行超长串，松手时再格式化一次
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            LaunchPanel_FormatAccountForUi(ui);
        }

        if (ImGui::Button("保存账号", ImVec2(AppDpi_Px(120.f), ui::BtnH()))) {
            LaunchPanel_SaveAccount(ui);
            ui.status = "已保存到 %LocalAppData%\\xcat_msc\\account.txt";
        }
        ImGui::SameLine();
        const bool busy = msc::weblogin::IsBusy();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("一键启动游戏", ImVec2(AppDpi_Px(160.f), ui::BtnH()))) {
            LaunchPanel_StartOneClick(ui);
        }
        if (busy) ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled(msc::weblogin::IsReady() ? "WebView 就绪" : "WebView 初始化中…");

        if (!ui.status.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", ui.status.c_str());
        }
    }

    CardGap();
    {
        xcat::ui::CardGuard card("##tab_launch_log", "登录日志", /*fillRemaining=*/true);
        ImGui::TextUnformatted(msc::weblogin::IsBusy() ? "进行中…" : "最近输出");
        ImGui::BeginChild("##log_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(ui.logTail.empty() ? "(暂无日志)" : ui.logTail.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.f) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
}

void DrawHomeTab(LaunchUiState& ui) {
    DesignBanner();
    static bool autoEnter = false;
    static int charSlot = 1;
    static int worldId = 0;
    static char worldName[64]{};
    static bool fly = false;
    static bool autoLie = false;
    static bool invincible = false;
    static bool autoCombat = false;
    static bool autoPickup = true;
    static bool petLoot = false;
    static bool pickupBlacklist = false;
    static char blacklistKw[128]{};
    static bool smartInterval = true;
    static int attackMs = 350;
    static bool autoSell = false;
    static bool memWatch = true;
    static bool coreLoaded = false;
    static uint64_t lastSeenTick = 0;

    if (!ui.prefsBinDir.empty()) {
        xcat::PayloadControl disk{};
        if (xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), disk)) {
            if (!coreLoaded || disk.writeTickMs != lastSeenTick) {
                fly = disk.fly != 0;
                invincible = disk.invuln != 0;
                autoEnter = disk.autoEnter != 0;
                charSlot = (int)(disk.charSlot ? disk.charSlot : 1u);
                worldId = disk.worldId;
                strncpy_s(worldName, disk.worldName, _TRUNCATE);
                lastSeenTick = disk.writeTickMs;
                coreLoaded = true;
            }
        } else if (!coreLoaded) {
            coreLoaded = true;
        }
    }

    auto persistCore = [&]() {
        if (ui.prefsBinDir.empty()) return;
        xcat::PayloadControl c{};
        (void)xcat::ReadPayloadControl(ui.prefsBinDir.c_str(), c);
        c.fly = fly ? 1u : 0u;
        c.invuln = invincible ? 1u : 0u;
        c.autoEnter = autoEnter ? 1u : 0u;
        c.charSlot = charSlot < 1 ? 1u : (uint32_t)charSlot;
        c.worldId = worldId;
        strncpy_s(c.worldName, worldName, _TRUNCATE);
        c.writeTickMs = GetTickCount64();
        if (xcat::WritePayloadControl(ui.prefsBinDir.c_str(), c)) {
            lastSeenTick = c.writeTickMs;
            xcat::log::Ok("App", "已下发 core：飞行=%d 无敌=%d 自动进=%d 分区=%d 槽=%d", fly ? 1 : 0,
                          invincible ? 1 : 0, autoEnter ? 1 : 0, worldId, charSlot);
        } else {
            xcat::log::Warn("App", "写入 user.ini [core] 失败");
        }
    };

    {
        xcat::ui::CardGuard card("##tab_home_hangup", "挂机");
        if (xcat::ui::OptionCheckbox("自动进游戏", &autoEnter)) persistCore();
        ImGui::SameLine();
        ImGui::TextDisabled("分区→最少人频道→选角");
        ImGui::SetNextItemWidth(AppDpi_Px(72.f));
        if (ImGui::DragInt("分区ID", &worldId, 1, 0, 99)) persistCore();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(AppDpi_Px(120.f));
        if (ImGui::InputTextWithHint("##worldName", "或填分区名", worldName, sizeof(worldName)))
            persistCore();
        ImGui::SetNextItemWidth(AppDpi_Px(56.f));
        if (ImGui::DragInt("角色槽", &charSlot, 1, 1, 15)) persistCore();
        if (xcat::ui::OptionCheckbox("飞行", &fly)) persistCore();
        xcat::ui::OptionCheckbox("自动测谎", &autoLie);
        ImGui::SameLine();
        ImGui::TextDisabled("(角色测谎 --/--)(本次 0/0)");
        if (xcat::ui::OptionCheckbox("无敌", &invincible)) persistCore();
        ImGui::SameLine();
        ImGui::TextDisabled("(F10 热键同步；写 user.ini [core])");
        if (ImGui::Button("随机换频", ImVec2(AppDpi_Px(100.f), 0.f))) {
        }
        ImGui::Separator();
        xcat::ui::OptionCheckbox("自动打怪", &autoCombat);
        ImGui::TextDisabled("注入后勾选即时下发；未注入时仅落盘，进图后 payload 轮询生效");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_pickup", "拾物");
        xcat::ui::OptionCheckbox("自动拾取", &autoPickup);
        xcat::ui::OptionCheckbox("宠物吸物", &petLoot);
        xcat::ui::OptionCheckbox("启用拾取黑名单", &pickupBlacklist);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##bl", "关键词（逗号分隔）", blacklistKw, sizeof(blacklistKw));
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_combat", "打怪设置");
        xcat::ui::OptionCheckbox("智能间隔", &smartInterval);
        ImGui::SetNextItemWidth(AppDpi_Px(100.f));
        ImGui::DragInt("攻击间隔 ms", &attackMs, 1, 50, 2000);
        xcat::ui::OptionCheckbox("群怪优先", &autoCombat);
        xcat::ui::OptionCheckbox("拾取优先", &autoPickup);
        ImGui::TextDisabled("粘怪 / 低血撤 / 限定区间：Classic 锚点确认后再接");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_sell", "卖背包 / 自动补给");
        ImGui::TextWrapped("不卖名单与自动回城卖装将接 Classic 背包契约；当前为布局占位。");
        xcat::ui::OptionCheckbox("自动卖", &autoSell);
        if (ImGui::Button("一键卖装", ImVec2(AppDpi_Px(100.f), 0.f))) {
        }
        ImGui::SameLine();
        if (ImGui::Button("回挂机图", ImVec2(AppDpi_Px(100.f), 0.f))) {
        }
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_home_mem", "低内存守护");
        xcat::ui::OptionCheckbox("低内存自动回收", &memWatch);
        xcat::ui::OptionCheckbox("换图后回收", &memWatch);
        if (ImGui::Button("手动安全回收一次", ImVec2(AppDpi_Px(160.f), 0.f))) {
        }
        ImGui::TextDisabled("payload 内存指标：未注入");
    }
}

void DrawHangupScheduleTab() {
    DesignBanner();
    static bool enabled = false;
    static bool hours[24]{};
    static bool hoursInit = false;
    if (!hoursInit) {
        for (int i = 8; i <= 23; ++i) hours[i] = true;  // 默认晚间示意
        hoursInit = true;
    }

    xcat::ui::CardGuard card("##tab_hangup", "挂机时段");
    xcat::ui::OptionCheckbox("启用挂机时段", &enabled);
    ImGui::SameLine();
    ImGui::TextDisabled(enabled ? "开启后按时段拉起/关机（待接 watchdog）" : "已关闭（忽略表）");

    if (ImGui::SmallButton("全选")) {
        for (bool& h : hours) h = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("清空")) {
        for (bool& h : hours) h = false;
    }

    if (!enabled) ImGui::BeginDisabled();
    if (ImGui::BeginTable("##hangup_hours", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0, AppDpi_Px(280.f)))) {
        ImGui::TableSetupColumn("时段", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.2f);
        ImGui::TableHeadersRow();
        SYSTEMTIME st{};
        GetLocalTime(&st);
        for (int hour = 0; hour < 24; ++hour) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32]{};
            snprintf(label, sizeof(label), "%d:00 - %d:59", hour, hour);
            if (hour == static_cast<int>(st.wHour))
                ImGui::Text("%s（当前）", label);
            else
                ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::PushID(hour);
            ImGui::Checkbox("##h", &hours[hour]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!enabled) ImGui::EndDisabled();
}

void DrawMultiSkillTab() {
    DesignBanner();
    static bool master = false;
    static bool safeSerial = true;
    static bool hideAssist = false;
    static char search[64]{};
    static const char* kDemoSkills[] = {"普通攻击", "群体攻击", "位移", "BUFF·攻击力", "BUFF·防御"};
    static bool selected[5]{true, true, false, true, false};

    xcat::ui::CardGuard card("##tab_multiskill", "技能多发", /*fillRemaining=*/true);
    xcat::ui::OptionCheckbox("启用技能多发", &master);
    ImGui::SameLine();
    ImGui::TextDisabled(master ? "预览开" : "预览关");
    xcat::ui::OptionCheckbox("安全串发", &safeSerial);
    xcat::ui::OptionCheckbox("隐藏辅助技能", &hideAssist);

    if (ImGui::Button("按职业自动勾选")) {
    }
    ImGui::SameLine();
    if (ImGui::Button("清空全部")) {
        for (bool& s : selected) s = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ms_search", "搜索技能", search, sizeof(search));

    if (ImGui::BeginTable("##ms_table", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("开", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
        ImGui::TableSetupColumn("技能", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 5; ++i) {
            if (search[0] && !strstr(kDemoSkills[i], search)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::Checkbox("##sel", &selected[i]);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kDemoSkills[i]);
        }
        ImGui::EndTable();
    }
}

void DrawReloginTab() {
    DesignBanner();
    static bool detect = true;
    static bool stopCombat = true;
    static bool stopFly = true;
    static bool channelHop = false;

    xcat::ui::CardGuard card("##tab_relogin", "有人来时怎么办");
    xcat::ui::OptionCheckbox("检测同图玩家", &detect);
    ImGui::Separator();
    ImGui::TextUnformatted("处理流程");
    xcat::ui::OptionCheckbox("先停手", &stopCombat);
    xcat::ui::OptionCheckbox("停止飞行", &stopFly);
    xcat::ui::OptionCheckbox("一直有人就换频", &channelHop);
    ImGui::TextDisabled("Classic 同图探测锚点确认后接入");
}

void DrawTimedKeysTab() {
    DesignBanner();
    static const char* kKeys[] = {"7", "8", "9", "0", "-", "=", "Z"};
    static bool en[7]{};
    static int sec[7]{30, 30, 60, 60, 120, 120, 10};

    xcat::ui::CardGuard card("##tab_timed", "定时按键");
    ImGui::TextWrapped("按间隔向游戏投递按键（需注入后生效）。下方为设计表。");
    if (ImGui::BeginTable("##timed_keys", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(40.f));
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(48.f));
        ImGui::TableSetupColumn("间隔秒", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 7; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kKeys[i]);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::Checkbox("##en", &en[i]);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::DragInt("##sec", &sec[i], 1, 1, 3600);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawBuffsTab() {
    DesignBanner();
    static bool master = false;
    static char search[64]{};
    static const char* kBuffs[] = {"攻击力提升", "防御提升", "移速", "跳跃", "伤害反弹"};
    static bool on[5]{true, true, false, false, false};
    static int remain[5]{42, 18, 0, 0, 0};
    static int cd[5]{120, 180, 60, 60, 300};

    xcat::ui::CardGuard card("##tab_buffs", "BUFF 管理器", /*fillRemaining=*/true);
    xcat::ui::OptionCheckbox("启用 BUFF 续航", &master);
    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
    }
    ImGui::SameLine();
    ImGui::TextDisabled(master ? "预览运行中" : "未启用");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##buff_search", "搜索 BUFF", search, sizeof(search));

    if (ImGui::BeginTable("##buff_table", 5,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("开", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.2f);
        ImGui::TableSetupColumn("技能", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("剩余", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(48.f));
        ImGui::TableSetupColumn("CD", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(40.f));
        ImGui::TableSetupColumn("策略", ImGuiTableColumnFlags_WidthFixed, AppDpi_Px(56.f));
        ImGui::TableHeadersRow();
        for (int i = 0; i < 5; ++i) {
            if (search[0] && !strstr(kBuffs[i], search)) continue;
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            ImGui::Checkbox("##on", &on[i]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kBuffs[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%d", remain[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%d", cd[i]);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("到期续");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawTravelTab() {
    DesignBanner();
    static char go[128]{};
    static char search[128]{};
    static int region = 0;
    static int selected = -1;
    static const char* kRegions[] = {"全部大区", "维多利亚", "阿斯旺", "神木村"};
    static const char* kMaps[] = {"弓箭手村", "魔法密林", "勇士部落", "废弃都市", "天空之城"};
    static const char* kKeys[] = {"100000000", "101000000", "102000000", "103000000", "200000000"};

    {
        xcat::ui::CardGuard card("##tab_travel_dest", "目的地");
        if (selected >= 0)
            ImGui::Text("已选  %s · %s", kMaps[selected], kKeys[selected]);
        else
            ImGui::TextDisabled("从下方目录点选，或输入地图名 / 图号");

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##go", "地图名 / 图号", go, sizeof(go));
        if (ImGui::Button("开始赶路", ImVec2(AppDpi_Px(100.f), 0.f))) {
        }
        ImGui::SameLine();
        if (ImGui::Button("停止赶路", ImVec2(AppDpi_Px(100.f), 0.f))) {
        }
        ImGui::SameLine();
        if (ImGui::Button("填入已选", ImVec2(AppDpi_Px(100.f), 0.f)) && selected >= 0) {
            strncpy_s(go, kKeys[selected], _TRUNCATE);
        }
        ImGui::TextDisabled("对接 x/features/fly 与地图目录后下发命令");
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_travel_cat", "地图目录", /*fillRemaining=*/true);
        ImGui::SetNextItemWidth(AppDpi_Px(140.f));
        ImGui::Combo("##region", &region, kRegions, IM_ARRAYSIZE(kRegions));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##map_search", "搜索", search, sizeof(search));
        if (ImGui::Button("重新加载")) {
        }
        ImGui::SameLine();
        if (ImGui::Button("保存学习图")) {
        }

        ImGui::BeginChild("##map_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (int i = 0; i < 5; ++i) {
            if (search[0] && !strstr(kMaps[i], search) && !strstr(kKeys[i], search)) continue;
            char line[96]{};
            snprintf(line, sizeof(line), "%s  (%s)", kMaps[i], kKeys[i]);
            if (ImGui::Selectable(line, selected == i)) selected = i;
        }
        ImGui::EndChild();
    }
}

void DrawBetaTab() {
    DesignBanner();
    static bool dropInCombat = false;
    static bool skipDialog = false;
    static bool autoAccept = true;
    static bool autoFirst = false;
    static bool classicFlyBoost = false;

    xcat::ui::CardGuard card("##tab_beta", "实验功能");
    xcat::ui::OptionCheckbox("战斗中可丢物", &dropInCombat);
    xcat::ui::OptionCheckbox("快速跳过对话", &skipDialog);
    if (skipDialog) {
        ImGui::Indent(ui::Gap() * 1.2f);
        xcat::ui::OptionCheckbox("自动接取/Yes", &autoAccept);
        xcat::ui::OptionCheckbox("自动选列表第一项", &autoFirst);
        ImGui::Unindent(ui::Gap() * 1.2f);
    }
    ImGui::Separator();
    ImGui::TextUnformatted("经典版专项（预留）");
    xcat::ui::OptionCheckbox("飞天研究开关", &classicFlyBoost);
    ImGui::TextDisabled("阴阳师灵力等 MSW 专属项不迁入 Classic");
}

void DrawDebugTab(LaunchUiState& ui) {
    DesignBanner();
    {
        xcat::ui::CardGuard card("##tab_dbg_status", "运行状态");
        ImGui::Text("产品：%s", xcat::kXcatProductName);
        ImGui::Text("版本：%s (build %u)", xcat::kXcatVersionString, xcat::kXcatBuildId);
        ImGui::Text("WebView：%s", msc::weblogin::IsReady() ? "就绪" : "未就绪");
        ImGui::Text("换票会话：%s", msc::weblogin::IsBusy() ? "忙碌" : "空闲");
        ImGui::TextDisabled("顶栏 5 灯：IPC / GameContext / LocalPlayer / Map / Cache");
        ImGui::TextDisabled("游戏 PID / 注入状态：见顶栏灯与状态条");
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
        xcat::ui::CardGuard card("##tab_dbg_maint", "日志 / 路径");
        ImGui::TextWrapped("启动器 JSONL：bin/logs/launcher.jsonl");
        ImGui::TextWrapped("注入 JSONL：bin/logs/inject.jsonl");
        ImGui::TextWrapped("载荷 JSONL：bin/XCat_data/logs/x.jsonl");
        ImGui::TextWrapped("换票文本：bin/launcher.log（WebView 兼容）");
        ImGui::TextWrapped("账号：%%LocalAppData%%\\xcat_msc\\account.txt");
        if (ImGui::Button("清空面板日志", ImVec2(AppDpi_Px(140.f), 0.f))) ui.logTail.clear();
        ImGui::Dummy(ImVec2(0.f, ui::Gap() * 0.6f));
        ImGui::Separator();
        ImGui::TextUnformatted("上报日志到 ops 服务");
        DrawLogUploadPanel(ui.prefsBinDir);
    }
    CardGap();
    {
        xcat::ui::CardGuard card("##tab_dbg_about", "关于");
        ImGui::TextWrapped(
            "目标服：新楓之谷：經典版（TW beanfun / Gamania Galaxy）。\n"
            "换票：同进程 WebView2（静默后台，不占 ImGui 区域）。\n"
            "UI：对齐 xcat_for_fengxing 工作区 Tab + CardGuard。\n"
            "功能页为设计稿，后续按模块接 inject / fly / hangup。");
    }
}

}  // namespace

void DrawWorkspaceTabContent(AppWindow& /*app*/, LaunchUiState& ui, int tabIndex) {
    switch (static_cast<WorkspaceTab>(tabIndex)) {
    case WorkspaceTab::Launch:
        DrawLaunchTab(ui);
        break;
    case WorkspaceTab::Home:
        DrawHomeTab(ui);
        break;
    case WorkspaceTab::HangupSchedule:
        DrawHangupScheduleTab();
        break;
    case WorkspaceTab::MultiSkill:
        DrawMultiSkillTab();
        break;
    case WorkspaceTab::Relogin:
        DrawReloginTab();
        break;
    case WorkspaceTab::TimedKeys:
        DrawTimedKeysTab();
        break;
    case WorkspaceTab::Buffs:
        DrawBuffsTab();
        break;
    case WorkspaceTab::Travel:
        DrawTravelTab();
        break;
    case WorkspaceTab::Beta:
        DrawBetaTab();
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
