#pragma once
// Classic TWMS shop_port — UIShopDialog ready + TalkToNpc + UI 买卖 / 飞镖充值。
// 字段防漂移：hash / 明文 → field_get_offset；dump 常量仅 fallback（见 EnsureShopFieldOffsets）。
// 卖出：UIShopDialog.SendSellRequestPacket @0x54DC40（2026-08-04）
//   Create(67)+Encode1(1)+Encode2(pos)+Encode4(itemId)+Encode2(qty)
// 买入：UIShopDialog.SendBuyRequestPacket @0x54CFF0（2026-08-04）
//   Create(67)+Encode1(0)+Encode2(buyIdx)+Encode4(itemId)+Encode2(qty)
// 飞镖：UIShopDialog.SendRechargeRequestPacket @0x54E1B0（2026-08-04）
//   Create(67)+Encode1(2)+Encode2(pos)；选中卖栏 _sellSelectedIndex
// 禁止 Session.Send 旁路 HashSet（会本地踢线）。

#include <string>
#include <cstdint>

namespace x::features::ports::shop {

struct BagItem {
    int  pos = 0;       // nPOS（发包用，与 Item.Position 一致）
    int  itemId = 0;
    int  count = 1;
    int  invType = 0;   // 1=equip 2=consume 4=etc
    char name[64]{};
    bool sellable = false;
};

bool EnsureBound();

// 当前是否有 UIShopDialog 实例（用户已开 NPC 店）。
bool ShopReady(bool& outReady);

// 关店：buttonExit.Press → SetRet → Close；仍开着则 GameObject.SetActive(false)。
// ShopReady 看 Unity 存活 + activeSelf（FindAll 残留不算开店）。
bool CloseShop();

// 找近距 NPC 并调 UserLocal.TalkToNpc（C→S UserSelectNpc=64）。
// 成功仅表示已发起对话；是否弹出商店 / 菜单由服端与脚本决定。
// maxDist：世界坐标欧氏距离上限；<=0 用默认 220。
// preferTemplateId：>0 时优先匹配 NpcData.Id（grocery 种子 npc_id）；找不到再退近距。
bool TryTalkNearestNpc(float maxDist = 0.f, int preferTemplateId = 0);

// 备用开对话：UserLocal.OnFuncKey(BasicAction=5, NpcTalk=54) Down+Up。
bool TryNpcTalkFuncKey();

// 若已弹出 UIUtilDialogEx：Say/YesNo 点确定推进；AskMenu(List) 按文案选「商店/交易/…」并确认。
// 成功仅表示已点 UI；是否开出 UIShopDialog 仍看 ShopReady。
bool TryConfirmShopScriptMenu();

// 扫装备栏(invType=1)或其它栏(invType=4)。names 用离线 catalog 填。
bool ScanBag(bool equipBag, BagItem* items, int maxItems, int& outCount);

// 开店后快照当前 TAB 的 _sellItemList（切 TAB + CmpSellItem）。
// 任务道具等不进卖栏投影；sellbag 建队时用此表跳过，避免 LIST_STALE 空耗。
// outItemIds 可空；成功时 outListN=投影条数（可为 0）。
// outTabSwitched：本拍刚切 TAB（列表可能尚未刷新；调用方宜短等再拍）。
bool SnapshotShopSellList(int invType, int* outItemIds, int maxOut, int& outCount, int& outListN,
                          bool* outTabSwitched = nullptr);

// 卖一件：需 ShopReady。成功仅表示已发包；对账由调用方看槽位变化。
bool SellItem(int invType, int pos, int itemId, int count, std::string& outErr);

// 买一件：需 ShopReady；按 ItemId 匹配买栏（+0x178/+0x180）后走 UI 发包。
// 成功仅表示已发包；数量可能被 MaxSlot 截断，实发写入 outBought（可空）。
// 失败码：NO_SHOP / SHOP_BUSY / LIST_MISS / NO_MESO / BAD_ARGS / ...
bool BuyItem(int itemId, int count, std::string& outErr, int* outBought = nullptr);

// 店内是否有该商品；outPrice=Item.Price（单价）；未命中 outInShop=false。
bool QueryShopBuyOffer(int itemId, bool& outInShop, int& outPrice);

// 按 itemId 查该栏是否仍有货（确认用）。invType: 1/2/4。
bool QueryItemPresent(int invType, int itemId, bool& outPresent, int& outCount);

// 背包占用：used=非空槽数，cap=List._size（含空槽；实机作容量候选）。
bool QueryBagUsage(bool equipBag, int& outUsed, int& outCap);

// CharacterStat.Money / 背包 ItemSlots：SSOT = x::ui::player（hash→field_get_offset）。
int64_t QueryMeso();

// 离线商店种子（dataservice/grocery_shop_npc.tsv）+ travel hops 就近开店。
// 产品语义：寻店只为「能卖」；补给项店内有则买、无则跳过（不按货架选型）。
// outNpcId/outShopId 可空；成功至少写出 outMapName（9 位 mapId 字符串）。
bool ResolveShopNpcForSell(std::string& outNpcId, std::string& outShopId, std::string& outMapName,
                            int& outMapId, const char* excludeMapName = nullptr);
// 兼容旧名：与 ForSell 相同（忽略 preferredItemCode）。
bool ResolveShopNpcForSupply(const char* preferredItemCode, std::string& outNpcId,
                              std::string& outShopId, std::string& outMapName, int& outMapId,
                              const char* excludeMapName = nullptr);

// 飞镖 Charge：开店后扫消耗栏卖栏投影，对 207xxxx 且 Quantity<MaxSlotCount、UintPrice>0
// 的飞镖写卖栏选中并调 SendRechargeRequestPacket。每次最多充 1 格；SHOP_BUSY/LIST_STALE
// 时 outErr 对应码、调用方应重试；无候选时 outCharged=0 且返回 true。
bool RechargeShurikensInOpenShop(int& outCharged, int& outSkippedNoMeso, int& outSkippedOther,
                                 std::string& outErr);

}  // namespace x::features::ports::shop
