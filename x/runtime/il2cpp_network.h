#pragma once
// NetworkManager Facade + Session 实例字段防漂 SSOT（经典版 TWMS）。
// hash → field_get_offset；dump 常量仅 fallback。
// 勿抄 CMS：TW Session RecvList@0x58 / State@0x60（CMS 是 0x50/0x58）。

#include <cstddef>

namespace x::runtime::il2cpp_network {

// 字段偏移解析。内部保证：未完成时若在非泵线程会 InvokeAndWait 到 MainPump
//（BIN 10:11 KickSniff worker 直调 → GC「Collecting from unknown thread」）。
void Ensure();

// LOGIN workers 启动前显式预热（与 Ensure 等价；语义上标「冷启动闸」）。
void WarmForLoginWorkers();

// —— NetworkManager Facade（TypeDef 13772 / hash c053d224…）——
size_t OffNmSession();        // fb 0x10 Session*
size_t OffNmSessionState();   // fb 0x18 SessionState（enum/I32）
size_t OffNmPacketQueue();    // fb 0x28 Queue<InPacket>*
size_t OffNmOpcodeHashSet();  // fb 0x48 HashSet<ushort>*（TW 独有）

// —— Session（TypeDef 13797 / hash b7c1f712…）——
size_t OffSessionSeqSend();       // fb 0x18 uint
size_t OffSessionClosed();        // fb 0x20 bool
size_t OffSessionPendingError();  // fb 0x40 int
size_t OffSessionRecvList();      // fb 0x58 List<InPacket>*
size_t OffSessionState();         // fb 0x60 SessionState backing

}  // namespace x::runtime::il2cpp_network
