#pragma once

#include <string>

namespace xcat::app {

struct LieAiPumpStats {
    int         pendingCount   = 0;
    int         sessionAnswered = 0;
    std::string lastError;
};

// PC 侧测谎 LLM 泵：消费 XCat_data/state/lie_ai/req/lie_*.{txt,jpg,bmp,png}
// → LlmAnswerQuiz / LlmOcrCaptcha → 写 state/lie_ai/ans/lie_*.ans；payload auto_lie 读 ans 作答。
void LieAiPump_Tick(const std::string& binDir);
void LieAiPump_Shutdown();
LieAiPumpStats LieAiPump_GetStats();

// 离线基建夹具：往 req/ 落一对 txt(+png)。echoOnly=true 走本地回声不调 LLM。
// 返回题 id（空=失败）。
std::string LieAiPump_EnqueueFixture(const std::string& binDir, bool echoOnly);

}  // namespace xcat::app
