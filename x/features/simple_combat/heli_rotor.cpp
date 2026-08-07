// heli_rotor — F5 Impact 贴怪旋翼环实现。设计说明见 heli_rotor.h。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "heli_rotor.h"

#include "../invuln/invuln.h"
#include "../kick_sniff/kick_sniff.h"
#include "../ports/map_bounds_port.h"
#include "../ports/skill_port.h"
#include "../ports/teleport_port.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <atomic>
#include <cmath>
#include <cstddef>

namespace x::features::simple_combat::heli {
namespace {

// 发射节奏：~11Hz。每发一次 = 一个主线程泵 job，太密会拖帧；太疏则两拍之间重力
// 累积的落差变大（90ms = 3 个物理步 = 180px/s 锯齿 ≈ 8px 位置起伏，可接受）。
constexpr DWORD kIssueMs = 90;
// 紧急档要按 tick 率走（旋翼 ~55Hz）。已出界还在往外飞时，每被节奏闸拦一拍就多滑
// 一个 v*Δt：BIN 1ce9a0 里连拦两拍 40ms、以 545px/s 多滑 22px，那正是剩余的全部出界深度。
// 紧急是短促的几拍，按 tick 发不构成持续高频。
constexpr DWORD kIssueEmergencyMs = 20;

// ── 实测物理常量（见头文件 ②③⑤）────────────────────────────────────
constexpr float kGravityPerStep = 60.f;  // px/s，每个物理步
constexpr float kPhysicsStepMs = 30.f;
// 配平窗口上限。主线程长卡顿后 sinceMs 可能是几秒，按实算会发出一发火箭把角色
// 顶穿图顶；超过这个窗口就认了这段下坠，靠包线保护去救，别用一发巨量冲量赌。
constexpr float kMaxTrimWindowMs = 400.f;

// 死区 = 稳态位置残差的上界。控制律在死区内不产生位移意图，只维持速度 0，所以角色
// 会**停在进入死区的那一点**——而进近总是从上方降下来的，残差就一边倒地偏高。
// BIN 14a58c：死区 30 时中位残差 +18px，叠加当时的 +14 抬升 = 稳定悬在怪上方 32px 砍空气。
//
// 纵向曾经取 30 是怕「怪与角色同台时把站定的角色从台上踹飞」（BIN 79947e）。那个
// 隐患现在由 `onFh` 分支兜住（挂台时 cmdVy 强制 0），不再需要靠宽死区代偿；而 fh-ban
// 悬空时根本没有台可踹。收窄到与横向同级即可，锯齿残余约 4px，不会在死区边缘来回抖。
constexpr float kDeadX = 12.f;
constexpr float kDeadY = 12.f;

// P 增益（px/s per px）。两轴的 P 都只算**期望速度**，真正要发的增量由闭环补差（见 Tick）。
//
// 两轴同增益：竖直曾取 3.0，是历史上「怕掉出图」的遗留代偿。现在防坠有三道独立机制
// （位置包线 + 竖直预刹 + kMaxFallVy 速度闸），不需要再靠钝化增益来间接减速——
// 那样只会让换层进近全程慢 25%，而对真·坠落毫无帮助（坠落是重力驱动的，与 Kp 无关）。
//
// ★ 取值由**实测发射周期**定，不是拍脑袋（BIN adc7b2，482 次发射）：
//    单周期位移 = Kp·err·T ⇒ 残差按 (1 - Kp·T) 逐拍收敛。
//      · |1-Kp·T| < 1（即 Kp·T < 2）才收敛；
//      · Kp·T ≤ 1 则**单调无过冲**，这是我们要的档位。
//    实测 sinceMs：中位 94、p90 106、p99 117、**最大 126**ms。按最坏的 126ms 反解
//    Kp ≤ 1/0.126 = 7.9。取 **7.0**：最坏 7×0.126=0.88（仍无过冲），常态
//    7×0.094=0.66 ⇒ 每拍残差降到 34%，而旧值 4.0 只降到 64%。
//    进近末段是指数收敛段，占了单次进近的大头，这一项比抬速度上限更能省时间。
constexpr float kKpX = 7.0f;
constexpr float kKpY = 7.0f;

// 包线：越过即 emergency，忽略战斗意图先保命。判定框见 heli_rotor.h 的 kEnvSlackX/YPx
// （**外扩** AABB；曾经内缩 72px，把最外沿台上的怪判成禁区，BIN 4a79e4）。
//
// ★★ 落速闸**不是绝对安全阈值，是判别式**——它区分「我想快速下降」与「我失控了」，
//    因此它的含义天生相对于最大下降意图定义，必须跟着意图走。曾经把它写成常量 850，
//    再让用户可调倍率去抬意图，判别式当场反号（BIN 2d6176）：
//      倍率拉到 3.0X ⇒ 下降意图 -1440 < -850 ⇒ 每一拍正常下降都被判失控
//      ⇒ 强行上拉 +300 ⇒ 速度回到闸上方 ⇒ P 控又要 -1440 ⇒ **极限环**。
//    实测：vy<-850 占 55.2%、emg=1 占 37.0%、相邻样本 vy 变号 21.0%、摆幅 -1627~+1319，
//    现象就是用户说的「人物疯狂抖动」。
//    两头都不能松的原理没变：低了误判正常下降，高了让出保护窗口。变的是它得是个函数。
//
// 余量 230 = 锯齿半周期（90ms 周期里重力吃 180px/s，瞬时值绕均值摆 ±90）+ 余量 140。
// 与旧常量自洽：Cruise 620 + 230 = 850，正是原值。
constexpr float kFallGateMarginVy = 230.f;
constexpr float kEnvPushVx = 300.f;
// 救援目标速度（不是增量）。闭环会自己算出「从当前 -600 拉到 +300」需要多大增量。
constexpr float kRescueClimbVy = 300.f;

// 作动器上限：一次能发出的**增量**幅值，两轴同级（合速限幅后两轴权限本就对称）。
//
// 这个数同时也是**最高可达速度**——这一点容易看漏。叠加语义里同向加速走的是
// `cmd = vt` 那条分支（见 VxCommandFor），落地 `clamp(v+vt, ±max(|v|,|vt|))`：想跑到 S
// 就必须发得出 |cmd| = S。发不出就永远停在 kMaxCmdV 上，再喊也没用。
//
// 取值由**安全机动一拍可达**反解，这是真正的硬约束（性能类机动允许摊到多拍，见 ReachableV）：
//   · 坠落自救：从落速闸上界拉回 +kRescueClimbVy —— maxCap + 230 + 300 + 配平 60
//   · 界外内推：从 +maxCap 反到 -kEnvPushVx     —— maxCap + 300
// 前者更严 ⇒ kMaxCmdV ≥ maxCap + 590。10X 下 maxCap = Rtb 660×10 = 6600 ⇒ ≥ 7190，取 8000。
//
// ★ 实测区间已两次外推、两次被实机证实，逐轮记账（都是「意图给多少就到多少、零饱和」）：
//     基线    cmd ≤  900 → v 840↑/670↓     全归档 20363 条遥测
//     3.0X    cmd = 2003 → v 1938          BIN 2e63d5
//     5.0X    cmd = 3231 → v 3100（=620×5，精确到个位）  BIN c05ac2
//   8000 是从 3100 起的 2.1 倍外推——与上一步（1940→4000，2.06 倍）同量级，那步已验。
//   仍属**未验**，但它的失效模式是良性的：若引擎在更低处封顶，表现是**速度上不去**而非
//   失控（可达集钳位保证「落地 == 意图」的等式仍成立，只是意图达不到）。遥测的 cmd 对
//   下一拍 v 可直接证伪，每轮加档必看。
constexpr float kMaxCmdVy = 8000.f;
// 横向同理。这个数**不是**限速阀门：落地速度恒等于 desiredVx（受合速档约束），钳位只咬
// 命令幅值。曾经把它设成档位上限，等于把「从 -556 反向到 +300」需要的 856 削成 340，
// 结果一发只能把速度推到 -256（人还在往外飞），连发四拍才反向 → 出界 43px（BIN c72cff）。
constexpr float kMaxCmdVx = 8000.f;

// 意图上限的**安全兜底**（不是速度档，档位见 CapsFor）。由可救性反解：
//   落速闸 = cap + 230，拉回 +300 还要配平 60，全部要塞进一发 kMaxCmdVy。
// ⇒ cap ≤ kMaxCmdVy − 230 − 300 − 60 = 7410。10X 下 Rtb 6600 < 7410，正常不咬合；
// 它只在有人把倍率上限调过头时兜底，防止出现「救不回来的坠落」。
constexpr float kIntentCeilV =
    kMaxCmdVy - kFallGateMarginVy - kRescueClimbVy - kGravityPerStep;

// ★★ 把意图钳进「这一拍真发得出去」的可达集。**5X 能成立全靠这一步。**
//
// 换算层保证「落地速度 == 意图」，但那是在 cmd 没被 kMaxCmdV 削掉的前提下。一旦削掉，
// 撞墙预刹赖以成立的等式就悄悄失效——预刹按 `desiredV × 视野` 预测本周期位移，等式一破
// 算出的余量就是假的（c72cff 的失效链）。
//
// 历史解法是反过来压意图上限：要求满速反向（增量 2·cap）一拍完成 ⇒ cap ≤ C/2。
// 代价是把最高速度砍成作动器权限的一半，5X 直接无解。而「反向一拍完成」是**性能优化，
// 不是安全需求**——真正必须一拍到位的只有刹停、界外内推、坠落自救，它们的 |vt−v| 都
// ≤ maxCap + 300，已由 kMaxCmdV 的取值覆盖。
//
// 改成直接钳可达集后，等式**按构造**恒成立：反向自然摊到两三拍，每一拍的预测依然精确，
// 最高速度不再打折。可达集（C = 作动器上限）：
//   · 差值分支 cmd = vt−v，要求 |vt−v| ≤ C          ⇒ vt ∈ [v−C, v+C]
//   · 加速分支 cmd = vt，  要求 |vt| ≤ C            ⇒ 合起来 |vt| ≤ max(C, |v|)
//     （|vt| > |v| 才走加速分支，故 max 取到 C 时正好等价于 |vt| ≤ C）
// bias 是竖直的重力前馈：竖直发的是 (vt−v)+ff，可达区间整体平移 −ff。
float ReachableV(float vt, float v, float bias, float cmax) {
    const float av = std::fabs(v);
    const float reach = av > cmax ? av : cmax;
    float lo = v - bias - cmax;
    float hi = v - bias + cmax;
    if (lo < -reach) lo = -reach;
    if (hi > reach) hi = reach;
    if (lo > hi) return vt;  // 退化（|bias| > C）：交给下游钳位，别返回反向的怪值
    return vt < lo ? lo : (vt > hi ? hi : vt);
}

// 把「想要的速度」翻译成「该发的增量」。作动器实测语义（头文件事实①，11/11 样本吻合）：
//     v_new = clamp(v_old + cmd, ±max(|v_old|, |cmd|))
// 关键在那个 max：**引擎不允许一发冲量把你降到比原速更慢**。所以「同向发个小值来减速」
// 是彻底的空操作——`-557 + (-208) = -765` 会被钳回 max(557,208) = 557，速度纹丝不动。
//
// 于是只有一种情况能直接发 vt，其余一律发差值：
//   · 同向**加速**（|vt| > |v|）：发 vt。v+vt 越过 |vt| 被钳回，落地正好 vt。
//   · 其余（同向减速 / 反向 / 刹停）：发 vt - v。此时 max(|v|, |vt-v|) ≥ |vt|，钳位不咬，
//     落地精确等于 vt。vt=0 时它自然退化成 -v，即一发刹停，不需要额外的特例。
float VxCommandFor(float vt, float v) {
    if (vt * v > 0.f && std::fabs(vt) > std::fabs(v)) return vt;
    return vt - v;
}

// 深度出界：超过安全框这么多就判不可救，缴械让引擎接管（见 Bailed()）。
constexpr float kBailoutPx = 420.f;
// 位置与速度持续这么久完全不变 = 状态停更（断线/切图/死亡）。
//
// ★ 必须按**墙钟**计，不能按拍数计。曾经写作「连续 12 拍」，但 Tick 的节拍由调用方决定：
//    F5 战斗 16ms 一拍（窗口 192ms），F6 飞行 8ms 一拍（窗口只剩 96ms）。而一次 96ms 的
//    引擎卡顿就能让 Ap 的四个字段一字不变，于是被误判成断线：gBailed 置起、旋翼停发，
//    偏偏解除条件只有 onFh（落地），净效果就是「自由落体 → 落地 → 自动重飞」。
//    鼠标不动时 PollAimFollow 不再阻塞 worker，拍频跑满，最容易踩中——正是 306d6e 的现象。
//    真·断线是秒级的，取 1s 既远离帧卡顿量级，也不会拖慢真断线后的缴械。
constexpr DWORD kStaleHoldMs = 1000;

// 每档的**意图**上限，不是作动器上限。
//
// ★ 三轴同速（2026-08-07）：竖直曾被压到横向的 40%~70%（Cruise 560/420/240），
//    换层进近因此奇慢。压制的两条理由现在都已失效：
//
//    ① 「下降速度必须低于一个发射周期内能抵消的量，否则一旦下沉就再也拉不平」——
//       这条在**设速度**模型下成立，在实测的**叠加**模型下不成立。叠加语义允许一发
//       冲量把任意速度改写成任意速度（`cmd = vt - v`），从 -560 拉到 +300 只要
//       一发 920，不存在「拉不平」。真正的历史成因是 bea1c3 当年配平写死常数导致
//       净下沉，已由 GravityLoss() 按真实耗时补偿修掉。
//    ② 「竖直作动器只实测到 cmd=300」——已结掉。全归档 20363 条遥测实测：
//       指令发到过 **900**，实际速度上升达 **840** / 下降达 **670**，全程线性无饱和。
//
//    防坠改由三道**独立**机制承担，不再靠钝化速度间接代偿：
//       位置包线（st.y < t）→ 竖直撞墙预刹（见 Tick）→ kMaxFallVy 速度闸 → 深度缴械。
//
// ★ 限幅口径是**合速矢量**，不是分轴（2026-08-07）。分轴钳有个隐蔽的各向异性：
//    同一个「多快算快」，纯横向只能跑 cap，45°斜向却能跑 cap·√2 —— 相差 41%。
//    而实测（BIN adc7b2，770 次进近）**74.4% 的进近横纵比 ≤ 0.1**，即绝大多数
//    赶路是近乎纯单轴的，全都被按最慢的那档限住了：
//      · 各方向实际可达合速：中位 560、p90 634、最大 784
//      · 改按合速钳到 780 后：中位 +39%，而**峰值 784→780 反而略降**
//    这是唯一能大幅提速却不抬高服务端所见峰值的改法，所以先做它、再谈抬上限。
//
//    各档取值 = 原分轴值 × √2 向下取整到十位，即「把斜向早就能跑的速度，允许所有
//    方向都跑」。峰值逐档保持不变：560→780(792)、480→660(679)、340→480(481)、
//    260→360(368)。括号内是旧口径下该档斜向本就可达的合速。
//
// ★★ Cruise 单独回调 780 → 620（BIN 270783）。**峰值不变 ≠ 暴露不变**，这是上面那套
//    推理漏掉的一环，而实机把它打了出来：
//      · 旧口径下 780 只有罕见的 45° 斜向摸得到；改合速后 74% 的纯单轴进近全程都在那儿。
//        `|v|>700` 的时间占比 1.7% → **24.4%**（14 倍），峰值却确实没动（865 → 848）。
//      · 同图同战斗强度下，会话存活 358s → **79s**；全归档 158 次掉线里 <110s 占 34.8%，
//        三次独立全落在该区间的概率 4.2%。
//      · 决定性的是**净产出**：重连空窗恒为 ~48s，占空比 90.9% → 70.8%，
//        墙钟效率 81.8 → **60.1 杀/分（−27%）**。空中省下的时间全被重连吃光，提速是亏的。
//    取 620 而非退回 780 之前的等效值：620 的峰值（约 680，含锯齿）**比改动前的 865 还低**，
//    而 74% 的纯单轴进近仍比旧口径的 560 快 11%。若存活恢复，即证实「高速暴露时长」
//    才是掉线自变量（峰值不是）；若不恢复，则速度整体被排除，回头查 invuln。
struct ModeCaps {
    float speed;  // 合速上限（矢量幅值），两轴共享
};

// 各档 1.0X 基准合速。提出来命名是为了让倍率上限能由它反解（见 kSpeedScaleMax），
// 而不是又一个跟别处对不上的字面量。
constexpr float kBaseCruise = 620.f;
constexpr float kBaseRtb = 660.f;  // 各档最高
constexpr float kBaseStation = 480.f;
constexpr float kBaseHold = 360.f;

// 倍率边界。上限由**可救性**反解：最快的 Rtb 档乘上倍率不得越过 kIntentCeilV，
// 否则坠落拉不回来 ⇒ 7410 / 660 = 11.2X。
//
// ★ 这里曾写死 3.0，与面板的 kHeliSpeedPctMax 各管各的。面板放到 500 之后实机仍只跑
//   3.00X —— BIN 2e63d5 日志原话：`SetFlySpeedPct 500 → 3.00X (req 5.00)`。
//   两道钳位分别硬编码，就一定会有走散的那天；改成同源反解后不可能再错位，
//   static_assert 还会在编译期替我们复核一遍。
constexpr float kSpeedScaleMin = 0.25f;
constexpr float kSpeedScaleMax = kIntentCeilV / kBaseRtb;
static_assert(kBaseRtb * 10.0f <= kIntentCeilV, "10X 必须落在可救性范围内，否则坠落拉不回");

// 当前持有者。语义见 heli_rotor.h 的 Owner 注释：抢占式，被抢者静默 no-op。
std::atomic<unsigned> gOwner{static_cast<unsigned>(Owner::None)};

bool Owns(Owner o) {
    return o != Owner::None && gOwner.load(std::memory_order_acquire) == static_cast<unsigned>(o);
}

// 用户可调倍率，基准 1.0，**按 Owner 各存一份**（手动飞与自动打怪的手感诉求不同）。
// 语义与边界见 heli_rotor.h 的 SetSpeedScale 注释。
constexpr size_t kOwnerCount = 4;  // None / Combat / Travel / Fly
std::atomic<float> gSpeedScale[kOwnerCount] = {{1.0f}, {1.0f}, {1.0f}, {1.0f}};

// 生效的是**当前持有者**那一份：交接时自动跟着换，不需要谁存档/恢复。
float ActiveSpeedScale() {
    const size_t i = static_cast<size_t>(gOwner.load(std::memory_order_acquire));
    return i < kOwnerCount ? gSpeedScale[i].load(std::memory_order_relaxed) : 1.0f;
}

ModeCaps CapsFor(Mode m) {
    const float k = ActiveSpeedScale();
    float s = 0.f;
    switch (m) {
        case Mode::Cruise:
            s = kBaseCruise * k;
            break;
        // Rtb 只许更快：自救权限不该因为用户想慢点打怪而被削。
        case Mode::Rtb:
            s = kBaseRtb * (k > 1.f ? k : 1.f);
            break;
        case Mode::Station:
            s = kBaseStation * k;
            break;
        case Mode::Hold:
            s = kBaseHold * k;
            break;
        default:
            return {0.f};
    }
    // 安全兜底，正常倍率（≤5X）不咬合。越过它坠落就救不回来了，见 kIntentCeilV。
    return {s < kIntentCeilV ? s : kIntentCeilV};
}

// 失控落速的判别线。跟着**最大**下降意图走而非跟当前档走：档间切换时若判别线也跟着跳，
// 一次 Cruise→Station 的正常降档就会把巡航中的合法下降瞬间判成失控。Rtb 恒为各档最高
// （倍率 <1 时它有 1.0 地板），故用它当上界。
//
// 可救性自检（这条链断了就会出现「救不回来的坠落」）：
//   5X 最坏落速 = Rtb(3300) + 余量(230) = 3530
//   拉回 kRescueClimbVy(+300) 需增量 3830 + 配平 60 = 3890 ≤ kMaxCmdVy(4000) ✓
//   ——这正是 kMaxCmdVy 取 4000 的来处，两者是同一约束的两种写法，改一个必须改另一个。
float FallGateVy() { return CapsFor(Mode::Rtb).speed + kFallGateMarginVy; }

// 两次发射之间重力吃掉的速度 = 盈亏线。用**真实耗时**而非标称周期：主线程一拥塞
// 发射就变稀，需要的配平同比变大；写死常数正是历史上净下沉的根源。
float GravityLoss(DWORD sinceMs) {
    float ms = static_cast<float>(sinceMs);
    if (ms < kPhysicsStepMs) ms = kPhysicsStepMs;
    if (ms > kMaxTrimWindowMs) ms = kMaxTrimWindowMs;
    return kGravityPerStep * ms / kPhysicsStepMs;
}

std::atomic<unsigned> gMode{static_cast<unsigned>(Mode::Off)};
std::atomic<float> gSpX{0.f};
std::atomic<float> gSpY{0.f};
std::atomic<float> gSpLeadX{0.f};
std::atomic<float> gSpLeadY{0.f};
std::atomic<bool> gSpUnbounded{false};

DWORD gLastIssueMs = 0;
bool gLastTickFired = false;

std::atomic<bool> gBailed{false};

// 状态停更检测：断线/切图后 QueryFlightState 会一直回同一组死数据，此时任何冲量
// 都是空发。bea1c3 断线后旋翼又对着冻结的 (932,-1188) 发了 500ms。
float gStaleX = 0.f, gStaleY = 0.f, gStaleVx = 0.f, gStaleVy = 0.f;
DWORD gStaleSinceMs = 0;  // 状态开始「一字不变」的时刻；0 = 尚未取过基准

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 返回 true = 数据与基准逐字节相同已持续超过 kStaleHoldMs。
bool UpdateStaleness(const ports::teleport::FlightState& st, DWORD now) {
    if (gStaleSinceMs && st.x == gStaleX && st.y == gStaleY && st.vx == gStaleVx &&
        st.vy == gStaleVy) {
        return (now - gStaleSinceMs) >= kStaleHoldMs;
    }
    gStaleSinceMs = now ? now : 1;  // 0 是「无基准」的哨兵，撞上就借一毫秒
    gStaleX = st.x;
    gStaleY = st.y;
    gStaleVx = st.vx;
    gStaleVy = st.vy;
    return false;
}

}  // namespace

bool Bailed() { return gBailed.load(std::memory_order_acquire); }

void ClearBailed() { gBailed.store(false, std::memory_order_release); }

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Hold:
            return "hold";
        case Mode::Station:
            return "station";
        case Mode::Cruise:
            return "cruise";
        case Mode::Rtb:
            return "rtb";
        default:
            return "off";
    }
}

bool ClampToAirspace(float* x, float* y) {
    if (!x || !y) return false;
    ports::map_bounds::Rect r{};
    if (!ports::map_bounds::QueryPlayBounds(0, &r) || !r.ok) return true;
    const float l = static_cast<float>(r.left) - kEnvSlackXPx;
    const float ri = static_cast<float>(r.right) + kEnvSlackXPx;
    const float t = static_cast<float>(r.top) - kEnvSlackYPx;
    const float b = static_cast<float>(r.bottom) + kEnvSlackYPx;
    if (ri <= l || b <= t) return true;
    if (*x < l) *x = l;
    if (*x > ri) *x = ri;
    if (*y < t) *y = t;
    if (*y > b) *y = b;
    return true;
}

const char* OwnerName(Owner o) {
    switch (o) {
        case Owner::Combat:
            return "combat";
        case Owner::Travel:
            return "travel";
        case Owner::Fly:
            return "fly";
        default:
            return "none";
    }
}

bool Acquire(Owner o) {
    if (o == Owner::None) return false;
    const unsigned prev = gOwner.exchange(static_cast<unsigned>(o), std::memory_order_acq_rel);
    if (prev == static_cast<unsigned>(o)) return false;
    // 易主不清 setpoint：新主第一拍就会写自己的，中间那一小段沿用旧点比归零安全
    // （归零 = 朝地图原点飞）。发射时钟也不重置，否则交接瞬间会多发一拍。
    x::runtime::LogI("Heli", "owner %s -> %s", OwnerName(static_cast<Owner>(prev)), OwnerName(o));
    return true;
}

bool TryAcquire(Owner o) {
    if (o == Owner::None) return false;
    unsigned expect = static_cast<unsigned>(Owner::None);
    if (!gOwner.compare_exchange_strong(expect, static_cast<unsigned>(o),
                                        std::memory_order_acq_rel))
        return Owns(o);  // 已是自己 → 视为成功；被别人占着 → false
    x::runtime::LogI("Heli", "owner none -> %s", OwnerName(o));
    return true;
}

void Release(Owner o) {
    unsigned expect = static_cast<unsigned>(o);
    // 只有当前持有者能释放：被抢占的一方随后调 Release 不该把新主踢下去。
    if (!gOwner.compare_exchange_strong(expect, static_cast<unsigned>(Owner::None),
                                        std::memory_order_acq_rel))
        return;
    x::runtime::LogI("Heli", "owner %s -> none", OwnerName(o));
}

Owner CurrentOwner() { return static_cast<Owner>(gOwner.load(std::memory_order_acquire)); }

void SetSetpoint(Owner o, const Setpoint& sp) {
    if (!Owns(o)) return;
    gSpX.store(sp.x, std::memory_order_release);
    gSpY.store(sp.y, std::memory_order_release);
    gSpLeadX.store(sp.leadVx, std::memory_order_release);
    gSpLeadY.store(sp.leadVy, std::memory_order_release);
    gSpUnbounded.store(sp.unbounded, std::memory_order_release);
    gMode.store(static_cast<unsigned>(sp.mode), std::memory_order_release);
}

Setpoint CurrentSetpoint() {
    Setpoint sp{};
    sp.mode = static_cast<Mode>(gMode.load(std::memory_order_acquire));
    sp.x = gSpX.load(std::memory_order_acquire);
    sp.y = gSpY.load(std::memory_order_acquire);
    sp.leadVx = gSpLeadX.load(std::memory_order_acquire);
    sp.leadVy = gSpLeadY.load(std::memory_order_acquire);
    sp.unbounded = gSpUnbounded.load(std::memory_order_acquire);
    return sp;
}

void Disarm(Owner o) { SetSetpoint(o, Setpoint{}); }

void Reset() {
    // Reset 是「换图/开关」的全局复位，不走 Owns 闸：此时正需要把残留所有权一并清掉，
    // 否则上一张图的持有者会把新图的驱动方一直挡在门外。
    gOwner.store(static_cast<unsigned>(Owner::None), std::memory_order_release);
    gSpX.store(0.f, std::memory_order_release);
    gSpY.store(0.f, std::memory_order_release);
    gSpLeadX.store(0.f, std::memory_order_release);
    gSpLeadY.store(0.f, std::memory_order_release);
    gSpUnbounded.store(false, std::memory_order_release);
    gMode.store(static_cast<unsigned>(Mode::Off), std::memory_order_release);
    gLastIssueMs = 0;
    gLastTickFired = false;
    gBailed.store(false, std::memory_order_release);
    gStaleSinceMs = 0;
    // 倍率是用户设置，不是本轮状态，换图/开关都不该把它冲掉。
}

void SetSpeedScale(Owner o, float scale) {
    if (!std::isfinite(scale)) return;
    const size_t i = static_cast<size_t>(o);
    if (i >= kOwnerCount) return;
    gSpeedScale[i].store(Clamp(scale, kSpeedScaleMin, kSpeedScaleMax), std::memory_order_relaxed);
}

float SpeedScale(Owner o) {
    const size_t i = static_cast<size_t>(o);
    if (i >= kOwnerCount) return 1.f;
    return gSpeedScale[i].load(std::memory_order_relaxed);
}

bool Tick(Owner o, DWORD now, Telemetry* out) {
    if (!Owns(o)) {
        if (out) *out = Telemetry{};
        return false;
    }
    Telemetry tm{};
    const Setpoint sp = CurrentSetpoint();
    tm.mode = sp.mode;
    tm.spX = sp.x;
    tm.spY = sp.y;

    // 状态查询放在 mode 闸之前：Bailed 后飞控会立刻 Disarm，若查询在 Off 之后就永远
    // 看不到「已落地」这个复位条件，缴械状态会卡死到下一次换图。
    ports::teleport::FlightState st{};
    const bool haveState = ports::teleport::QueryFlightState(st) && st.ok;
    if (haveState) {
        tm.haveState = true;
        tm.x = st.x;
        tm.y = st.y;
        tm.vx = st.vx;
        tm.vy = st.vy;
        tm.onFh = st.onFh;
        if (st.onFh) {  // 引擎把人接住了 = 脱险，可重新武装
            if (gBailed.exchange(false, std::memory_order_acq_rel)) {
                x::runtime::LogI("Heli", "bail cleared onFh at (%.0f,%.0f)", st.x, st.y);
            }
            gStaleSinceMs = 0;
        }
    }

    if (sp.mode == Mode::Off) {
        tm.guard = "off";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (!haveState) {
        tm.guard = "no_state";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (Bailed()) {
        tm.guard = "bailed";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    // soft settle / NM 已断：禁止任何 Owner 继续发 Impact（752824：settle 窗内仍 Firing
    // + heli，约 1.2s 后 Classic 自退；SawDisconnect 粘性不能当闸，只用 hold + 当前态）。
    if (x::features::soft_login_probe::IsHoldActive()) {
        tm.guard = "soft_hold";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    {
        const int nm = kick_sniff::LastSessionState();
        // CMS: Disconnecting=0 Disconnected=1（kick_sniff）；-1=尚未采样，放行。
        if (nm == 0 || nm == 1) {
            tm.guard = "nm_down";
            gLastTickFired = false;
            if (out) *out = tm;
            return false;
        }
    }
    if (UpdateStaleness(st, now)) {  // 断线/切图：再发也只是对着死数据空转
        if (!gBailed.exchange(true, std::memory_order_acq_rel)) {
            x::runtime::LogW("Heli", "bail stale owner=%s pos=(%.0f,%.0f) v=(%.0f,%.0f) held=%ums",
                             OwnerName(o), st.x, st.y, st.vx, st.vy,
                             static_cast<unsigned>(now - gStaleSinceMs));
        }
        tm.guard = "stale";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }

    const ModeCaps caps = CapsFor(sp.mode);
    const DWORD sinceMs = gLastIssueMs ? (now - gLastIssueMs) : static_cast<DWORD>(kIssueMs);
    const float trim = GravityLoss(sinceMs);
    tm.sinceMs = sinceMs;
    tm.trimVy = trim;

    // 两轴都先算「这一周期想要的速度」，作动器增量最后统一换算。
    //
    // 意图 = 目标自身速度（前馈）+ Kp·误差（反馈）。前馈项负责「跟上」，反馈项只负责
    // 「补掉剩下的偏差」；少了前馈就只能靠偏差换速度，于是跟移动目标恒留 V目标/Kp 的滞后
    // （见 Setpoint::leadVx）。它必须加在这里、即所有钳位之前 —— 档位限幅、包线内推、
    // 撞墙预刹、可达集依旧在下游最后说话，安全性一分不减。
    const float errX = sp.x - st.x;
    const float errY = sp.y - st.y;
    float desiredVx = sp.leadVx;
    if (std::fabs(errX) > kDeadX) desiredVx += kKpX * errX;

    // 竖直：先定「这一周期想要的平均速度」，作动器指令另算。+Y 向上 ⇒ errY>0 = 往上。
    // 挂着台时不给前馈：地板上的水平/竖直语义由引擎走路负责，硬塞竖直意图会把人抠离地板。
    float desiredVy = st.onFh ? 0.f : sp.leadVy;
    if (!st.onFh && std::fabs(errY) > kDeadY) desiredVy += kKpY * errY;

    // ── 安全包线 ────────────────────────────────────────────────────
    bool emergency = false;
    if (!st.onFh && st.vy < -FallGateVy()) {  // 高速下坠：弃战拉平
        desiredVy = kRescueClimbVy;
        emergency = true;
    }
    ports::map_bounds::Rect r{};
    if (ports::map_bounds::QueryPlayBounds(0, &r) && r.ok) {
        const float rawL = static_cast<float>(r.left);
        const float rawR = static_cast<float>(r.right);
        // 外扩成合法空域，且两轴余量不同：AABB 是**可站立面**，最低/最高台上站着怪是常态，
        // 竖直必须让出整条出刀带才够得着（见 heli_rotor.h 的 kEnvSlackXPx / kEnvSlackYPx）。
        const float l = rawL - kEnvSlackXPx;
        const float ri = rawR + kEnvSlackXPx;
        const float t = static_cast<float>(r.top) - kEnvSlackYPx;
        const float b = static_cast<float>(r.bottom) + kEnvSlackYPx;
        if (ri > l && b > t) {
            // ── 向下：恒生效，不受 sp.unbounded 放行 ──────────────────
            // 四个方向里只有这一个会毁掉会话（穿出最低台 ⇒ 引擎判掉图 ⇒ 重载/断线，
            // 即 a69130 野猪图那次「越界重拉」）。左右和上方都是纯空气，飞出去最坏什么
            // 也不发生，所以那三面允许手动驾驶放开；这一面对谁都不放。
            // Rect 的 top/bottom 是**数值**含义（top=min y）；+Y 向上 ⇒ top 才是图底。
            if (st.y < t) {  // 已跌破最低台：全力上拉
                desiredVy = kRescueClimbVy;
                emergency = true;
                if (st.y < t - kBailoutPx) {
                    // 掉这么深已经不是控制问题，冲量也追不回来。缴械让引擎接管落地/复位，
                    // 别像 bea1c3 那样对着注定的结局继续空发到断线。
                    if (!gBailed.exchange(true, std::memory_order_acq_rel)) {
                        x::runtime::LogW("Heli", "bail deep owner=%s y=%.0f floor=%.0f depth=%.0f",
                                         OwnerName(o), st.y, t, t - st.y);
                    }
                    tm.guard = "bailout";
                    gLastTickFired = false;
                    if (out) *out = tm;
                    return false;
                }
            }

            // ── 左右 / 上方：手动驾驶时整段让位给驾驶员（见 Setpoint::unbounded）──
            if (!sp.unbounded) {
                if (st.x < l) {
                    desiredVx = kEnvPushVx;
                    emergency = true;
                } else if (st.x > ri) {
                    desiredVx = -kEnvPushVx;
                    emergency = true;
                }
                // 与横向**各自独立**判断（别串成 else if）。这里可以只写 st.y > b：
                // 它与上面的 st.y < t 因 b > t 而互斥，等价于原先那条 else if。
                if (st.y > b) {
                    // 已在最高台之上：下降是免费的，给个受控下沉速度而不是放任自由落体。
                    // 用固定值而非 caps.speed，理由见 kEnvSinkVy。
                    desiredVy = -kEnvSinkVy;
                    emergency = true;
                }
            }

            // ── 撞墙预刹 ────────────────────────────────────────────
            // 落地速度恒等于 desiredV（横向由 VxCommandFor 保证、竖直由闭环补差保证），
            // 于是「本周期位移 ≈ desiredV × 周期」是可预测的。只要让这段位移落在边界内，
            // 就不可能冲出去——从源头消灭过冲。事后靠 RTB 拉回是追不上的：
            // kEnvPushVx=300 反不过 448 的入射速度。
            //（注：这里**不是**「设速度语义」。作动器是叠加的，见 heli_rotor.h 事实③；
            // 能这么算是因为换算层已把落地速度钉死在 desiredV 上，不是因为引擎直接设速。）
            //
            // BIN c9b8dc：怪在最左台，角色带 vx=-448 扑过去，AABB 左沿 -585 却冲到 -658
            //（出界 73px）并在界外继续出刀，服务端判非法坐标，一轮掉线两次。
            //
            // 视野取 150ms 而非标称的 90ms：主线程一卡顿发射就变稀（实测 since 到过
            // 106ms），按标称算会刹不住。早刹几十毫秒不影响出刀，冲出去要掉线。
            // 视野按「一次错过的发射」给足：紧急档 45ms，卡顿时实测能拖到 106ms，取 200ms。
            constexpr float kBrakeHorizonSec = 0.200f;
            // 刹车线从 AABB 内缩 kBrakeInsetXPx，让稳态停靠点离墙留出余量而不是紧贴 rawR
            //（成因与「为什么横向能缩、竖直不能」见 heli_rotor.h 的 kBrakeInsetXPx）。
            // 窄图兜底：内缩量不超过图宽的 1/4，两条刹车线永远不会交叉——交叉了这两句钳位
            // 会互相打架，把人钉死在某一侧。
            float insetX = (rawR - rawL) * 0.25f;
            if (insetX > kBrakeInsetXPx) insetX = kBrakeInsetXPx;
            if (insetX < 0.f) insetX = 0.f;
            if (!sp.unbounded) {
                const float roomR = (rawR - insetX - st.x) / kBrakeHorizonSec;  // 允许的最大 +vx
                const float roomL = (rawL + insetX - st.x) / kBrakeHorizonSec;  // 允许的最小 vx（界内为负）
                // 已在刹车线之外时 room 同号，这两句会把意图顶成内推，与紧急推同向且力度更足。
                if (desiredVx > roomR) desiredVx = roomR;
                if (desiredVx < roomL) desiredVx = roomL;
            }

            // 埋点：内缩之后还能贴到离 AABB 半个内缩量以内，说明有东西绕过了这条钳位。
            // 只在跨过阈值的那一拍打一行，不刷屏；平时一行都不该出现。
            // 自由空域下贴墙是驾驶员的意图，不是异常，不记。
            if (!sp.unbounded) {
                const float dR = rawR - st.x;
                const float dL = st.x - rawL;
                const float clearance = dR < dL ? dR : dL;
                static std::atomic<bool> gWallTight{false};
                const bool tight = clearance < insetX * 0.5f;
                if (tight && !gWallTight.exchange(tight, std::memory_order_acq_rel)) {
                    x::runtime::LogW("Heli",
                                     "wall tight owner=%s x=%.0f clr=%.0f vx=%.0f sp=%.0f aabb=[%.0f,%.0f]",
                                     OwnerName(o), st.x, clearance, st.vx, sp.x, rawL, rawR);
                } else if (!tight) {
                    gWallTight.store(false, std::memory_order_release);
                }
            }

            // 竖直预刹 —— 竖直提速到与横向同级后，这一段是**必需品**而非对称美化：
            // 620px/s 下降在一个 90ms 周期里走 56px，位置包线是「越过才报警」的事后判据，
            // 等它发现时人已经在界外几十 px。横向冲出去是掉线，竖直冲出去是掉出地图。
            //
            // 用**外扩后**的 t/b（合法空域）而非 raw：贴地面层的怪就站在 rawT 上，
            // 站位点本身在 rawT 附近，距 t 还有整整 kEnvSlackYPx 的余量，正常作战根本
            // 碰不到这条线；它只在锯齿或卡顿导致的异常下沉时才咬合。
            // 刹车线同样从空域上下沿内缩，让稳态停靠点落在紧急触发线**里侧** 24px —— 否则
            // 目标点被钳到 b、绊线也在 b，人就贴着开关悬停，过冲 1px 就吃一发下压。
            // 成因、几何免费性与 BIN a0ab58 的实测见 heli_rotor.h 的 kBrakeInsetYPx。
            // 矮图兜底同 X：内缩量不超过空域高度的 1/4，两条刹车线永不交叉。
            float insetY = (b - t) * 0.25f;
            if (insetY > kBrakeInsetYPx) insetY = kBrakeInsetYPx;
            if (insetY < 0.f) insetY = 0.f;
            //
            // 上沿那条随自由空域一起让位；**底侧那条不让** —— 它才是「不许俯冲出图」的实际
            // 执行者：位置包线是越过才报警的事后判据，5X 一个周期就能走过整个 kBailoutPx，
            // 只留位置判据等于没留。放开上沿、留住底侧，正是「四面里只关会摔死的那一面」。
            const float roomB = (b - insetY - st.y) / kBrakeHorizonSec;  // 允许的最大 +vy（上升）
            const float roomT = (t + insetY - st.y) / kBrakeHorizonSec;  // 允许的最小 vy（界内为负）
            if (!sp.unbounded && desiredVy > roomB) desiredVy = roomB;
            if (desiredVy < roomT) desiredVy = roomT;
        }
    }
    tm.emergency = emergency;

    // 档位限幅：按**合速矢量**等比缩，不分轴（口径与实测依据见 CapsFor）。
    //
    // 仍然放在预刹**之后**，与分轴时代同序：预刹给的是「离边界还有多远所以最多能多快」，
    // 档位给的是「这个模式下最多想多快」，两者取交集，谁更严谁生效。等比缩只会减小幅值，
    // 不会让任一轴反超预刹给的余量，所以后钳不会撤销前者。
    //
    // ★ 这一钳还兼着一个**必需**的兜底：界外时预刹的 room 与位移同号，那两句会把意图顶成
    //   「(出界深度)/0.2」的大幅内推——深出界 400px 就是 2000px/s。分轴时代靠这里的
    //   Clamp 兜住，改合速后若把限幅前移到包线之前，这个值就没人管了。
    //
    // 曾经这里写着「下降必须低于一个发射周期能抵消的量，否则再也拉不平」——那条已随
    // 叠加语义的实证作废（见 CapsFor 注释①）。bea1c3 送出图的真凶是配平写死常数，
    // 不是下降太快。
    {
        const float mag = std::sqrt(desiredVx * desiredVx + desiredVy * desiredVy);
        if (mag > caps.speed && mag > 1e-3f) {
            const float k = caps.speed / mag;
            desiredVx *= k;
            desiredVy *= k;
        }
    }
    // ── 可达集钳位（必须在换算成增量之前）────────────────────────────
    // 让「落地速度 == 意图」按构造成立，撞墙预刹与竖直预刹的预测才是真的。原理与它
    // 取代的那条 cap ≤ C/2 约束，见 ReachableV。
    //
    // 放在预刹与档位限幅**之后**：它只在意图本身这一拍够不着时才收窄，而刹停/内推/救援
    // 这些安全机动恒满足 |vt−v| ≤ maxCap+300 ≤ C，一拍可达，不会被它撤销（见 kMaxCmdVx）。
    // 会被摊到多拍的只有「满速换靶到反方向」这类纯性能场景。
    const float feedforward = trim * 0.5f + kGravityPerStep * 0.5f;
    desiredVx = ReachableV(desiredVx, st.vx, 0.f, kMaxCmdVx);
    if (!st.onFh) desiredVy = ReachableV(desiredVy, st.vy, feedforward, kMaxCmdVy);
    tm.desiredVy = desiredVy;

    // 叠加语义 ⇒ 发的是**增量**：把当前速度补到目标，再预付本周期的重力损耗。
    //
    // 目标是让**周期平均**速度等于 desiredVy（平均为 0 才是真不漂）。设发射前速度 vp、
    // 周期 N 步，冲量落地那一帧也吃一步重力（头文件事实③），于是周期内速度依次为
    // vp+cmd-60, vp+cmd-120, …, vp+cmd-60N，平均 = vp + cmd - (trim/2 + 30)。
    // 令其等于 desiredVy 即得下式。末尾那个 kGravityPerStep/2 就是"落地帧那一步"，
    // 漏掉它稳态会稳定下沉 30px/s —— 十秒 300px，正是过去"看着在悬停却越飘越低"的量级。
    float cmdVy = st.onFh ? 0.f : (desiredVy - st.vy) + feedforward;

    // 限幅落在**意图速度**上（这才是落地速度），再换算成增量。反过来钳增量会把反向
    // 所需的 |vt|+|v| 削掉，那正是 c72cff 里刹不住的原因。
    tm.desiredVx = desiredVx;
    float cmdVx = VxCommandFor(desiredVx, st.vx);

    // 闸门之前就落盘：被闸掉的拍也要能在日志里看出「本来想发多少」，否则
    // cadence 行一律显示 cmd=(0,0)，排查时会被误读成「控制律算出了 0」。
    cmdVx = Clamp(cmdVx, -kMaxCmdVx, kMaxCmdVx);
    cmdVy = Clamp(cmdVy, -kMaxCmdVy, kMaxCmdVy);
    tm.cmdVx = cmdVx;
    tm.cmdVy = cmdVy;

    // ── 闸门 ─────────────────────────────────────────────────────────
    // emergency 只让过节奏闸与技能前置闸；无敌闸不能绕（Impact 端口自身也会拒）。
    if (!x::features::invuln::IsEnabled()) {
        tm.guard = "invuln_off";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    if (!emergency) {
        // 拥塞闸只在**挂着台**时生效：脚下有地板，少发几拍无非是站着不动。
        // 悬空时反过来——拥塞正是最需要旋翼续命的时刻，此时让闸拦下就是自由落体，
        // 而卡顿又恰好是历史掉图的放大器。改由 trim 按真实耗时补偿这段空窗。
        if (st.onFh && x::runtime::main_thread::IsCongested()) {
            tm.guard = "congested";
            gLastTickFired = false;
            if (out) *out = tm;
            return false;
        }
        int prepSkill = 0;
        if (ports::skill::IsPreparingSkill(&prepSkill)) {
            tm.guard = "skill_prep";
            gLastTickFired = false;
            if (out) *out = tm;
            return false;
        }
    }
    const DWORD cadence = emergency ? kIssueEmergencyMs : kIssueMs;
    if (gLastIssueMs && (now - gLastIssueMs) < cadence) {
        tm.guard = "cadence";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }

    if (!emergency && std::fabs(cmdVx) < 8.f && std::fabs(cmdVy) < 8.f) {
        tm.guard = "deadband";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }

    ports::teleport::ImpactVelOpts vopts{};
    // 端口这层只做防呆兜底。按档位上限收窄会削掉反向所需的大增量（同 cmdVy 那条注释）；
    // 真正的限速是上面的 desiredVx——落地速度恒等于它，与命令幅值无关。
    vopts.maxAbsVx = kMaxCmdVx;
    // 端口这一层只做防呆兜底，真正的意图限幅在上面的 desiredVy；这里再按档位收窄
    // 会把「从 -600 拉平」需要的那一发大增量削掉，等于恢复历史上的净下沉。
    vopts.maxAbsVy = kMaxCmdVy;
    vopts.minAbs = 6.f;  // 叠加语义下写 0 是空操作，跳过即可省一个泵 job
    vopts.quietLog = true;
    const bool ok = ports::teleport::ImpactSetVelocity(
        cmdVx, cmdVy, ports::teleport::ImpactRoute::SetImpactNext, vopts);
    if (!ok) {
        tm.guard = "impact_fail";
        gLastTickFired = false;
        if (out) *out = tm;
        return false;
    }
    gLastIssueMs = now;
    gLastTickFired = true;
    tm.fired = true;
    if (out) *out = tm;
    return true;
}

}  // namespace x::features::simple_combat::heli
