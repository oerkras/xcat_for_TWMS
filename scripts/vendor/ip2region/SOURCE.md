# ip2region（离线 IP 库 · 经典版运维台用）

- 查询器：官方 npm `ip2region.js@3.1.8`（`index.js` / `searcher.js` / `util.js`）
- 数据：`ip2region_v4.xdb`（IPv4），缺失时由 `twms-ip-geo.mjs` 从 jsDelivr 拉取缓存到本目录
- 上游：https://github.com/lionsoul2014/ip2region
- 说明：免费 xdb 城市粒度多为地级市；区县精度仍依赖在线源（如 ip9 `area`）择优合并
