#pragma once
// Classic TWMS AntiMacro 只读 / 提交口（无 INLINE HOOK）。
// 锚点见 docs/features/auto_lie/P0a_锚点复核.md。

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace x::features::auto_lie::anti_macro_port {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};
// IL2CPP valuetype：x64 按值传 RDX（8B 打包），禁止拆成 (float,float) 以免 out* 掉进 R9。
static_assert(sizeof(Vec2) == 8, "Vec2 must be 8B (IL2CPP Vector2-by-value ABI)");

// mousePosList 元素为 Vector2Int（两 int）；本仓只读 List._size，不读元素。
struct Vec2Int {
    int x = 0;
    int y = 0;
};
static_assert(sizeof(Vec2Int) == 8, "Vec2Int must be 8B");

enum class Kind : uint8_t { None = 0, TextCaptcha, NonFinite };

// 题图落盘格式（本仓 Unity 无 EncodeToJPG，仅有 EncodeToPNG）。
enum class CaptchaImageKind : uint8_t { Unknown = 0, Jpeg, Png };

bool Ensure();

// 离线基建：逐项探测 FindClass / EncodeToPNG / InputField，不依赖测谎 UI。
struct BindReady {
    bool il2cpp = false;
    bool gaBase = false;
    bool klassUtil = false;
    bool klassText = false;
    bool klassNonFinite = false;
    bool inputSetText = false;
    bool getTransform = false;
    bool encodePng = false;
    // 知识题最小集：jpegData 通路可不依赖 EncodeToPNG
    bool quizOk = false;
    // 轨迹映射最小集
    bool mouseOk = false;
    // quizOk && mouseOk（不含 encodePng）
    bool ok = false;
};

BindReady ProbeBindReady();

// 下面三个谓词与两个 GetXxx 都由一条后台刷新线程在**主泵**上求值，调用方只读快照、绝不阻塞。
// 快照超过 600ms 未刷新即视为「取不到」，一律返回 false / nullptr。
bool IsOpenAntiMacro();
bool IsTextCaptchaOpen();
bool IsNonFiniteOpen();

// 停掉那条刷新线程（由 auto_lie::Shutdown 调用）。
void StopRefresher();

void* GetTextCaptcha();
void* GetNonFinite();

// TextCaptcha：优先 Info.jpegData（真 JPEG）；否则 RawImage.texture → EncodeToPNG。
// 成功时 out 非空，kind 为 Jpeg 或 Png。
bool DumpTextCaptchaImage(std::vector<uint8_t>& out, CaptchaImageKind& kind);

// 写 inputField 文本并调 OnOk（主线程）。
bool SubmitTextCaptchaAnswer(const std::string& answer);

// NonFinite
int ReadNonFiniteTickFrame(void* instance);
int ReadMouseSampleCount(void* instance);
// _isResultRecv（TW +0xF8）：服端结果已回；对照 Artale recvResult，优先于 path 长度放闸。
bool ReadNonFiniteIsResultRecv(void* instance);
// mousePosList 对象指针（供 SendWill 帧脉冲无锁读 Count；勿跨题缓存）。
void* PeekNonFiniteMouseList(void* instance);
bool ReadRawPosList(void* instance, std::vector<Vec2>& out);
void* ReadNonFiniteTargetRect(void* instance);  // RawImage → RectTransform

// rawPosList 是 DecodePath 后的归一化题面坐标（量级 ~1），不是像素。
// 对照 CMS/枫星 DecryptPointToCursorPoint：`(x+0.75)*500,(y+0.5)*500` → 750×500 题面局部。
// E175 BIN：未解密直接 TryGetWinCursorPos → 屏幕跨度≈1px → 跟死点 → 踢号。
Vec2 RawToCursorLocal(Vec2 raw);
bool RawPathLooksLikeCanvasPixels(const std::vector<Vec2>& raw);

bool TryMapWinCursor(void* rectTransform, float localX, float localY, POINT& outScreen);
// 一次主线程任务把整条局部轨迹映射成屏幕点（避免逐点 InvokeAndWait 拖死跟随）。
// 内部会按需 RawToCursorLocal；调用方仍传 rawPosList 原值即可。
// 主路径：面板四角 TransformPoint→WorldToScreen 仿射（对照 Artale）；
// TryGetWinCursorPos 仅交叉验证 + 仿射失败时回退。
// outPanelCorners4 非空时写入桌面四角（LT/RT/RB/LB 序与 Artale corners[0..3] 一致：
// rect (x0,y0)/(x1,y0)/(x1,y1)/(x0,y1)）。
bool MapWinCursorBatch(void* rectTransform, const std::vector<Vec2>& localPts,
                       std::vector<POINT>& outScreen, POINT* outPanelCorners4 = nullptr,
                       bool* outHavePanelCorners = nullptr);
// 屏幕轨是否塌缩（本地跨度尚可但桌面 AABB 过小 / 飞出客户区）。
bool IsScreenPathCollapsed(const std::vector<Vec2>& localPts, const std::vector<POINT>& screen,
                           long* outSpanX = nullptr, long* outSpanY = nullptr);
bool IsGameForeground();

// 无人值守抢前台（对照仓 Artale 同款）：worker 上 AttachThreadInput + SFW。
// 禁止在 MainPump 线程调用。force=true 绕过 400ms 节流（lie-open / follow-start）。
bool TryBringGameForeground(const char* why, bool force = false);

}  // namespace x::features::auto_lie::anti_macro_port
