/**
 * XCAT 启动激活门 · 个人 TOKEN 签发（产品=经典版 / TWMS）。
 *
 * 用离线私钥给某个成员签一张激活 TOKEN；把打印出来的字符串连同发给他即可，
 * 他在 xcat.exe 启动弹窗里粘贴一次即激活（之后本机免输）。
 *
 * TOKEN 结构：base64url(payloadJson) + "." + base64url(rawSig)
 *   payload = { uid, iss, exp, jti }   （exp=0 表示永不过期；jti=卡号，供服务端按卡吊销）
 *   rawSig  = ECDSA P-256 over SHA-256(payloadB64) —— IEEE P1363 r||s（64 字节）
 * 客户端(BCrypt)与服务端(node crypto)都按同一口径验签。
 *
 * 用法：
 *   node scripts/xcat-gate-sign.mjs --uid 张三
 *   node scripts/xcat-gate-sign.mjs --uid lisi --days 90
 */
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, "..");
const privPath = path.join(repoRoot, "secrets", "gate_ec_priv.pem");

function parseArgs(argv) {
  const out = { uid: "", days: 0 };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--uid") out.uid = String(argv[++i] || "").trim();
    else if (a === "--days") out.days = Math.max(0, Math.floor(Number(argv[++i] || 0)) || 0);
    else if (a.startsWith("--uid=")) out.uid = a.slice(6).trim();
    else if (a.startsWith("--days=")) out.days = Math.max(0, Math.floor(Number(a.slice(7)) || 0));
  }
  return out;
}

const { uid, days } = parseArgs(process.argv);

if (!uid) {
  console.error("用法: node scripts/xcat-gate-sign.mjs --uid <成员标识> [--days <有效天数>]");
  process.exit(1);
}
if (uid.length > 64) {
  console.error("[sign] uid 过长（<=64）");
  process.exit(1);
}
if (!fs.existsSync(privPath)) {
  console.error(`[sign] 未找到私钥 ${path.relative(repoRoot, privPath)}；先跑 node scripts/xcat-gate-keygen.mjs`);
  process.exit(1);
}

const privateKey = crypto.createPrivateKey(fs.readFileSync(privPath, "utf8"));

const now = Math.floor(Date.now() / 1000);
// jti = 卡号：服务端可按它单独废掉这一张卡（不牵连同 uid 的其他卡）。
// CLI 签的卡不落 OPS 台账，所以要把卡号打出来自己记一下，否则日后只能按 uid 整个封人。
const jti = crypto.randomBytes(6).toString("hex");
const payload = { uid, iss: now, exp: days > 0 ? now + days * 86400 : 0, jti };
const payloadB64 = Buffer.from(JSON.stringify(payload), "utf8").toString("base64url");
const sig = crypto.sign("sha256", Buffer.from(payloadB64, "utf8"), {
  key: privateKey,
  dsaEncoding: "ieee-p1363",
});
const token = `${payloadB64}.${sig.toString("base64url")}`;

const expText = payload.exp ? new Date(payload.exp * 1000).toISOString() : "永不过期";
console.log(
  `[sign] uid=${uid}  卡号=${jti}  签发=${new Date(now * 1000).toISOString()}  到期=${expText}`,
);
console.log("\n把下面这一整行 TOKEN 发给该成员（启动 xcat.exe 时粘贴）：\n");
console.log(token);
console.log("\n台数配额在服务端台账按 uid 配置；加人无需重新发版。");
console.log(`记下卡号 ${jti}：这张卡万一泄露，可只废它而不影响该成员的其他卡——`);
console.log(`  POST /twms/admin/access {"action":"revokejti","jti":"${jti}"}`);
