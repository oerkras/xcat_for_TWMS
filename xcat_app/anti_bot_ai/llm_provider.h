#pragma once

#include <string>

namespace xcat::app::anti_bot_ai {

// 反 BOT LLM 提供方：符文→视觉(Qwen)，测谎→文本(DeepSeek)。
enum class LlmProviderKind { QwenVision, DeepSeekText };

struct LlmEndpoint {
    std::string apiUrl;
    std::string apiKey;
    std::string model;
    int         timeoutMs = 20000;
};

const char* LlmEmbeddedKey(LlmProviderKind kind);
LlmEndpoint LlmDefaultEndpoint(LlmProviderKind kind);

// ini/面板 apiKey 为空时回落内置密钥；非空则尊重用户覆盖。
void LlmResolveEndpoint(LlmEndpoint& ep, LlmProviderKind kind);

std::string LlmEffectiveApiKey(const LlmEndpoint& ep, LlmProviderKind kind);

}  // namespace xcat::app::anti_bot_ai
