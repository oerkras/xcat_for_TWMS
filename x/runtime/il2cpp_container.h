#pragma once
// IL2CPP 容器 / 数组防漂 SSOT（经典版 TWMS）。
//
// 1) System.Collections.Generic：
//    Dictionary`2 / List`1 / Queue`1 / Stack`1 / HashSet`1
//    → hash/明文 field_get_offset；常量仅 fallback。
//    Dict 勿把 freeCount 读成 0x2C（那是 _version）。
//
// 2) Il2CppArray 原生头 + Dictionary.Entry valuetype 槽：
//    x64 Unity ABI 常量（非 managed field meta）；集中命名，禁止各 port 再散落 0x18/0x20。

#include <cstddef>
#include <cstdint>

namespace x::runtime::il2cpp_container {

void Ensure();

// —— Dictionary`2 ——
size_t OffDictBuckets();    // fb 0x10
size_t OffDictEntries();    // fb 0x18
size_t OffDictCount();      // fb 0x20
size_t OffDictFreeList();   // fb 0x24
size_t OffDictFreeCount();  // fb 0x28
size_t OffDictVersion();    // fb 0x2C（诊断用）

// —— List`1 ——
size_t OffListItems();  // fb 0x10
size_t OffListSize();   // fb 0x18

// —— Queue`1 ——
size_t OffQueueArray();    // fb 0x10
size_t OffQueueHead();     // fb 0x18
size_t OffQueueTail();     // fb 0x1C
size_t OffQueueSize();     // fb 0x20
size_t OffQueueVersion();  // fb 0x24

// —— Stack`1 ——
size_t OffStackArray();    // fb 0x10
size_t OffStackSize();     // fb 0x18
size_t OffStackVersion();  // fb 0x1C

// —— HashSet`1 ——
size_t OffHashSetBuckets();    // fb 0x10
size_t OffHashSetSlots();      // fb 0x18
size_t OffHashSetCount();      // fb 0x20
size_t OffHashSetLastIndex();  // fb 0x24
size_t OffHashSetFreeList();   // fb 0x28
size_t OffHashSetVersion();    // fb 0x38

// —— Il2CppArray（原生头；不走 field_get_offset）——
size_t OffArrayMaxLength();  // 0x18
size_t OffArrayData();       // 0x20（首元素）

// —— Dictionary.Entry 槽（entries[] 内 valuetype；按 K/V 选型）——
size_t DictEntryStrideIntPtr();       // 0x18：int→T* / 常见对齐
size_t DictEntryStrideIntIntTight();  // 0x10：int→int 紧凑
size_t DictEntryStrideIntIntAlign();  // 0x18：int→int 对齐
size_t OffDictEntryHash();            // 0
size_t OffDictEntryNext();            // 4
size_t OffDictEntryKey();             // 8
size_t OffDictEntryValuePtr();        // 0x10（int→T*）
size_t OffDictEntryValueIntTight();   // 12
size_t OffDictEntryValueIntAlign();   // 16

// entries 为 Il2CppArray*；返回第 index 个 Entry 字节首址（越界由调用方控）。
uint8_t* DictEntryAt(void* entries, int index, size_t stride);

// 用存活实例 klass 再解析（开泛型 FindClass 失败时兜底）。
void RefineFromDictInstance(void* dict);
void RefineFromListInstance(void* list);
void RefineFromQueueInstance(void* queue);
void RefineFromStackInstance(void* stack);
void RefineFromHashSetInstance(void* set);

}  // namespace x::runtime::il2cpp_container
