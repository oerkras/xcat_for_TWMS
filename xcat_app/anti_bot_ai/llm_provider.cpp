#include "llm_provider.h"

#include "llm_secrets.h"

namespace xcat::app::anti_bot_ai {

const char* LlmEmbeddedKey(LlmProviderKind kind) {
    switch (kind) {
    case LlmProviderKind::QwenVision:
        return kEmbeddedQwenApiKey;
    case LlmProviderKind::DeepSeekText:
        return kEmbeddedDeepSeekApiKey;
    }
    return "";
}

LlmEndpoint LlmDefaultEndpoint(LlmProviderKind kind) {
    LlmEndpoint ep{};
    switch (kind) {
    case LlmProviderKind::QwenVision:
        ep.apiUrl    = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
        ep.model     = "qwen3.6-flash";
        ep.timeoutMs = 20000;
        break;
    case LlmProviderKind::DeepSeekText:
        ep.apiUrl    = "https://api.deepseek.com/v1/chat/completions";
        ep.model     = "deepseek-v4-flash";
        ep.timeoutMs = 20000;
        break;
    }
    return ep;
}

void LlmResolveEndpoint(LlmEndpoint& ep, LlmProviderKind kind) {
    if (ep.apiKey.empty()) ep.apiKey = LlmEmbeddedKey(kind);
}

std::string LlmEffectiveApiKey(const LlmEndpoint& ep, LlmProviderKind kind) {
    if (!ep.apiKey.empty()) return ep.apiKey;
    return LlmEmbeddedKey(kind);
}

}  // namespace xcat::app::anti_bot_ai
