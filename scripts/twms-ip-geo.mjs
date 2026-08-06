/**
 * TWMS IP 归属地查询（产品=经典版）。
 * 多源：ip-api(zh-CN) + 太平洋 pconline（国内常更准）+ ip9 兜底。
 * 对照旁路仓仅为「队列+缓存」模式；数据源已升级，不套用枫星协议。
 */

const kIpGeoCacheVer = 2; // 升版本使旧 ip9 单源脏缓存失效
const kIpGeoOkTtlMs = 12 * 60 * 60 * 1000; // 12h（库不准时少卡脏数据）
const kIpGeoErrTtlMs = 60 * 1000;
const kIpGeoMinIntervalMs = 1200;
const kIpGeoProvider = "ip-api+pconline/ip9";

/**
 * @param {{
 *   logWarn?: (msg: string) => void,
 *   userAgent?: string,
 * }} [opts]
 */
export function createIpGeo(opts = {}) {
  const logWarn = opts.logWarn || ((msg) => console.warn(msg));
  const userAgent = opts.userAgent || "xcat-twms-ops/1.0";

  /** @type {Map<string, { text: string, at: number, ok: boolean, ver: number, source?: string }>} */
  const ipGeoCache = new Map();
  /** @type {string[]} */
  const ipGeoQueue = [];
  /** @type {Set<string>} */
  const ipGeoQueued = new Set();
  let ipGeoPumpRunning = false;
  let ipGeoLastAt = 0;

  /** @type {() => Iterable<{ ip: string, geo?: string, geoStatus?: string }>} */
  let listClients = () => [];

  function bindClientLister(fn) {
    listClients = fn;
  }

  function isLoopback(ip) {
    return ip === "127.0.0.1" || ip === "::1" || ip === "localhost";
  }

  function isPrivateOrLocalIp(ip) {
    if (!ip || ip === "unknown") return true;
    if (isLoopback(ip)) return true;
    if (ip.startsWith("10.") || ip.startsWith("192.168.") || ip.startsWith("169.254.")) return true;
    const m = /^172\.(\d+)\./.exec(ip);
    if (m) {
      const n = Number(m[1]);
      if (n >= 16 && n <= 31) return true;
    }
    return false;
  }

  function dedupJoin(parts) {
    const cleaned = parts.map((s) => String(s || "").trim()).filter(Boolean);
    const dedup = [];
    for (const p of cleaned) {
      if (!dedup.length || dedup[dedup.length - 1] !== p) dedup.push(p);
    }
    return dedup.join(" ");
  }

  function looksChina(...parts) {
    const s = parts.join(" ");
    return /中国|CN|China/i.test(s);
  }

  async function fetchText(url, { timeoutMs = 2500, gbk = false } = {}) {
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), timeoutMs);
    try {
      const res = await fetch(url, {
        signal: ac.signal,
        headers: {
          Accept: "application/json,text/plain,*/*",
          "User-Agent": userAgent,
        },
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      if (gbk) {
        const buf = Buffer.from(await res.arrayBuffer());
        try {
          return new TextDecoder("gbk").decode(buf);
        } catch {
          return buf.toString("utf8");
        }
      }
      return await res.text();
    } finally {
      clearTimeout(timer);
    }
  }

  async function lookupIpApi(ip) {
    // 免费档仅 HTTP；lang=zh-CN 便于运维台直读。
    const url =
      `http://ip-api.com/json/${encodeURIComponent(ip)}` +
      `?lang=zh-CN&fields=status,message,country,regionName,city,district,isp,org,as,query`;
    const raw = await fetchText(url);
    const body = JSON.parse(raw);
    if (body?.status !== "success") throw new Error(body?.message || "ip-api fail");
    const text = dedupJoin([body.country, body.regionName, body.city, body.district, body.isp]);
    if (!text) throw new Error("ip-api empty");
    return {
      text,
      source: "ip-api",
      china: looksChina(body.country, body.countryCode || ""),
      score: [body.city, body.regionName, body.district].filter(Boolean).length,
    };
  }

  async function lookupPconline(ip) {
    const url = `https://whois.pconline.com.cn/ipJson.jsp?ip=${encodeURIComponent(ip)}&json=true`;
    const raw = (await fetchText(url, { gbk: true })).replace(/^\uFEFF/, "").trim();
    // 偶发包一层空白/回调噪声，取首个 {...}
    const m = raw.match(/\{[\s\S]*\}/);
    if (!m) throw new Error("pconline no json");
    const body = JSON.parse(m[0]);
    if (body?.err && String(body.err).toLowerCase() === "noprovince") {
      throw new Error("pconline noprovince");
    }
    const addr = String(body.addr || "").trim();
    const text =
      dedupJoin([body.pro, body.city, body.region, addr && !addr.includes(body.pro) ? addr : ""]) ||
      addr;
    // addr 常已含「省 市 ISP」，优先用它（国内宽带/专线更贴近）。
    const prefer = addr || text;
    if (!prefer) throw new Error("pconline empty");
    return {
      text: prefer.replace(/\s+/g, " ").trim(),
      source: "pconline",
      china: true,
      score: prefer.length > 4 ? 3 : 1,
    };
  }

  async function lookupIp9(ip) {
    const url = `https://ip9.com.cn/get?ip=${encodeURIComponent(ip)}`;
    const raw = await fetchText(url);
    const body = JSON.parse(raw);
    if (body?.ret !== 200 || !body?.data) throw new Error(`ip9 ret=${body?.ret ?? "?"}`);
    const d = body.data;
    const text = dedupJoin([d.country, d.prov, d.city, d.area, d.isp]);
    if (!text || text === "未知") throw new Error("ip9 empty");
    return {
      text,
      source: "ip9",
      china: looksChina(d.country, d.country_code),
      score: [d.city, d.area, d.prov].filter(Boolean).length,
    };
  }

  /**
   * 国内优先太平洋；海外/失败再 ip-api；最后 ip9。
   * 两源并行，减少串行等待。
   */
  async function lookupIpGeoBest(ip) {
    const settled = await Promise.allSettled([lookupIpApi(ip), lookupPconline(ip)]);
    /** @type {Array<{ text: string, source: string, china: boolean, score: number }>} */
    const ok = [];
    for (const s of settled) {
      if (s.status === "fulfilled" && s.value?.text) ok.push(s.value);
    }

    const pc = ok.find((x) => x.source === "pconline");
    const api = ok.find((x) => x.source === "ip-api");

    // 国内：太平洋「省市+运营商」通常比免费国际库更贴近宽带出口。
    if (pc && (api?.china || !api)) return pc;
    if (api) return api;
    if (pc) return pc;

    return await lookupIp9(ip);
  }

  function geoCacheFresh(cached) {
    if (!cached || !cached.text || cached.ver !== kIpGeoCacheVer) return false;
    const ttl = cached.ok ? kIpGeoOkTtlMs : kIpGeoErrTtlMs;
    return Date.now() - cached.at < ttl;
  }

  function applyGeoCacheToIp(ip, cached) {
    if (!cached) return;
    for (const row of listClients()) {
      if (row.ip !== ip) continue;
      if (cached.ok && cached.text) {
        row.geo = cached.text;
        row.geoStatus = "ok";
      } else if (cached.text) {
        row.geo = cached.text;
        row.geoStatus = "error";
      }
    }
  }

  async function runGeoLookup(ip) {
    const cached = ipGeoCache.get(ip);
    if (geoCacheFresh(cached) && cached.ok) {
      applyGeoCacheToIp(ip, cached);
      return;
    }
    for (const row of listClients()) {
      if (row.ip === ip && row.geoStatus !== "ok") row.geoStatus = "pending";
    }
    try {
      const hit = await lookupIpGeoBest(ip);
      ipGeoCache.set(ip, {
        text: hit.text,
        at: Date.now(),
        ok: true,
        ver: kIpGeoCacheVer,
        source: hit.source,
      });
      applyGeoCacheToIp(ip, ipGeoCache.get(ip));
    } catch (err) {
      const text = "查询失败";
      ipGeoCache.set(ip, {
        text,
        at: Date.now(),
        ok: false,
        ver: kIpGeoCacheVer,
      });
      for (const row of listClients()) {
        if (row.ip !== ip) continue;
        if (row.geoStatus === "pending" || !row.geo || row.geoStatus === "error") {
          row.geo = text;
          row.geoStatus = "error";
        }
      }
      logWarn(`ip geo failed ip=${ip} ${err.message || err}`);
    }
  }

  async function pumpGeoQueue() {
    if (ipGeoPumpRunning) return;
    ipGeoPumpRunning = true;
    try {
      while (ipGeoQueue.length) {
        const wait = Math.max(0, kIpGeoMinIntervalMs - (Date.now() - ipGeoLastAt));
        if (wait > 0) await new Promise((r) => setTimeout(r, wait));
        const ip = ipGeoQueue.shift();
        if (!ip) continue;
        ipGeoQueued.delete(ip);
        ipGeoLastAt = Date.now();
        await runGeoLookup(ip);
      }
    } finally {
      ipGeoPumpRunning = false;
      if (ipGeoQueue.length) pumpGeoQueue();
    }
  }

  function scheduleGeoLookup(ip, { force = false } = {}) {
    if (!ip || isPrivateOrLocalIp(ip)) return;
    const cached = ipGeoCache.get(ip);
    if (!force && geoCacheFresh(cached)) {
      applyGeoCacheToIp(ip, cached);
      if (cached.ok) return;
    }
    if (force) ipGeoCache.delete(ip);
    if (ipGeoQueued.has(ip)) return;
    for (const row of listClients()) {
      if (row.ip === ip && row.geoStatus !== "ok") row.geoStatus = "pending";
    }
    ipGeoQueued.add(ip);
    ipGeoQueue.push(ip);
    pumpGeoQueue();
  }

  /** 运维切换数据源后：清内存缓存并重查当前在线客户端。 */
  function invalidateAllAndRefresh() {
    ipGeoCache.clear();
    for (const row of listClients()) {
      if (!row?.ip || isPrivateOrLocalIp(row.ip)) continue;
      row.geo = "";
      row.geoStatus = "pending";
      scheduleGeoLookup(row.ip, { force: true });
    }
  }

  function markPrivate(row) {
    if (!row) return;
    row.geo = "内网/私网";
    row.geoStatus = "private";
  }

  function displayGeo(row) {
    if (!row) return "";
    if (row.geo) return row.geo;
    if (row.geoStatus === "pending") return "查询中…";
    return "";
  }

  return {
    provider: kIpGeoProvider,
    isPrivateOrLocalIp,
    isLoopback,
    bindClientLister,
    scheduleGeoLookup,
    invalidateAllAndRefresh,
    markPrivate,
    displayGeo,
  };
}
