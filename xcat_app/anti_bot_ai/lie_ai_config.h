#pragma once

#include "llm_provider.h"

#include <string>

namespace xcat::app::anti_bot_ai {

// 测谎 LLM：user.ini [lie_ai]；旧 lie_ai.ini 只读 migrate。默认 enabled（内置 Qwen）。
struct LieAiConfig {
    bool        enabled  = true;
    LlmEndpoint endpoint{};
};

void LieAiConfigSetDefaults(LieAiConfig& cfg);
void LoadLieAiConfig(const std::string& binDir, LieAiConfig& cfg);
void SaveLieAiConfig(const std::string& binDir, const LieAiConfig& cfg);

// 加载后解析内置 Qwen 视觉密钥（apiKey 为空时）。
LieAiConfig LoadLieAiConfigResolved(const std::string& binDir);

}  // namespace xcat::app::anti_bot_ai
