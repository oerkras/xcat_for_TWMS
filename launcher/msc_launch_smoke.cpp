// MSC launcher smoke：编译验证 + 本地解析 + 可选 OTT 换票（不默认真拉游戏）
//
// 用法：
//   msc_launch_smoke.exe                  # 本地单元冒烟
//   msc_launch_smoke.exe --ott "OTT:944:Login:..."
//   msc_launch_smoke.exe --ott-url "https://maplestoryclassic.beanfun.com/Main?OTT=..."
//   msc_launch_smoke.exe --ott "..." --launch   # 换票成功后真 NGM 拉起（慎用）

#include "msc_launch.h"
#include "ott_ticket_fetch.h"

#include <iostream>
#include <string>

namespace {

int Fail(const char* msg) {
    std::cerr << "[FAIL] " << msg << "\n";
    return 1;
}

int LocalSmoke() {
    using namespace msc::launcher;
    int failed = 0;

    GalaxyTicket bad{};
    bad.userObjectId = L"1";
    bad.userSessionToken = L"tok'en";
    bad.gid = L"2";
    bad.galaxyGameId = L"3";
    bad.ngmGameId = L"4";
    if (TicketLooksUsable(bad)) {
        std::cerr << "[FAIL] 含引号票应被拒\n";
        ++failed;
    } else {
        std::cout << "[OK] TicketLooksUsable 拒引号\n";
    }

    GalaxyTicket t{};
    t.userObjectId = L"4662771";
    t.userSessionToken = L"sess_smoke_token_0123456789abcdef";
    t.gid = L"2373";
    t.galaxyGameId = L"944";
    t.ngmGameId = L"smoke-game-id";
    if (!TicketLooksUsable(t)) {
        std::cerr << "[FAIL] 合法票应通过\n";
        ++failed;
    } else {
        std::cout << "[OK] TicketLooksUsable 接受合法票\n";
    }

    const std::wstring deep = BuildNgmDeepLink(t, NgmMode::Launch);
    const std::string sum = FormatDeepLinkForLog(deep);
    if (sum.find("sess_smoke") != std::string::npos) {
        std::cerr << "[FAIL] deep-link 摘要泄漏 token: " << sum << "\n";
        ++failed;
    } else if (sum.find("-passarg:'***'") == std::string::npos) {
        std::cerr << "[FAIL] 摘要未打码 passarg: " << sum << "\n";
        ++failed;
    } else {
        std::cout << "[OK] FormatDeepLinkForLog " << sum << "\n";
    }

    const std::wstring fakeCmd =
        L"\"G:\\Games\\maplestory_classic\\Maplestory_Classic.exe\" 4662771 "
        L"sess_smoke_token_0123456789abcdef 2373 944";
    if (!CmdMatchesGalaxyTicket(fakeCmd, t)) {
        std::cerr << "[FAIL] CmdMatchesGalaxyTicket 应匹配\n";
        ++failed;
    } else {
        std::cout << "[OK] CmdMatchesGalaxyTicket\n";
    }
    std::cout << "[OK] FormatCmdLineForLog " << FormatCmdLineForLog(fakeCmd) << "\n";

    GalaxyTicket wrong = t;
    wrong.gid = L"9999";
    if (CmdMatchesGalaxyTicket(fakeCmd, wrong)) {
        std::cerr << "[FAIL] 错 gid 不应匹配\n";
        ++failed;
    } else {
        std::cout << "[OK] 错 gid 拒匹配\n";
    }

    const auto ott1 = ExtractOttToken(L"OTT:944:Login:abc");
    const auto ott2 =
        ExtractOttToken(L"https://maplestoryclassic.beanfun.com/Main?OTT=OTT:944:Login:abc&x=1");
    if (ott1 != L"OTT:944:Login:abc" || ott2 != L"OTT:944:Login:abc") {
        std::cerr << "[FAIL] ExtractOttToken\n";
        ++failed;
    } else {
        std::cout << "[OK] ExtractOttToken\n";
    }

    const std::wstring ngm = FindNgmPath();
    std::cout << "[INFO] FindNgmPath=" << (ngm.empty() ? "(empty)" : "found") << "\n";

    Options dry;
    dry.ticket = t;
    dry.dryRunDeepLinkOnly = true;
    const Result rr = Run(dry, [](const Progress& p) {
        std::cout << "  progress stage=" << static_cast<int>(p.stage) << " " << p.message << "\n";
    });
    if (!rr.ok || rr.finalStage != Stage::Done) {
        std::cerr << "[FAIL] dryRun Run: " << rr.errorMessage << "\n";
        ++failed;
    } else {
        std::cout << "[OK] Run dry-run summary=" << rr.deepLinkSummary << "\n";
    }

    // 可选：读本机已在跑的经典版 cmdline（脱敏）
    // 不作为失败条件
    return failed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring ottArg;
    bool doLaunch = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if ((a == L"--ott" || a == L"--ott-url") && i + 1 < argc) {
            ottArg = argv[++i];
        } else if (a == L"--launch") {
            doLaunch = true;
        } else if (a == L"--help" || a == L"-h") {
            std::wcout << L"msc_launch_smoke [--ott OTT|url] [--launch]\n";
            return 0;
        }
    }

    const int localFails = LocalSmoke();
    if (localFails) {
        std::cerr << "[FAIL] local smoke failures=" << localFails << "\n";
        return 2;
    }
    std::cout << "[OK] local smoke passed\n";

    if (ottArg.empty()) {
        std::cout << "[INFO] 未提供 --ott，跳过换票 HTTP\n";
        return 0;
    }

    msc::launcher::TicketFetchOptions fo;
    fo.ott = ottArg;
    std::cout << "[INFO] FetchGalaxyTicketFromOtt...\n";
    auto fr = msc::launcher::FetchGalaxyTicketFromOtt(fo);
    if (!fr.ok) {
        std::cerr << "[FAIL] 换票失败 http=" << fr.httpStatus << " apiCode=" << fr.apiCode
                  << " msg=" << fr.message << "\n";
        // 无效 OTT 预期失败；仍算 smoke 工具可用
        return 3;
    }
    std::wcout << L"[OK] 换票成功 uid=" << fr.ticket.userObjectId
               << L" gid=" << fr.ticket.gid << L" galaxyId=" << fr.ticket.galaxyGameId
               << L" token_len=" << fr.ticket.userSessionToken.size()
               << L" ngmGameId_len=" << fr.ticket.ngmGameId.size() << L"\n";

    if (!doLaunch) {
        msc::launcher::Options dry;
        dry.ticket = fr.ticket;
        dry.dryRunDeepLinkOnly = true;
        auto rr = msc::launcher::Run(dry);
        std::cout << "[OK] ticket dry-run deepLink=" << rr.deepLinkSummary << "\n";
        return 0;
    }

    std::cout << "[WARN] --launch：将调用 NGM 真拉起\n";
    msc::launcher::Options opt;
    opt.ticket = fr.ticket;
    auto rr = msc::launcher::Run(opt, [](const msc::launcher::Progress& p) {
        std::cout << "  " << p.message << "\n";
    });
    if (!rr.ok) {
        std::cerr << "[FAIL] launch: " << rr.errorMessage << "\n";
        return 4;
    }
    std::cout << "[OK] launch pid=" << rr.gamePid << " cmd=" << rr.cmdLineSummary << "\n";
    return 0;
}
