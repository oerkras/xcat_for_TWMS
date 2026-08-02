#pragma once

#include "llm_provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xcat::app::anti_bot_ai {

std::string LlmBase64Encode(const uint8_t* data, size_t len);

bool LlmConnectTest(const LlmEndpoint& ep, std::string& outMsg);

// 视觉：单箭头方向识别（符文反 BOT）。
bool LlmClassifyArrow(const LlmEndpoint& ep, const std::vector<uint8_t>& imgBytes,
                      const char* imgMime, std::string& outDir, std::string& outErr);

// 文本：OpenAI 兼容 chat（thinking 仅部分端点支持时生效）。
bool LlmChatText(const LlmEndpoint& ep, const std::string& systemPrompt,
                 const std::string& userPrompt, bool thinking, std::string& outText,
                 std::string& outErr);

// 视觉多选：图 + 题干 + N 个选项 → 命中选项（测谎 MapleQuiz / 经典像素测谎）。
// 返回命中选项的 1-based 序号（outIndex）与原文（outAnswer）。imgBytes 可为空（纯文字题）。
bool LlmAnswerQuiz(const LlmEndpoint& ep, const std::vector<uint8_t>& imgBytes, const char* imgMime,
                   const std::string& question, const std::vector<std::string>& options,
                   int& outIndex, std::string& outAnswer, std::string& outErr);

// Classic TextCaptcha：视觉 OCR，只输出答案字符串。
bool LlmOcrCaptcha(const LlmEndpoint& ep, const std::vector<uint8_t>& imgBytes, const char* imgMime,
                   const std::string& hint, std::string& outAnswer, std::string& outErr);

}  // namespace xcat::app::anti_bot_ai
