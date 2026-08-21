#pragma once
// Classic TWMS shop_port — UIShopDialog ready + TalkToNpc + UI 买卖 / 飞镖充值。
// 字段防漂移：hash / 明文 → field_get_offset；dump 常量仅 fallback（见 EnsureShopFieldOffsets）。
// 卖出：UIShopDialog.SendSellRequestPacket @0x55B010（2026-08-04）
//   Create(67)+Encode1(1)+Encode2(pos)+Encode4(itemId)+Encode2(qty)
// 买入：UIShopDialog.SendBuyRequestPacket @0x55A4C0（2026-08-04）
//   Create(67)+Encode1(0)+Encode2(buyIdx)+Encode4(itemId)+Encode2(qty)
// 飞镖：UIShopDialog.SendRechargeRequestPacket @0x55B610（2026-08-04）
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
// 须 Unity 存活且 GameObject.activeSelf；getGo/getActive 失败当作未开（勿保守当开着）。
bool ShopReady(bool& outReady);

// 关店：buttonExit.Press → SetRet → Close；仍开着则 GameObject.SetActive(false)。
bool CloseShop();

struct NpcLocate {
    bool ok = false;
    int oid = 0;
    int tpl = 0;
    float dist = 0.f;
    float x = 0.f;
    float y = 0.f;
    float playerX = 0.f;
    float playerY = 0.f;
};

// 只定位、不 TalkToNpc。preferTemplateId>0 全图找该 tpl。
bool LocateNpcByTemplate(int templateId, NpcLocate& out);

// 找近距 NPC 并调 UserLocal.TalkToNpc（C→S UserSelectNpc=64）。
// 成功仅表示已发起对话；是否弹出商店 / 菜单由服端与脚本决定。
// maxDist：世界坐标欧氏距离上限；<=0 用默认 220（无 tpl 时）。
// preferTemplateId：>0 时全图只对该 NpcData.Id Talk；找不到不退近距乱点。
// inRangeOnly：true 时 dist>maxDist 不发包（船/转职 NPC 远距 Talk 会被服端踢）。
bool TryTalkNearestNpc(float maxDist = 0.f, int preferTemplateId = 0, bool inRangeOnly = false);

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

// 店内是否有该商品；outPrice=Item.Price（单价，必要时回退 UintPrice）；未命中 outInShop=false。
bool QueryShopBuyOffer(int itemId, bool& outInShop, int& outPrice);

// 诊断：开店后把买栏 (+0x178/+0x180) 条数与货架 ID/价打到 Shop 日志。
// focusItemId>0 时额外标是否命中该 ID。失败（无店）返回 false。
bool LogBuyShelfSnapshot(int focusItemId = 0);

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

// 飞镖 Charge：开店后扫消耗栏卖栏投影，对 207xxxx 未满格写卖栏选中并调
// SendRechargeRequestPacket。每次最多 1 格；SHOP_BUSY/LIST_STALE 时 outErr 对应码。
// 开始一键/行程 Charge 前调用 ResetChargeSession（清卡住格拉黑）。
void ResetChargeSession();
bool RechargeShurikensInOpenShop(int& outCharged, int& outSkippedNoMeso, int& outSkippedOther,
                                 std::string& outErr);

}  // namespace x::features::ports::shop
