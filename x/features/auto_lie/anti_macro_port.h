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

bool IsOpenAntiMacro();
bool IsTextCaptchaOpen();
bool IsNonFiniteOpen();

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
bool ReadRawPosList(void* instance, std::vector<Vec2>& out);
void* ReadNonFiniteTargetRect(void* instance);  // RawImage → RectTransform
bool TryMapWinCursor(void* rectTransform, float localX, float localY, POINT& outScreen);
// 一次主线程任务把整条局部轨迹映射成屏幕点（避免逐点 InvokeAndWait 拖死跟随）。
bool MapWinCursorBatch(void* rectTransform, const std::vector<Vec2>& localPts,
                       std::vector<POINT>& outScreen);
bool IsGameForeground();

}  // namespace x::features::auto_lie::anti_macro_port
