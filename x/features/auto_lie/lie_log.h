#pragma once

// 测谎日志分流：同一行既进 x.jsonl（结构化、可检索），也进 logs\auto_lie.log。
//
// 为什么要单开频道：x.jsonl 是全模块共用的，combat / mobscan 那些高频通道能把一卷
// 524KiB 在 3.5 分钟内写满。BIN aa29bc 实测——11 卷加起来只剩最近 39 分钟，而那份包里
// 16 次真题跨了 13 小时，现场全被冲掉，只有不轮转的 lie_events 留下了映射证据，
// 于是「missed=1 是哪一次、为什么」查不出来。
//
// 测谎一次只写几十行，独占频道后同样 512KiB×24 代能留住数月。
namespace x::features::auto_lie::lie_log {

// tag 用各模块原有的（AutoLie / AutoLieMouse / AutoLiePort …），写进行首便于区分来源。
void Line(const char* tag, const char* body);

}  // namespace x::features::auto_lie::lie_log
