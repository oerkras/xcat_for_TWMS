#pragma once

// XCAT 启动激活门（gate/1，产品=经典版 / TWMS）。
//
// 每人一张离线 ECDSA P-256 签名 TOKEN；exe 内嵌公钥（xcat_start_gate_secret.h）离线验签，
// 断网也能拦。首次激活成功后把 TOKEN + 本机 deviceId 加密缓存，之后免输。
// 缓存 JSON 的 v 是世代。当前写入 2；1 与 2 都认（曾用抬世代强制全员重贴，已改为
// 「服务端验不出 uid 才弹框」）。再抬到 3 时把下面读路径改成只认新值即可。
// 单人吊销 / 台数配额走已有在线 gate/2（本模块只负责本地准入 + 供上报的 TOKEN）。

#include <string>

namespace xcat {
namespace gate {

// gate/1 拒启退出码（与 update_client 的 gate/2=2、gate/3=3 区分）。
constexpr int kStartGateExitCode = 4;

struct TokenClaims {
    std::string uid;      // 成员标识（签发时写入；兼作运维台按人识别键）
    long long   iss = 0;  // 签发时间（unix 秒）
    long long   exp = 0;  // 到期时间（unix 秒；0=永不过期）
};

// 纯离线验签：解析 TOKEN、用内嵌公钥验 ECDSA P-256 签名、检查是否过期。
// 通过返回 true 并填 out；任何环节失败返回 false。不联网、不触碰缓存。
bool VerifyToken(const std::string& token, TokenClaims& out);

// 读激活缓存里存的完整签名 TOKEN。
// 会校验缓存绑定的 deviceId == currentDeviceId 且 TOKEN 仍验签通过；否则返回空串。
// 注意：整张 TOKEN 是持有即可用的凭证，别再放进网络请求头 —— 上报改用 BuildGateProof。
std::string LoadActivatedGateToken(const std::string& payloadBinDir,
                                   const std::string& currentDeviceId);

// 构造探活用的派生凭证（头 X-XCat-Gate-Proof），格式 payloadB64.ts.macB64url：
//   mac = HMAC-SHA256(key=卡签名段原字节, msg=payloadB64|ts|deviceId)
// 卡的签名段永不上网；凭证绑定时间戳与本机 deviceId，抓包重放出窗即废。
// 无有效激活缓存时返回空串。unixSec 传当前 unix 秒。
std::string BuildGateProof(const std::string& payloadBinDir, const std::string& deviceId,
                           long long unixSec);

// 是否已有有效激活缓存（绑定本机 deviceId、TOKEN 未过期且验签通过）。不弹 UI、不联网。
bool HasValidActivation(const std::string& payloadBinDir, const std::string& deviceId);

// 丢掉本机激活缓存（ProgramData + 安装目录）。服务端认不出当前卡时调用，下一步弹框重贴。
void InvalidateActivationCache(const std::string& payloadBinDir);

// 提交一张用户输入的 TOKEN：验签通过则写激活缓存并放行，填 out（含 uid/exp）返回 true；
// TOKEN 无效或过期返回 false（不写缓存）。UI 层（gate_activation_ui）调用它落地激活。
bool CommitActivation(const std::string& payloadBinDir, const std::string& deviceId,
                      const std::string& token, TokenClaims& out);

}  // namespace gate
}  // namespace xcat
