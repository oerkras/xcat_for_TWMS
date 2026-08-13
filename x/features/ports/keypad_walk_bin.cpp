// Classic TWMS — 只读 KeyPad/走路采证（docs MoveElem §11.12）。
// 不写 A/B/C、不改 Query bit0、不 SetInput。仅观察：
//   PackState 返回值（Slot4 透传钩）+ MainPump 帧末 Ap/inX/latch/OS键
//   + MovePath 尾段 Attr（挨打击退时钉 a=12 / Jump / Normal）。
// 默认关；排障设 XCAT_WALK_BIN=1（日志巨大，勿日常开）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "keypad_walk_bin.h"

#include "player_combat_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/mono_clock.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x::features::ports::keypad_walk_bin {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;
using x::runtime::NowMs;

// 注意：这个哈希解析出来的类**不是** KeyPad，而是 Rand32（伪随机数生成器）。
// 运行期真值 origRva=0x1AEC320 与 dump 对上，而该 RVA 在 dump 里是 `Rand32.Random()`，
// 反编译确认是 xorshift（三状态字、移位 13/19 · 4/25 · 8/11）；`Random()` 正是 Rand32 的
// 首个自有虚方法，落在 vtable slot 4（前四槽属 Object）—— 四条证据一致。
// 因此曾经挂在 slot4 上的「PackState 钩子」实际挂的是 PRNG：读数全是随机噪声，
// 且它每帧被调多次，把 BIN 的空闲节流整体旁通。钩子已拆除，此处只保留字段观测。
// A/B/C/p28 仍作为该单例的指纹留用；slot4 打出原始 RVA 便于换构建时核对。
constexpr char kHashKeyPad[] =
    "d193aa6d20957fb0a38f8189469763371e72f04982c35534a7c6d2364d86bbe";
constexpr size_t kOffKeyPadSlot4 = 0x178;
constexpr size_t kOffKpA = 0x10;
constexpr size_t kOffKpB = 0x14;
constexpr size_t kOffKpC = 0x18;
constexpr size_t kOffKpPlus28 = 0x28;
constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kOffVcInputX = 0x50;
constexpr size_t kOffVcMoveAction = 0x84;
constexpr size_t kOffVcMovePath = 0x78;
constexpr size_t kOffVcApX = 0x98;
constexpr size_t kOffVcApVx = 0xA8;
constexpr size_t kOffVcApVy = 0xB0;  // AbsPos.Vy
// MovePath / MoveElem（与 movepath_flush_probe 同源）
constexpr size_t kOffMpElemList = 0x30;
constexpr size_t kOffMpElemLast = 0x38;
constexpr size_t kOffElAttr = 0x10;
constexpr size_t kOffElVx = 0x16;
constexpr size_t kOffElVy = 0x18;
constexpr size_t kOffElFh = 0x1C;
// docs：锁存对象 @ vc+0x100；若空则扫邻域。
constexpr size_t kCandLatchOffs[] = {0xF0, 0xF8, 0x100, 0x108, 0x110, 0x118};

std::atomic<bool> gEnabled{false};
std::atomic<bool> gInited{false};
void* gKeyPadKlass = nullptr;
void* gKeyPadSing = nullptr;
DWORD gSingRebindMs = 0;
size_t gOffVecCtrl = kFbVecCtrl;
size_t gLatchObjOff = 0x100;
bool gLatchOffResolved = false;

FILE* gLog = nullptr;
CRITICAL_SECTION gLogCs{};
bool gLogCsInit = false;

double gLastApX = 0.0;
bool gHaveAp = false;
int gLastAttrLogged = -1;

uint8_t RdU8(void* p, size_t off) {
    if (!p) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int16_t RdI16(void* p, size_t off) {
    if (!p) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int32_t RdI32(void* p, size_t off) {
    if (!p) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

const char* AttrName(uint8_t a) {
    switch (a) {
        case 0: return "Normal";
        case 1: return "Jump";
        case 2: return "Impact";
        case 3: return "Immediate";
        case 4: return "Teleport";
        case 12: return "MobPowerKnockBack";
        case 14: return "StartFalldown";
        case 15: return "FallDown";
        case 19: return "CustomImpact";
        default: return "?";
    }
}

// 读 _elemLast + 尾部最多 4 段的 Attr；out 形如 aLast=12(MobPowerKnockBack)! non0=12 n=5 lastV=(200,300) fh=0
// 返回 aLast（无 mp 时 -1）。
int SampleMoveAttr(void* vc, char* out, size_t outN) {
    if (!out || outN == 0) return -1;
    out[0] = 0;
    if (!LooksLikeHeapPtr(vc)) {
        snprintf(out, outN, "aLast=-");
        return -1;
    }
    void* mp = nullptr;
    __try {
        mp = ReadPtr(vc, kOffVcMovePath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mp = nullptr;
    }
    if (!LooksLikeHeapPtr(mp)) {
        snprintf(out, outN, "aLast=- no_mp");
        return -1;
    }
    x::runtime::il2cpp_container::Ensure();
    void* list = ReadPtr(mp, kOffMpElemList);
    void* last = ReadPtr(mp, kOffMpElemLast);
    void* arr = list ? ReadPtr(list, x::runtime::il2cpp_container::OffListItems()) : nullptr;
    int32_t size = list ? RdI32(list, x::runtime::il2cpp_container::OffListSize()) : 0;
    if (size < 0) size = 0;
    const size_t dataOff = x::runtime::il2cpp_container::OffArrayData();

    int aLast = -1;
    int16_t lvx = 0, lvy = 0, lfh = 0;
    if (LooksLikeHeapPtr(last)) {
        aLast = static_cast<int>(RdU8(last, kOffElAttr));
        lvx = RdI16(last, kOffElVx);
        lvy = RdI16(last, kOffElVy);
        lfh = RdI16(last, kOffElFh);
    }

    uint32_t seenMask = 0;
    const int take = size > 4 ? 4 : size;
    const int start = size - take;
    for (int i = start; i < size; ++i) {
        void* el =
            arr ? ReadPtr(arr, dataOff + static_cast<size_t>(i) * sizeof(void*)) : nullptr;
        if (!LooksLikeHeapPtr(el)) continue;
        const uint8_t a = RdU8(el, kOffElAttr);
        if (a < 32) seenMask |= (1u << a);
        if (aLast < 0) aLast = static_cast<int>(a);
    }

    char non0[72];
    int nz = 0;
    for (int a = 1; a < 32; ++a) {
        if (seenMask & (1u << a)) {
            nz += snprintf(non0 + nz, sizeof(non0) - static_cast<size_t>(nz), "%s%d(%s)",
                           nz ? "," : "", a, AttrName(static_cast<uint8_t>(a)));
            if (nz >= static_cast<int>(sizeof(non0)) - 8) break;
        }
    }
    if (nz == 0) snprintf(non0, sizeof(non0), "none");

    const char* chg = "";
    if (aLast >= 0 && aLast != gLastAttrLogged && gLastAttrLogged >= 0) chg = "!";
    if (aLast >= 0) gLastAttrLogged = aLast;

    if (aLast < 0) {
        snprintf(out, outN, "aLast=- n=%d non0=%s", (int)size, non0);
    } else {
        snprintf(out, outN, "aLast=%d(%s)%s n=%d non0=%s lastV=(%d,%d) fh=%d", aLast,
                 AttrName(static_cast<uint8_t>(aLast)), chg, (int)size, non0, (int)lvx, (int)lvy,
                 (int)lfh);
    }
    return aLast;
}

void EnsureLogCs() {
    if (gLogCsInit) return;
    InitializeCriticalSection(&gLogCs);
    gLogCsInit = true;
}

void OpenLog() {
    EnsureLogCs();
    EnterCriticalSection(&gLogCs);
    if (!gLog) {
        char path[MAX_PATH]{};
        snprintf(path, sizeof(path), "%slogs\\keypad_walk_bin.log", x::runtime::GetBinDir());
        gLog = fopen(path, "a");
        if (gLog) {
            setvbuf(gLog, nullptr, _IONBF, 0);
            fprintf(gLog,
                    "# keypad_walk_bin RO — 关F5/拟人；空闲/按→/按← 各≥1s；挨打看 aLast/non0\n"
                    "# ts keys slot4 A B C p28 latch inX ma ApX dAp vx vy aLast n non0 lastV fh sing\n");
        }
    }
    LeaveCriticalSection(&gLogCs);
}

void LogLine(const char* fmt, ...) {
    OpenLog();
    if (!gLog) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    EnterCriticalSection(&gLogCs);
    fprintf(gLog, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(gLog, fmt, ap);
    va_end(ap);
    fputc('\n', gLog);
    LeaveCriticalSection(&gLogCs);
}

bool EnvOn() {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_WALK_BIN", buf, sizeof(buf));
    if (n == 0) return false;  // 缺省关（采证已完成；开着日志巨大）
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

char KeysTag() {
    const bool L = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    const bool R = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    if (L && R) return 'B';
    if (L) return 'L';
    if (R) return 'R';
    return '-';
}

void* EnsureSingleton() {
    const DWORD now = GetTickCount();
    if (gKeyPadSing && LooksLikeHeapPtr(gKeyPadSing) && (now - gSingRebindMs) < 2000) {
        void* k = nullptr;
        __try {
            k = *reinterpret_cast<void**>(gKeyPadSing);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            k = nullptr;
        }
        if (LooksLikeHeapPtr(k)) return gKeyPadSing;
        gKeyPadSing = nullptr;
    }
    if (!x::runtime::il2cpp::Ensure()) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!gKeyPadKlass) gKeyPadKlass = x::runtime::il2cpp::FindClass("", kHashKeyPad);
    if (!gKeyPadKlass || !e.classStaticData) return nullptr;
    void* sf = nullptr;
    __try {
        sf = e.classStaticData(gKeyPadKlass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sf = nullptr;
    }
    if (!LooksLikeHeapPtr(sf)) return nullptr;
    void* sing = nullptr;
    __try {
        sing = *reinterpret_cast<void**>(sf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sing = nullptr;
    }
    if (!LooksLikeHeapPtr(sing)) return nullptr;
    gKeyPadSing = sing;
    gSingRebindMs = now;
    return sing;
}

int ReadLatchX(void* vc, size_t* usedOff) {
    if (!vc) return -999;
    auto tryOff = [&](size_t off) -> int {
        void* obj = nullptr;
        __try {
            obj = ReadPtr(vc, off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -998;
        }
        if (!LooksLikeHeapPtr(obj)) return -997;
        int x = -996;
        __try {
            x = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + 0x10);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -995;
        }
        if (usedOff) *usedOff = off;
        return x;
    };

    if (gLatchOffResolved) {
        const int x = tryOff(gLatchObjOff);
        if (x > -900) return x;
        gLatchOffResolved = false;
    }
    for (size_t off : kCandLatchOffs) {
        const int x = tryOff(off);
        // 合法锁存多为 -1/0/+1；也接受短暂其它小整数
        if (x >= -2 && x <= 2) {
            gLatchObjOff = off;
            gLatchOffResolved = true;
            return x;
        }
    }
    // 不回退读「像堆指针但值怪」的邻域——会把垃圾当 latch（BIN 曾误钉 0xF8）。
    return -999;
}

void BinFrameTick(void*) {
    if (!gEnabled.load(std::memory_order_relaxed)) return;

    void* sing = EnsureSingleton();
    uint32_t A = 0, B = 0, C = 0, p28 = 0;
    uint32_t slot4rva = 0;
    if (sing) {
        auto* u8 = reinterpret_cast<uint8_t*>(sing);
        __try {
            A = *reinterpret_cast<uint32_t*>(u8 + kOffKpA);
            B = *reinterpret_cast<uint32_t*>(u8 + kOffKpB);
            C = *reinterpret_cast<uint32_t*>(u8 + kOffKpC);
            p28 = *reinterpret_cast<uint32_t*>(u8 + kOffKpPlus28);
            void* ik = *reinterpret_cast<void**>(sing);
            if (LooksLikeHeapPtr(ik)) {
                void* fn = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ik) + kOffKeyPadSlot4);
                HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
                if (ga && fn) {
                    slot4rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn) -
                                                     reinterpret_cast<uintptr_t>(ga));
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    int inX = -999, ma = -999, latch = -999;
    double apx = 0.0, vx = 0.0, vy = 0.0, dAp = 0.0;
    void* vc = nullptr;
    player_combat::CombatCtx ctx{};
    if (player_combat::QueryCombatCtx(ctx) && ctx.ok && LooksLikeHeapPtr(ctx.localUser)) {
        __try {
            vc = ReadPtr(ctx.localUser, gOffVecCtrl);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vc = nullptr;
        }
        if (LooksLikeHeapPtr(vc)) {
            size_t latchOff = 0;
            latch = ReadLatchX(vc, &latchOff);
            __try {
                inX = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcInputX);
                ma = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
                apx = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(vc) + kOffVcApX);
                vx = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(vc) + kOffVcApVx);
                vy = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(vc) + kOffVcApVy);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            if (gHaveAp) dAp = apx - gLastApX;
            gLastApX = apx;
            gHaveAp = true;
        }
    }

    char attrBuf[192];
    const int aLast = SampleMoveAttr(vc, attrBuf, sizeof(attrBuf));

    const char keys = KeysTag();

    // 有键、位移、硬冲量、或 Attr 非 Normal —— 才打一行；空闲每 500ms 基线。
    // 原先还有个「本帧 Pack」条件，但那个计数来自 PRNG 钩子、几乎恒真，等于把节流旁通了。
    static DWORD sIdle = 0;
    const DWORD now = NowMs();
    const bool knockLike = (keys == '-') && (vx > 150.0 || vx < -150.0 || vy > 80.0 || vy < -80.0);
    const bool attrHot = (aLast > 0);
    const bool interesting = (keys != '-') || (dAp > 0.5 || dAp < -0.5) || knockLike || attrHot;
    if (!interesting) {
        if (sIdle && now - sIdle < 500) return;
        sIdle = now;
    } else {
        sIdle = now;
    }

    // kind=BASE/LIVE 曾按 slot4 是否等于「PackState RVA」分类，而那个 RVA 实为 Rand32.Random()，
    // 两个标签都名不副实，已去掉；slot4 打原始 RVA 就够作指纹。
    LogLine("keys=%c slot4=0x%X A=%u B=%u C=%u p28=%u latch=%d@0x%X inX=%d ma=%d "
            "ApX=%.2f dAp=%.2f vx=%.2f vy=%.2f %s sing=%p",
            keys, slot4rva, A, B, C, p28, latch, (unsigned)gLatchObjOff, inX, ma, apx, dAp, vx, vy,
            attrBuf, sing);
}

}  // namespace

bool Enabled() { return gEnabled.load(std::memory_order_acquire); }

void Init() {
    if (gInited.exchange(true)) return;
    if (!EnvOn()) {
        gEnabled.store(false);
        x::runtime::LogI("WalkBin", "off (set XCAT_WALK_BIN=1 to enable)");
        return;
    }
    gEnabled.store(true);
    OpenLog();
    LogLine("init enabled=1 (RO only; disable F5/human for clean sample)");
    if (x::runtime::main_thread::Ensure()) {
        x::runtime::main_thread::SetBinFrameTick(&BinFrameTick, nullptr);
    }
    x::runtime::LogI("WalkBin", "keypad_walk_bin ON → logs/keypad_walk_bin.log");
}

void Shutdown() {
    if (!gInited.exchange(false)) return;
    gEnabled.store(false);
    x::runtime::main_thread::SetBinFrameTick(nullptr, nullptr);
    EnsureLogCs();
    EnterCriticalSection(&gLogCs);
    if (gLog) {
        fclose(gLog);
        gLog = nullptr;
    }
    LeaveCriticalSection(&gLogCs);
}

}  // namespace x::features::ports::keypad_walk_bin
