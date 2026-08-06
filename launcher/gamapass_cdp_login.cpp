#include "gamapass_cdp_login.h"

#include "chromium_cdp.h"
#include "msc_launch.h"
#include "ott_ticket_fetch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace msc::launcher {
namespace {

constexpr int kCdpPort = msc::cdp::kDefaultRemoteDebugPort;
constexpr int kSlotMin = 1;
constexpr int kSlotMax = 16;

constexpr wchar_t kGalaxyLogin[] =
    L"https://galaxy.games.gamania.com/webapi/view/login/mstc"
    L"?redirect_url=https://maplestoryclassic.beanfun.com/Main";

constexpr wchar_t kClassicMain[] = L"https://maplestoryclassic.beanfun.com/Main";

HttpLoginResult Fail(HttpLoginError e, const std::string& msg) {
    HttpLoginResult r;
    r.ok = false;
    r.error = e;
    r.message = msg;
    return r;
}

void Log(const HttpLoginLogFn& log, const std::wstring& s) {
    if (log) log(s);
}

std::wstring ExeDirLocal() {
    wchar_t buf[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return {};
    std::wstring p(buf);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : p.substr(0, slash);
}

std::wstring NickSlotPath() { return ExeDirLocal() + L"\\gamapass_nick_slot.txt"; }
std::wstring AccountSlotPath() { return ExeDirLocal() + L"\\gamapass_account_slot.txt"; }

std::string NarrowPath(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

int ClampSlot(int slot) {
    if (slot < kSlotMin) return kSlotMin;
    if (slot > kSlotMax) return kSlotMax;
    return slot;
}

int LoadSlotFile(const std::wstring& path) {
    std::ifstream f(NarrowPath(path), std::ios::binary);
    if (!f) return kSlotMin;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!raw.empty() &&
           (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ' || raw.back() == '\t'))
        raw.pop_back();
    if (raw.empty()) return kSlotMin;
    try {
        return ClampSlot(std::stoi(raw));
    } catch (...) {
        return kSlotMin;
    }
}

void SaveSlotFile(const std::wstring& path, int slot) {
    slot = ClampSlot(slot);
    std::ofstream f(NarrowPath(path), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << slot;
}

int gNickSlotCached = 0;     // 0=未加载
int gAccountSlotCached = 0;  // 0=未加载

int LoadNickSlotFromDisk() { return LoadSlotFile(NickSlotPath()); }
void SaveNickSlotToDisk(int slot) { SaveSlotFile(NickSlotPath(), slot); }
int LoadAccountSlotFromDisk() { return LoadSlotFile(AccountSlotPath()); }
void SaveAccountSlotToDisk(int slot) { SaveSlotFile(AccountSlotPath(), slot); }

std::wstring JsClickGamaPassProvider() {
    // 只触发一次原生 click：勿叠加 pointer/mouse 序列 + click()，否则官网 OAuth
    // 会当成连点两次，Galaxy 页常弹「登录阶段超时」而另一路仍跳到 select-account。
    return std::wstring(L"(function(){try{") +
           L"function T(el){return ((el&&(el.innerText||el.textContent||''))||'').replace(/\\s+/g,' ').trim();}"
           L"function prefer(el){"
           L"  if(!el)return null;"
           L"  var t=el.closest('a,button,[role=button]')||el;"
           L"  return t;"
           L"}"
           L"function fireOnce(el){if(!el)return false;"
           L"  var t=prefer(el); if(!t)return false;"
           L"  try{t.scrollIntoView({block:'center'});}catch(e){}"
           L"  try{t.click(); return true;}catch(e){return false;}}"
           L"var nodes=[].slice.call(document.querySelectorAll('a,button,[role=button],div,span,li'));"
           L"var exact=[];"
           L"for(var i=0;i<nodes.length;i++){"
           L"  var t=T(nodes[i]); var tl=t.toLowerCase();"
           L"  if(tl==='sign in with gama pass' || tl==='gama pass') exact.push(nodes[i]);"
           L"}"
           L"if(exact.length){"
           // 优先真实 button/a（面积更小的更像可点控件），避免点到外包一层再冒泡成二次提交。
           L"  exact.sort(function(a,b){"
           L"    function rank(el){var tag=(el.tagName||'').toLowerCase();"
           L"      if(tag==='button'||tag==='a')return 0;"
           L"      if((el.getAttribute('role')||'')==='button')return 1; return 2;}"
           L"    var ra=rank(prefer(a)||a), rb=rank(prefer(b)||b); if(ra!==rb)return ra-rb;"
           L"    var aa=(prefer(a)||a).getBoundingClientRect();"
           L"    var bb=(prefer(b)||b).getBoundingClientRect();"
           L"    return (aa.width*aa.height)-(bb.width*bb.height);"
           L"  });"
           L"  if(fireOnce(exact[0])) return 'click-gamapass|exact';"
           L"}"
           L"for(var i=0;i<nodes.length;i++){"
           L"  var t=T(nodes[i]); var tl=t.toLowerCase();"
           L"  if(t.length>80) continue;"
           L"  if(tl.indexOf('gama pass')>=0 || tl.indexOf('gamapass')>=0){"
           L"    if(fireOnce(nodes[i])) return 'click-gamapass|'+t.slice(0,40);"
           L"  }"
           L"}"
           L"return 'wait-gamapass';"
           L"}catch(e){return 'err:'+String(e);}})();";
}

std::wstring JsSelectAccount(int accountSlot) {
    // accounts.gamania.com/login/select-account：按自上而下序号点第 N 张账号卡。
    // 避开「使用其他帳號」；卡片中心一次 MouseEvent click（React 外层 div 需坐标点击）。
    // 刹车：父节点若含 ≥2 个邮箱绝不往上扩，避免两张卡被收成一块。
    accountSlot = ClampSlot(accountSlot);
    return std::wstring(L"(function(){try{") +
           L"var wantSlot=" + std::to_wstring(accountSlot) + L";"
           L"function T(el){return ((el&&(el.innerText||el.textContent||''))||'').replace(/\\s+/g,' ').trim();}"
           L"function mailCount(t){var m=(t||'').match(/[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}/gi); return m?m.length:0;}"
           L"function mailKey(t){var m=(t||'').match(/[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}/i);"
           L"  return m?m[0].toLowerCase():'';}"
           L"function bad(t){t=(t||'').toLowerCase();"
           L"  return t.indexOf('使用其他')>=0||t.indexOf('其他帳')>=0||t.indexOf('其它帳')>=0||"
           L"         t.indexOf('other account')>=0||t.indexOf('建立')>=0||t.indexOf('创建帳')>=0;}"
           L"function resolveCard(el){if(!el)return null;"
           L"  var t=el;"
           L"  for(var k=0;k<10&&t.parentElement;k++){"
           L"    var p=t.parentElement; var pt=T(p);"
           L"    if(!pt||pt.length>160||bad(pt)) break;"
           // 父块已含两张及以上邮箱 → 列表容器，停在当前单卡。
           L"    if(mailCount(pt)>=2) break;"
           L"    if(pt.indexOf('@')>=0) t=p; else break;"
           L"  }"
           L"  try{var c=t.closest('a,button,[role=button],li');"
           L"    if(c&&!bad(T(c))&&T(c).length<=160&&mailCount(T(c))<=1) t=c;}catch(e){}"
           L"  return t;}"
           L"function fireOnce(el){var t=resolveCard(el)||el; if(!t)return false;"
           L"  try{t.scrollIntoView({block:'center'});}catch(e){}"
           L"  var r=t.getBoundingClientRect();"
           L"  if(!(r.width>=12&&r.height>=12)) return false;"
           L"  var x=Math.floor(r.left+r.width/2), y=Math.floor(r.top+r.height/2);"
           L"  var top=document.elementFromPoint(x,y)||t;"
           L"  try{var up=top.closest('a,button,[role=button],li,div');"
           L"    if(up&&!bad(T(up))&&mailCount(T(up))===1&&T(up).length<=160) top=up;}catch(e){}"
           L"  try{"
           L"    top.dispatchEvent(new MouseEvent('click',{bubbles:true,cancelable:true,view:window,"
           L"      clientX:x,clientY:y,button:0,buttons:1}));"
           L"    return true;"
           L"  }catch(e){ try{top.click(); return true;}catch(e2){return false;} }}"
           L"var path=(location.pathname||'').toLowerCase();"
           L"var onSelect=path.indexOf('select-account')>=0||path.indexOf('/login/select')>=0;"
           L"if(!onSelect && (location.href||'').toLowerCase().indexOf('accounts.gamania.com')<0)"
           L"  return 'wait-select-account';"
           L"var all=[].slice.call(document.querySelectorAll('a,button,li,div,span,p,section,article'));"
           L"var raw=[];"
           L"for(var i=0;i<all.length;i++){"
           L"  var el=all[i]; var t=T(el); if(!t||t.length<3||t.length>120) continue;"
           L"  if(bad(t)) continue;"
           L"  if(t.indexOf('使用 Gama Pass')>=0||t.indexOf('使用 Gama')>=0) continue;"
           L"  var hasMail=t.indexOf('@')>=0;"
           L"  var hasId=/[a-z]{2,}[0-9]{2,}/i.test(t);"
           L"  if(!hasMail && !hasId) continue;"
           L"  var card=resolveCard(el)||el;"
           L"  var ct=T(card);"
           L"  if(bad(ct)) continue;"
           // 仍含多邮箱的块不当作单卡（列表壳）。
           L"  if(mailCount(ct)>=2) continue;"
           L"  var top=0,left=0,area=0;"
           L"  try{var rr=card.getBoundingClientRect(); top=rr.top; left=rr.left; area=rr.width*rr.height;}catch(e){}"
           L"  if(area<80) continue;"
           L"  var mk=mailKey(ct)||mailKey(t);"
           L"  var mc=mailCount(ct)||mailCount(t);"
           L"  var bonus=0;"
           L"  if(mc===1) bonus+=20;"
           L"  if(hasMail||ct.indexOf('@')>=0) bonus+=10;"
           L"  if(hasId) bonus+=10;"
           L"  if(mc===1&&hasId) bonus+=30;"
           L"  raw.push({el:card,t:ct||t,mk:mk,top:top,left:left,area:area,bonus:bonus,mc:mc});"
           L"}"
           L"if(!raw.length){"
           L"  var txt=(document.body&&document.body.innerText||'').slice(0,200);"
           L"  return 'wait-select-account|'+txt.replace(/\\s+/g,' ').slice(0,60);"
           L"}"
           // 按邮箱去重；contains 合并时优先保留单邮箱、更完整的那张（勿吞并成列表壳）。
           L"var uniq=[];"
           L"for(var i=0;i<raw.length;i++){"
           L"  var h=raw[i]; var idx=-1;"
           L"  for(var j=0;j<uniq.length;j++){"
           L"    var u=uniq[j];"
           L"    if((h.mk&&u.mk&&h.mk===u.mk)||h.el===u.el||"
           L"       (h.el.contains&&u.el.contains&&(h.el.contains(u.el)||u.el.contains(h.el)))){ idx=j; break; }"
           L"  }"
           L"  if(idx<0){ uniq.push(h); continue; }"
           L"  var cur=uniq[idx];"
           L"  var preferH=false;"
           L"  if((h.mc===1)&&(cur.mc!==1)) preferH=true;"
           L"  else if((h.mc!==1)&&(cur.mc===1)) preferH=false;"
           L"  else if(h.bonus>cur.bonus||(h.bonus===cur.bonus&&h.area>cur.area)) preferH=true;"
           L"  if(preferH) uniq[idx]=h;"
           L"}"
           L"uniq.sort(function(a,b){return (a.top-b.top)||(a.left-b.left);});"
           L"var idx=wantSlot-1; var clamped=0;"
           L"if(idx<0){ idx=0; clamped=1; }"
           L"if(idx>=uniq.length){ idx=uniq.length-1; clamped=1; }"
           L"var pick=uniq[idx];"
           L"var tag='slot'+wantSlot+'|use'+(idx+1)+'|goods'+uniq.length+(clamped?'|clamped':'');"
           L"if(!fireOnce(pick.el)) return 'wait-select-account|click-fail|'+tag;"
           L"return 'select-account|'+tag+'|'+pick.t.slice(0,50);"
           L"}catch(e){return 'err:'+String(e);}})();";
}

std::wstring JsSelectGameNick(int nickSlot) {
    // SelectGameAccount：①按 1-based 槽勾选昵称 ②仅在目标已勾选后点「繼續」
    // 跳过「建立遊戲暱稱」；控件只 click 一次。
    nickSlot = ClampSlot(nickSlot);
    return std::wstring(L"(function(){try{") +
           L"var wantSlot=" + std::to_wstring(nickSlot) + L";"
           L"function T(el){return ((el&&(el.innerText||el.textContent||el.value||''))||'').replace(/\\s+/g,' ').trim();}"
           L"function badNick(t){t=(t||''); return t.indexOf('建立')>=0||t.indexOf('创建')>=0;}"
           L"function fireOnce(el){if(!el)return false;"
           L"  try{el.scrollIntoView({block:'center'});}catch(e){}"
           L"  try{el.click(); return true;}catch(e){return false;}}"
           L"function findContinue(){"
           L"  var btns=[].slice.call(document.querySelectorAll('button,input[type=submit],a,[role=button]'));"
           L"  for(var i=0;i<btns.length;i++){"
           L"    var t=T(btns[i]);"
           L"    if(t==='繼續'||t==='继续'||t.toLowerCase()==='continue') return btns[i];"
           L"  }"
           L"  return null;"
           L"}"
           L"if(document.readyState!=='complete' && document.readyState!=='interactive')"
           L"  return 'wait-nick-load';"
           L"var href=(location.href||'').toLowerCase();"
           L"var onNick=href.indexOf('selectgameaccount')>=0;"
           L"var radios=[].slice.call(document.querySelectorAll('input[type=radio]'));"
           L"var goods=[];"
           L"for(var i=0;i<radios.length;i++){"
           L"  var lab=radios[i].closest('label')||radios[i].parentElement;"
           L"  if(badNick(T(lab))) continue;"
           L"  goods.push(radios[i]);"
           L"}"
           L"var pick=null;"
           L"var usedSlot=wantSlot;"
           L"var clamped=0;"
           L"if(goods.length){"
           L"  var idx=wantSlot-1;"
           L"  if(idx<0){ idx=0; clamped=1; }"
           L"  if(idx>=goods.length){ idx=goods.length-1; clamped=1; }"
           L"  usedSlot=idx+1;"
           L"  pick=goods[idx];"
           L"} else if(radios.length){ pick=radios[0]; usedSlot=1; clamped=1; }"
           L"var tag='slot'+wantSlot+'|use'+usedSlot+'|goods'+goods.length+(clamped?'|clamped':'');"
           L"if(pick && !pick.checked){"
           L"  try{pick.checked=true;}catch(e){}"
           L"  try{pick.dispatchEvent(new Event('input',{bubbles:true}));}catch(e){}"
           L"  try{pick.dispatchEvent(new Event('change',{bubbles:true}));}catch(e){}"
           L"  var lab2=pick.closest('label')||pick.parentElement;"
           L"  if(lab2) fireOnce(lab2); else fireOnce(pick);"
           L"  return 'nick-radio-selected|'+tag+'|'+(T(lab2)||'ok').slice(0,40);"
           L"}"
           L"var cont=findContinue();"
           L"if(cont && pick && pick.checked){"
           L"  if(cont.disabled) return 'wait-nick-btn';"
           L"  try{cont.disabled=false; cont.removeAttribute('disabled');}catch(e){}"
           L"  if(!fireOnce(cont)) return 'wait-nick-btn|click-fail';"
           L"  return 'nick-continue|'+tag;"
           L"}"
           L"if(!radios.length){"
           L"  var rows=[].slice.call(document.querySelectorAll('label,li,div[role=radio]'));"
           L"  var goodRows=[];"
           L"  for(var i=0;i<rows.length;i++){"
           L"    var t=T(rows[i]); if(!t||t.length<1||t.length>40) continue;"
           L"    if(badNick(t)) continue;"
           L"    if(t.indexOf('選擇')>=0||t.indexOf('选择')>=0||t==='登入') continue;"
           L"    goodRows.push(rows[i]);"
           L"  }"
           L"  if(goodRows.length){"
           L"    var ridx=wantSlot-1;"
           L"    var rclamp=0;"
           L"    if(ridx<0){ ridx=0; rclamp=1; }"
           L"    if(ridx>=goodRows.length){ ridx=goodRows.length-1; rclamp=1; }"
           L"    var row=goodRows[ridx];"
           L"    var rtag='slot'+wantSlot+'|use'+(ridx+1)+'|goods'+goodRows.length+(rclamp?'|clamped':'');"
           L"    if(fireOnce(row)) return 'nick-row|'+rtag+'|'+T(row).slice(0,40);"
           L"  }"
           L"}"
           L"return onNick?'wait-nick':'no-nick';"
           L"}catch(e){return 'err:'+String(e);}})();";
}

// 页面未就绪时不点：抗网络卡顿 / 慢渲染
std::wstring JsDomStageReady() {
    // 选号：有 radio 且未勾选 → ready-nick-radio；已勾选且「繼續」可点 → ready-nick-continue
    return std::wstring(L"(function(){try{") +
           L"var rs=document.readyState||'';"
           L"if(rs!=='complete' && rs!=='interactive') return 'wait-load|'+rs;"
           L"var h=(location.href||'').toLowerCase();"
           L"function hasText(s){try{return (document.body&&document.body.innerText||'').indexOf(s)>=0;}catch(e){return false;}}"
           L"function findContinue(){"
           L"  var btns=[].slice.call(document.querySelectorAll('button,input[type=submit],a,[role=button]'));"
           L"  for(var i=0;i<btns.length;i++){"
           L"    var t=((btns[i].innerText||btns[i].textContent||btns[i].value||'')+'').replace(/\\s+/g,' ').trim();"
           L"    if(t==='繼續'||t==='继续'||t.toLowerCase()==='continue') return btns[i];"
           L"  } return null;}"
           L"if(h.indexOf('selectgameaccount')>=0){"
           L"  var radios=[].slice.call(document.querySelectorAll('input[type=radio]'));"
           L"  if(!radios.length) return 'wait-nick-dom';"
           L"  var checked=radios.filter(function(r){return r.checked;});"
           L"  if(!checked.length) return 'ready-nick-radio';"
           L"  var cont=findContinue();"
           L"  if(!cont) return 'wait-nick-btn';"
           L"  if(cont.disabled) return 'wait-nick-btn';"
           L"  return 'ready-nick-continue';"
           L"}"
           L"if(h.indexOf('select-account')>=0){"
           L"  return (hasText('@')||document.querySelectorAll('a,button,li,div').length>8)?'ready-acc':'wait-acc-dom';"
           L"}"
           // 仅路径 /login 才是完整登录表；oauth2/authorize?prompt=login 不算
           L"if(h.indexOf('accounts.gamania.com')>=0){"
           L"  var path=(location.pathname||'').toLowerCase();"
           L"  while(path.length>1&&path.slice(-1)==='/') path=path.slice(0,-1);"
           L"  if(path==='/login') return 'need-login';"
           L"  if(path.indexOf('/oauth')>=0) return 'wait-oauth';"
           L"}"
           L"if(h.indexOf('galaxy.games.gamania.com')>=0 && h.indexOf('access_token=')<0){"
           L"  var t=(document.body&&document.body.innerText||'').toLowerCase();"
           L"  if(t.indexOf('gama pass')>=0||t.indexOf('gamapass')>=0) return 'ready-gp';"
           L"  return 'wait-gp-dom';"
           L"}"
           L"return 'ready-other';"
           L"}catch(e){return 'wait-err';}})();";
}

std::wstring ToLower(std::wstring u) {
    for (auto& c : u)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return u;
}

std::wstring ExtractQueryValueCi(const std::wstring& url, const wchar_t* key) {
    if (!key || !*key) return {};
    const std::wstring lower = ToLower(url);
    const std::wstring needle = ToLower(key) + L"=";
    size_t p = lower.find(needle);
    if (p == std::wstring::npos) return {};
    p += needle.size();
    std::wstring v;
    while (p < url.size() && url[p] != L'&' && url[p] != L'#' && url[p] != L' ') {
        v.push_back(url[p++]);
    }
    return v;
}

// Galaxy beanfun result 回跳用 access_token；官网换票入口认 WebToken=
std::wstring ExtractBrowserTicketToken(const std::wstring& url) {
    std::wstring t = ExtractQueryValueCi(url, L"WebToken");
    if (!t.empty()) return t;
    t = ExtractQueryValueCi(url, L"access_token");
    if (!t.empty()) return t;
    return ExtractQueryValueCi(url, L"OneTimeToken");
}

bool IsGalaxyInitSessionUrl(const std::wstring& url) {
    const std::wstring lower = ToLower(url);
    return lower.find(L"galaxy.games.gamania.com") != std::wstring::npos &&
           lower.find(L"/login/init/") != std::wstring::npos;
}

// accounts.gamania.com/login/select-account = 已登录选号卡
bool IsSelectAccountUrl(const std::wstring& lowerUrl) {
    return lowerUrl.find(L"select-account") != std::wstring::npos;
}

// 仅当「路径」恰好是 /login 才算完整账密页。
// 禁止误伤：oauth2/authorize?prompt=login、redirect_uri 里带 /login 的中间跳转。
bool IsGamaniaFullLoginUrl(const std::wstring& lowerUrl) {
    const std::wstring hostKey = L"accounts.gamania.com";
    const size_t host = lowerUrl.find(hostKey);
    if (host == std::wstring::npos) return false;
    if (IsSelectAccountUrl(lowerUrl)) return false;
    const size_t pathStart = lowerUrl.find(L'/', host + hostKey.size());
    if (pathStart == std::wstring::npos) return false;
    const size_t pathEnd = lowerUrl.find_first_of(L"?#", pathStart);
    std::wstring path = (pathEnd == std::wstring::npos)
                            ? lowerUrl.substr(pathStart)
                            : lowerUrl.substr(pathStart, pathEnd - pathStart);
    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    return path == L"/login";
}

bool IsOauthAuthorizeUrl(const std::wstring& lowerUrl) {
    return lowerUrl.find(L"accounts.gamania.com") != std::wstring::npos &&
           (lowerUrl.find(L"/oauth2/authorize") != std::wstring::npos ||
            lowerUrl.find(L"/oauth/authorize") != std::wstring::npos);
}

bool IsAccountsErrorUrl(const std::wstring& lowerUrl) {
    if (lowerUrl.find(L"accounts.gamania.com") == std::wstring::npos) return false;
    const std::wstring hostKey = L"accounts.gamania.com";
    const size_t host = lowerUrl.find(hostKey);
    const size_t pathStart = lowerUrl.find(L'/', host + hostKey.size());
    if (pathStart == std::wstring::npos) return false;
    const size_t pathEnd = lowerUrl.find_first_of(L"?#", pathStart);
    std::wstring path = (pathEnd == std::wstring::npos)
                            ? lowerUrl.substr(pathStart)
                            : lowerUrl.substr(pathStart, pathEnd - pathStart);
    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    return path == L"/error";
}

HttpLoginResult TryFetchFromOtt(const std::wstring& ott, const HttpLoginLogFn& log) {
    if (ott.empty() || ott.find(L"OTT:") == std::wstring::npos) {
        return Fail(HttpLoginError::OttMissing, "OTT 无效");
    }
    // init 会话 OTT 不能直接 GetOneTimeWebInfo
    if (ott.find(L":Login:") == std::wstring::npos) {
        return Fail(HttpLoginError::OttMissing, "OTT 非 Login 票");
    }
    Log(log, L"[gamapass-cdp] 捕获 OTT，换票中…");
    TicketFetchOptions fo;
    fo.ott = ott;
    fo.timeoutMs = 20000;
    auto fr = FetchGalaxyTicketFromOtt(fo);
    HttpLoginResult out;
    out.ok = fr.ok;
    out.error = fr.ok ? HttpLoginError::Ok : HttpLoginError::OttMissing;
    out.message = fr.message;
    out.ott = ott;
    out.ticket = std::move(fr.ticket);
    out.ticketFilled = fr.ok;
    return out;
}

// 从当前页 URL / HTML 里抠换票 OTT（地址栏常被清成裸 Main，但 DOM/跳转链仍可能残留）
std::wstring JsScrapeTicketOtt() {
    return std::wstring(L"(function(){try{") +
           L"function pick(s){if(!s)return '';"
           L"  var m=String(s).match(/OTT:[0-9]+:Login:[A-Za-z0-9_\\-+/=]+/);"
           L"  return m?m[0]:'';}"
           L"var h=location.href||'';"
           L"if(/[?&#]OTT=/i.test(h)||/OTT:944:Login:/.test(h)){"
           L"  var a=pick(h); if(a) return a;"
           L"  var q=(h.match(/[?&#]OTT=([^&#]+)/i)||[])[1]; if(q) return decodeURIComponent(q);"
           L"}"
           L"var html=(document.documentElement&&document.documentElement.innerHTML)||'';"
           L"var t=pick(html); if(t) return t;"
           L"var txt=(document.body&&document.body.innerText)||'';"
           L"return pick(txt);"
           L"}catch(e){return '';}})();";
}

std::string StripJsonStringValue(const std::string& cdpValueBlob) {
    // 从 "value":"...." 或整段里抽出字符串
    size_t p = cdpValueBlob.find("\"value\":\"");
    if (p == std::string::npos) return cdpValueBlob;
    p += 9;
    std::string out;
    while (p < cdpValueBlob.size()) {
        char c = cdpValueBlob[p++];
        if (c == '\\' && p < cdpValueBlob.size()) {
            out.push_back(cdpValueBlob[p++]);
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

}  // namespace

int GetGamaPassNickSlot() {
    if (gNickSlotCached == 0) gNickSlotCached = LoadNickSlotFromDisk();
    return gNickSlotCached;
}

void SetGamaPassNickSlot(int slot1Based) {
    gNickSlotCached = ClampSlot(slot1Based);
    SaveNickSlotToDisk(gNickSlotCached);
}

int GetGamaPassAccountSlot() {
    if (gAccountSlotCached == 0) gAccountSlotCached = LoadAccountSlotFromDisk();
    return gAccountSlotCached;
}

void SetGamaPassAccountSlot(int slot1Based) {
    gAccountSlotCached = ClampSlot(slot1Based);
    SaveAccountSlotToDisk(gAccountSlotCached);
}

HttpLoginResult HttpGamaPassCdpLoginToOtt(HttpLoginLogFn log, int timeoutMs) {
    const int nickSlot = GetGamaPassNickSlot();
    const int accountSlot = GetGamaPassAccountSlot();
    Log(log, L"[gamapass-cdp] 开始：默认浏览器点选（不调用 refresh/token，不改 LS）；账号=" +
                 std::to_wstring(accountSlot) + L" 昵称=" + std::to_wstring(nickSlot));

    msc::cdp::BrowserProfile profile;
    if (!msc::cdp::ResolvePreferredChromium(profile, [&](const std::wstring& s) { Log(log, s); })) {
        return Fail(HttpLoginError::BadInput,
                    "未找到 Chrome/Edge。请安装官方 Chrome、Edge 或 Chrome++ 后再试。");
    }

    msc::cdp::Session cdp;
    std::wstring failHint;
    if (!cdp.EnsureBrowser(profile, kCdpPort, [&](const std::wstring& s) { Log(log, s); },
                           &failHint)) {
        if (!failHint.empty()) {
            // message 走 UTF-8，与现有 Fail 文案一致
            int n = WideCharToMultiByte(CP_UTF8, 0, failHint.data(), (int)failHint.size(), nullptr, 0,
                                        nullptr, nullptr);
            std::string utf8(n, 0);
            if (n > 0) {
                WideCharToMultiByte(CP_UTF8, 0, failHint.data(), (int)failHint.size(), utf8.data(), n,
                                    nullptr, nullptr);
            }
            return Fail(HttpLoginError::Network, utf8.empty()
                                                     ? "无法连接浏览器调试口，请先关闭已打开的浏览器后重试"
                                                     : utf8);
        }
        return Fail(HttpLoginError::Network,
                    "无法连接浏览器调试口。若浏览器已在运行，请先自行关闭后重试"
                    "（程序不会自动结束你的浏览器）。");
    }
    Log(log, L"[gamapass-cdp] Browser=" + cdp.BrowserVersion());

    // 已在「成功路径中途」标签上则复用。下列不算 reusable（应重新开 Galaxy）：
    // - /login、/error、oauth 半截
    // - 空 Main（无 OTT）——守护重拉常见残留，会卡死 TokenWait（build37 实锤）
    auto urlReusableWithoutGalaxyNav = [](const std::wstring& lower) -> bool {
        if (lower.empty()) return false;
        if (IsGamaniaFullLoginUrl(lower) || IsAccountsErrorUrl(lower) || IsOauthAuthorizeUrl(lower))
            return false;
        if (IsSelectAccountUrl(lower)) return true;
        if (lower.find(L"selectgameaccount") != std::wstring::npos) return true;
        if (lower.find(L"access_token=") != std::wstring::npos) return true;
        if (lower.find(L"webtoken=") != std::wstring::npos) return true;
        // Main（含带 OTT）一律不复用：OTT 可能已兑过；守护重拉必须重走 Galaxy
        if (lower.find(L"maplestoryclassic.beanfun.com") != std::wstring::npos) return false;
        // Galaxy 登录入口可继续点 Gama Pass；其它 Galaxy 页不盲目复用
        if (lower.find(L"galaxy.games.gamania.com") != std::wstring::npos) {
            return lower.find(L"/login/") != std::wstring::npos ||
                   lower.find(L"access_token=") != std::wstring::npos;
        }
        return false;
    };

    std::wstring curUrl;
    bool skippedGalaxyNav = false;
    if (cdp.GetUrl(curUrl, nullptr) && urlReusableWithoutGalaxyNav(ToLower(curUrl))) {
        skippedGalaxyNav = true;
        Log(log, L"[gamapass-cdp] 复用当前标签，跳过 Galaxy Navigate：" + curUrl.substr(0, 160));
    } else if (!cdp.Navigate(kGalaxyLogin, [&](const std::wstring& s) { Log(log, s); })) {
        return Fail(HttpLoginError::Network, "无法打开 Galaxy 登录页");
    }
    // Galaxy 已占住主标签后，清掉启动/会话恢复留下的多余 about:blank
    if (const int n = cdp.CloseExtraBlankPages([&](const std::wstring& s) { Log(log, s); })) {
        Log(log, L"[gamapass-cdp] 已清理多余空白标签 ×" + std::to_wstring(n));
    }
    if (!skippedGalaxyNav) {
        Log(log, L"[gamapass-cdp] 已打开 Galaxy，开始自动点选…");
    } else {
        Log(log, L"[gamapass-cdp] 开始自动点选（未重新打开 Galaxy）…");
    }

    // FSM：步骤墙钟预算；点击 ack 超时可同步重试 1 次；AwaitLeave↔Wait 不刷新墙钟/不重计重试
    enum class Stage : int {
        GalaxyWaitGp = 0,
        AwaitLeaveGalaxy,
        AccWait,
        AwaitLeaveAcc,
        NickWaitRadio,
        NickWaitContinue,
        AwaitLeaveNick,
        TokenWait,
    };
    enum class Step : int { Galaxy = 0, Acc, Nick, Token };
    auto stageName = [](Stage s) -> const wchar_t* {
        switch (s) {
            case Stage::GalaxyWaitGp: return L"GalaxyWaitGp";
            case Stage::AwaitLeaveGalaxy: return L"AwaitLeaveGalaxy";
            case Stage::AccWait: return L"AccWait";
            case Stage::AwaitLeaveAcc: return L"AwaitLeaveAcc";
            case Stage::NickWaitRadio: return L"NickWaitRadio";
            case Stage::NickWaitContinue: return L"NickWaitContinue";
            case Stage::AwaitLeaveNick: return L"AwaitLeaveNick";
            case Stage::TokenWait: return L"TokenWait";
        }
        return L"?";
    };
    auto stepOf = [](Stage s) -> Step {
        switch (s) {
            case Stage::GalaxyWaitGp:
            case Stage::AwaitLeaveGalaxy:
                return Step::Galaxy;
            case Stage::AccWait:
            case Stage::AwaitLeaveAcc:
                return Step::Acc;
            case Stage::NickWaitRadio:
            case Stage::NickWaitContinue:
            case Stage::AwaitLeaveNick:
                return Step::Nick;
            case Stage::TokenWait:
                return Step::Token;
        }
        return Step::Galaxy;
    };

    constexpr DWORD kNavSettleMs = 1100;
    constexpr DWORD kPollMs = 1000;
    constexpr DWORD kAfterGpClickMs = 800;
    constexpr DWORD kAfterAccClickMs = 800;
    constexpr DWORD kAfterNickRadioMs = 1200;
    constexpr DWORD kAfterNickContinueMs = 800;
    constexpr DWORD kAccessTokenGraceMs = 5500;
    constexpr DWORD kClickAckMs = 28000;  // 单次点击等多久离开；超时可重试 1 次
    constexpr DWORD kStepGalaxyMs = 70000;
    constexpr DWORD kStepAccMs = 50000;
    constexpr DWORD kStepNickMs = 70000;

    const DWORD t0 = GetTickCount();
    FILETIME sessionNotBefore{};
    GetSystemTimeAsFileTime(&sessionNotBefore);
    // 允许约 2s 时钟/调度偏差，仍滤掉更早的残留 NGM/Classic
    {
        ULARGE_INTEGER uli{};
        uli.LowPart = sessionNotBefore.dwLowDateTime;
        uli.HighPart = sessionNotBefore.dwHighDateTime;
        constexpr ULONGLONG kSkew100ns = 2ULL * 10000000ULL;
        if (uli.QuadPart > kSkew100ns) uli.QuadPart -= kSkew100ns;
        sessionNotBefore.dwLowDateTime = uli.LowPart;
        sessionNotBefore.dwHighDateTime = uli.HighPart;
    }
    Stage stage = Stage::GalaxyWaitGp;
    DWORD stageEnteredAt = t0;   // 当前 Stage 进入时刻（ack 计时）
    DWORD stepWallAt = t0;       // 当前业务步骤墙钟（AwaitLeave↔Wait 不刷新）
    int gpClickRetry = 0;        // Galaxy 点击重试次数（最多 1）
    int accClickRetry = 0;       // Acc 点击重试次数（最多 1）
    bool chasedWebToken = false;
    bool sawAccessToken = false;
    bool forcedGalaxyAfterStaleOtt = false;
    bool sawNgmHint = false;         // NGM 更早信号；成功门禁仍是 Classic + cmdline 票
    bool parkedAwayFromMain = false; // 离开 Main?OTT，避免官网 JS 兑 init 票弹超时/假锁定
    DWORD accessTokenSeenTick = 0;
    DWORD ottHttpFailWaitSince = 0;  // Main HTTP 兑票失败后，先等官网拉起的起点
    DWORD ngmHintAt = 0;
    std::wstring galaxyInitOtt;      // Galaxy /login/init/… 会话 OTT（常被回填到 Main?OTT=，不可 HTTP 兑）
    std::wstring lastUrl;
    std::wstring pendingWebToken;
    std::wstring ackFromUrl;
    DWORD urlChangedTick = GetTickCount();
    DWORD noClickUntil = GetTickCount() + kNavSettleMs;
    DWORD nickRadioAt = 0;
    DWORD lastStageLog = 0;

    auto enterStage = [&](Stage next, const wchar_t* why, bool force = false) {
        if (!force && stage == next) return;
        const Step prevStep = stepOf(stage);
        const Step nextStep = stepOf(next);
        stage = next;
        stageEnteredAt = GetTickCount();
        // 仅业务步骤前进/后退时刷新墙钟；AwaitLeave↔同一步 Wait 不刷新
        if (force || nextStep != prevStep) {
            stepWallAt = GetTickCount();
        }
        Log(log, std::wstring(L"[gamapass-cdp] 状态→") + stageName(next) +
                     (why && *why ? (std::wstring(L"（") + why + L"）") : L""));
    };

    auto stepBudgetMs = [&](Step s) -> DWORD {
        switch (s) {
            case Step::Galaxy: return kStepGalaxyMs;
            case Step::Acc: return kStepAccMs;
            case Step::Nick: return kStepNickMs;
            case Step::Token: return 0;  // 吃总超时
        }
        return 60000;
    };

    auto inferStageFromUrl = [&](const std::wstring& hrefLower) -> Stage {
        if (hrefLower.find(L"access_token=") != std::wstring::npos ||
            hrefLower.find(L"webtoken=") != std::wstring::npos)
            return Stage::TokenWait;
        // Main 带 OTT 才进 TokenWait；空 Main 不进（避免 reuse-align 卡死）
        if (hrefLower.find(L"maplestoryclassic.beanfun.com") != std::wstring::npos) {
            if (hrefLower.find(L"ott=") != std::wstring::npos ||
                hrefLower.find(L"ott:") != std::wstring::npos)
                return Stage::TokenWait;
            return Stage::GalaxyWaitGp;
        }
        if (hrefLower.find(L"selectgameaccount") != std::wstring::npos)
            return Stage::NickWaitRadio;
        // 完整登录页 / error 不推进（由主循环显式 Fail；启动时也不应 reusable）
        if (IsGamaniaFullLoginUrl(hrefLower) || IsAccountsErrorUrl(hrefLower)) return stage;
        if (IsSelectAccountUrl(hrefLower)) return Stage::AccWait;
        if (IsOauthAuthorizeUrl(hrefLower)) return Stage::AwaitLeaveGalaxy;
        if (hrefLower.find(L"galaxy.games.gamania.com") != std::wstring::npos)
            return Stage::GalaxyWaitGp;
        return stage;
    };

    // 复用标签：立刻按 URL 对齐 stage，避免停在 GalaxyWaitGp 空等 Gama Pass
    if (skippedGalaxyNav && !curUrl.empty()) {
        lastUrl = curUrl;
        urlChangedTick = GetTickCount();
        noClickUntil = GetTickCount() + kNavSettleMs;
        const Stage inferred = inferStageFromUrl(ToLower(curUrl));
        enterStage(inferred, L"reuse-align", true);
    }

    auto failNeedManualLogin = [&]() -> HttpLoginResult {
        Log(log, L"[gamapass-cdp] 落到 accounts 完整登录页（非 select-account）。"
                 L"未调用 refresh、未清 Cookie；当前会话没有可用 SSO。");
        return Fail(HttpLoginError::BadInput,
                    "Gama Pass 需要先登录：浏览器打开的是完整登录表单，没有「已登录账号」可选。"
                    "程序没有调用 refresh、也没有清除 Cookie。"
                    "请在日志「浏览器=」对应的日常窗口登录 Gama Pass（accounts 选号页勾选记住），再点一键启动。"
                    "只用 Edge 请先卸载 Google Chrome（优先序：Chrome++/Chrome > Edge）。"
                    "（配置目录以日志 UserData= 为准。）");
    };

    auto tryOttFromUrl = [&](const std::wstring& url) -> HttpLoginResult {
        if (IsGalaxyInitSessionUrl(url)) {
            const std::wstring initOtt = ExtractOttToken(url);
            if (!initOtt.empty()) galaxyInitOtt = initOtt;
            return {};
        }
        std::wstring ott = ExtractOttToken(url);
        if (!ott.empty() && !galaxyInitOtt.empty() && ott == galaxyInitOtt) {
            // 实锤（Chrome148）：Main?OTT= 常回填 Galaxy init 会话票；HTTP 兑必失败，
            // 若立刻 stale-ott 重开 Galaxy → 用户看到「浏览器登录两次 / 游戏开两次」。
            // 正确做法：交给页内 JS / NGM 拉起，我们从 cmdline 接管。
            Log(log, L"[gamapass-cdp] Main 上的 OTT 与 Galaxy init 会话票相同，跳过 HTTP 兑票，"
                     L"等待官网拉起经典版…");
            if (stage != Stage::TokenWait)
                enterStage(Stage::TokenWait, L"init-ott-wait-classic");
            return {};
        }
        if (!ott.empty() && ott.find(L"OTT:") != std::wstring::npos &&
            ott.find(L":Login:") != std::wstring::npos &&
            !IsGalaxyInitSessionUrl(url)) {
            if (ToLower(url).find(L"ott=") != std::wstring::npos ||
                url.rfind(L"OTT:", 0) == 0 ||
                ToLower(url).find(L"maplestoryclassic.beanfun.com") != std::wstring::npos) {
                return TryFetchFromOtt(ott, log);
            }
        }
        return {};
    };

    // Main?OTT= 被我们 HTTP 兑票后，页内 JS 还会再兑一次 → 官网常误报「帳號已被鎖定」。
    // init 会话票同样会兑失败 → 「登录阶段超时」类弹窗。见 NGM / 换票成功后驶离。
    auto parkAwayFromMainOtt = [&](const wchar_t* why) {
        if (parkedAwayFromMain) return;
        const std::wstring lower = ToLower(lastUrl);
        if (lower.find(L"maplestoryclassic.beanfun.com") == std::wstring::npos) return;
        if (lower.find(L"ott=") == std::wstring::npos && lower.find(L"ott:") == std::wstring::npos)
            return;
        parkedAwayFromMain = true;
        Log(log, why && why[0] ? why
                               : L"[gamapass-cdp] 离开 Main?OTT（防官网二次兑票弹窗）…");
        cdp.Navigate(L"about:blank", [&](const std::wstring& s) { Log(log, s); });
        Sleep(200);
    };

    auto parkBrowserAfterTicketOk = [&]() {
        parkAwayFromMainOtt(
            L"[gamapass-cdp] 换票成功，离开 Main?OTT（防官网二次兑票误报「帳號鎖定」）…");
        if (!parkedAwayFromMain) {
            parkedAwayFromMain = true;
            cdp.Navigate(L"about:blank", [&](const std::wstring& s) { Log(log, s); });
            Sleep(200);
        }
    };

    auto returnIfTicketOk = [&](HttpLoginResult&& r) -> HttpLoginResult {
        if (r.ok && r.ticketFilled) {
            parkBrowserAfterTicketOk();
        }
        return std::move(r);
    };

    // NGM 只作「官网已开始拉起」的早信号；成功仍要求 Classic + 可解析 cmdline 票。
    // 只认本轮 sessionNotBefore 之后创建的 NGM，避免残留进程误报。
    // 见 NGM 即可关调试浏览器：票已在拉起链路上，网页不再需要。
    bool closedBrowserAfterNgm = false;
    auto noteNgmLaunchHint = [&]() {
        if (sawNgmHint) return false;
        if (!IsNgmProcessRunningCreatedAfter(sessionNotBefore)) return false;
        sawNgmHint = true;
        ngmHintAt = GetTickCount();
        Log(log, L"[gamapass-cdp] 探测到 NGM 已启动（官网拉起中；成功门禁仍等经典版 cmdline 票）…");
        if (!closedBrowserAfterNgm) {
            closedBrowserAfterNgm = true;
            Log(log, L"[gamapass-cdp] 已见 NGM，关闭登录用网页（后续只等经典版 cmdline 票）…");
            msc::cdp::CloseRemoteBrowser(kCdpPort, [&](const std::wstring& s) { Log(log, s); });
        }
        return true;
    };

    auto tryHarvestRunningClassic = [&]() -> HttpLoginResult {
        noteNgmLaunchHint();
        std::wstring cmd;
        bool matched = false;
        GalaxyTicket empty{};
        const DWORD pid = FindExistingClassicPid(empty, L"Maplestory_Classic.exe", &cmd, &matched,
                                                 &sessionNotBefore);
        if (!pid) return {};
        const auto parsed = ParseClassicPassArgs(cmd);
        if (!parsed.ok) {
            Log(log, L"[gamapass-cdp] 已有经典版 PID=" + std::to_wstring(pid) +
                         L" 但 cmdline 无 Galaxy 四元组，继续等…");
            return {};
        }
        Log(log, L"[gamapass-cdp] 官网已拉起经典版 PID=" + std::to_wstring(pid) +
                     L"，从 cmdline 接管票（跳过 NGM 重开）" +
                     (sawNgmHint ? L"（此前已见 NGM）" : L""));
        HttpLoginResult out;
        out.ok = true;
        out.error = HttpLoginError::Ok;
        out.message = "attach-existing-classic";
        out.ticket.userObjectId = parsed.userObjectId;
        out.ticket.userSessionToken = parsed.userSessionToken;
        out.ticket.gid = parsed.gid;
        out.ticket.galaxyGameId = parsed.galaxyGameId;
        out.ticket.ngmGameId = parsed.galaxyGameId;
        out.ticketFilled = true;
        out.ott = L"(from-classic-cmdline)";
        return out;
    };

    auto failStepTimeout = [&](Step s) -> HttpLoginResult {
        const char* name = s == Step::Galaxy   ? "Galaxy/GamaPass"
                           : s == Step::Acc    ? "选账号"
                           : s == Step::Nick   ? "选昵称"
                                                 : "收票";
        return Fail(HttpLoginError::OttMissing,
                    std::string("Gama Pass 阶段超时：") + name + "。请检查网络后重试。");
    };

    while ((int)(GetTickCount() - t0) < timeoutMs) {
        const DWORD nowTick = GetTickCount();
        std::wstring url;
        if (cdp.GetUrl(url, nullptr) && !url.empty()) {
            if (url != lastUrl) {
                lastUrl = url;
                urlChangedTick = GetTickCount();
                noClickUntil = (std::max)(noClickUntil, urlChangedTick + kNavSettleMs);
                Log(log, L"[gamapass-cdp] nav " + url.substr(0, 160));

                const std::wstring lowerNav = ToLower(url);
                // ★ 禁止改写 OAuth prompt=login→none：实机会进 accounts.gamania.com/error（build34）
                // Galaxy 自带的 prompt=login 原样放行，等自然跳到 select-account /login
                if (IsAccountsErrorUrl(lowerNav)) {
                    Log(log, L"[gamapass-cdp] 落到 accounts/error（OAuth 失败）。"
                             L"不会清 Cookie；请回 Galaxy 页再点一次 Gama Pass，或手动登录后重试。");
                    return Fail(HttpLoginError::Protocol,
                                "Gama Pass OAuth 失败（accounts/error）。"
                                "请回到 Galaxy 登录页重新点 Gama Pass；程序不会清 Cookie。");
                }
                if (stage == Stage::AwaitLeaveGalaxy) {
                    // oauth2/authorize 是中间跳转：继续等，勿当完整登录页、勿改 URL
                    if (IsOauthAuthorizeUrl(lowerNav)) {
                        // stay AwaitLeaveGalaxy
                    } else if (IsSelectAccountUrl(lowerNav)) {
                        enterStage(Stage::AccWait, L"gp-click-acked");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    } else if (IsGamaniaFullLoginUrl(lowerNav)) {
                        return failNeedManualLogin();
                    } else if (lowerNav.find(L"selectgameaccount") != std::wstring::npos) {
                        enterStage(Stage::NickWaitRadio, L"gp-skip-to-nick");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    } else if (lowerNav.find(L"access_token=") != std::wstring::npos ||
                               lowerNav.find(L"maplestoryclassic.beanfun.com") !=
                                   std::wstring::npos) {
                        enterStage(Stage::TokenWait, L"gp-skip-to-token");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    }
                } else if (stage == Stage::AwaitLeaveAcc) {
                    if (lowerNav.find(L"selectgameaccount") != std::wstring::npos) {
                        enterStage(Stage::NickWaitRadio, L"acc-click-acked");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    } else if (lowerNav.find(L"access_token=") != std::wstring::npos ||
                               lowerNav.find(L"maplestoryclassic.beanfun.com") !=
                                   std::wstring::npos) {
                        enterStage(Stage::TokenWait, L"acc-skip-to-token");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    } else if (lowerNav.find(L"select-account") == std::wstring::npos &&
                               lowerNav.find(L"accounts.gamania.com") == std::wstring::npos) {
                        Stage inferred = inferStageFromUrl(lowerNav);
                        if (inferred != Stage::AccWait && inferred != stage)
                            enterStage(inferred, L"acc-left");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    }
                } else if (stage == Stage::AwaitLeaveNick) {
                    if (lowerNav.find(L"selectgameaccount") == std::wstring::npos ||
                        lowerNav.find(L"access_token=") != std::wstring::npos) {
                        enterStage(Stage::TokenWait, L"nick-continue-acked");
                        noClickUntil = GetTickCount() + kNavSettleMs;
                    }
                } else {
                    Stage inferred = inferStageFromUrl(lowerNav);
                    if (inferred != stage && (int)inferred > (int)stage)
                        enterStage(inferred, L"url-advance");
                }
            }

            const std::wstring lowerNav = ToLower(url);
            if (IsAccountsErrorUrl(lowerNav)) {
                Log(log, L"[gamapass-cdp] 落到 accounts/error（OAuth 失败）。"
                         L"不会清 Cookie；请回 Galaxy 页再点一次 Gama Pass。");
                return Fail(HttpLoginError::Protocol,
                            "Gama Pass OAuth 失败（accounts/error）。"
                            "请回到 Galaxy 登录页重新点 Gama Pass；程序不会清 Cookie。");
            }
            // 任意阶段落到完整登录页：立刻停（禁止 soft-retry 再 Navigate Galaxy）
            if (IsGamaniaFullLoginUrl(lowerNav)) {
                return failNeedManualLogin();
            }
            if (lowerNav.find(L"errorhandler") != std::wstring::npos ||
                lowerNav.find(L"spga0001") != std::wstring::npos ||
                lowerNav.find(L"%e9%96%92%e7%bd%ae") != std::wstring::npos ||
                lowerNav.find(L"sga0004") != std::wstring::npos) {
                // 绝不 soft-retry Navigate Galaxy（禁止冲刷/重鉴权登录态）
                return Fail(HttpLoginError::Protocol,
                            "Gama Pass 选号失败（SPGA0001 閒置過久 / 验证失败）。"
                            "请关闭多余登录标签后，在当前浏览器会话内重试（不会清 Cookie）。");
            }

            auto fromOtt = returnIfTicketOk(tryOttFromUrl(url));
            if (fromOtt.ok && fromOtt.ticketFilled) return fromOtt;
            // HTTP 兑票失败：先等官网自己拉起经典版（常见于 init OTT 已被页内兑过）。
            // 禁止立刻重开 Galaxy——会整段点选第二遍，体感=登录两次/开两次游戏。
            // 若已见 NGM：说明官网已在拉，再宽限到 45s，勿过早 stale-ott-retry。
            if (!fromOtt.message.empty() &&
                ToLower(url).find(L"maplestoryclassic.beanfun.com") != std::wstring::npos &&
                (ToLower(url).find(L"ott=") != std::wstring::npos ||
                 ToLower(url).find(L"ott:") != std::wstring::npos)) {
                noteNgmLaunchHint();
                if (ottHttpFailWaitSince == 0) {
                    ottHttpFailWaitSince = GetTickCount();
                    enterStage(Stage::TokenWait, L"ott-http-fail-wait-classic");
                    Log(log, L"[gamapass-cdp] Main OTT HTTP 换票失败，先等官网拉起经典版"
                             L"（见 NGM 则宽限 45s，否则 20s 内不重开 Galaxy）…");
                } else if (!forcedGalaxyAfterStaleOtt) {
                    const DWORD waited = GetTickCount() - ottHttpFailWaitSince;
                    const bool ngmAlive = IsNgmProcessRunningCreatedAfter(sessionNotBefore);
                    if (ngmAlive) noteNgmLaunchHint();
                    const DWORD waitBudget =
                        (sawNgmHint || ngmAlive) ? 45000u : 20000u;
                    if (waited > waitBudget) {
                        forcedGalaxyAfterStaleOtt = true;
                        Log(log, L"[gamapass-cdp] 等待官网拉起超时" +
                                     (sawNgmHint ? std::wstring(L"（曾见 NGM）") : L"") +
                                     L"，最后一次重开 Galaxy…");
                        if (!cdp.Navigate(kGalaxyLogin,
                                          [&](const std::wstring& s) { Log(log, s); })) {
                            return Fail(HttpLoginError::Network, "无法打开 Galaxy 登录页");
                        }
                        sawAccessToken = false;
                        chasedWebToken = false;
                        ottHttpFailWaitSince = 0;
                        galaxyInitOtt.clear();
                        enterStage(Stage::GalaxyWaitGp, L"stale-ott-retry", true);
                        urlChangedTick = GetTickCount();
                        noClickUntil = urlChangedTick + kNavSettleMs;
                        Sleep(1200);
                        continue;
                    }
                }
            }

            const std::wstring lower = ToLower(url);
            if (lower.find(L"access_token=") != std::wstring::npos) {
                pendingWebToken = ExtractQueryValueCi(url, L"access_token");
                if (!sawAccessToken) {
                    sawAccessToken = true;
                    accessTokenSeenTick = GetTickCount();
                    enterStage(Stage::TokenWait, L"access_token");
                    Log(log, L"[gamapass-cdp] 收到 access_token，等待官网自然回跳/拉起"
                             L"（不打断 result 页）…");
                }
            } else {
                std::wstring wt = ExtractQueryValueCi(url, L"WebToken");
                if (!wt.empty()) {
                    pendingWebToken = wt;
                    if (!chasedWebToken) {
                        chasedWebToken = true;
                        enterStage(Stage::TokenWait, L"WebToken");
                        const std::wstring chase =
                            std::wstring(kClassicMain) + L"?WebToken=" + pendingWebToken;
                        Log(log, L"[gamapass-cdp] 收到 WebToken，导航官网收 OTT…");
                        cdp.Navigate(chase, [&](const std::wstring& s) { Log(log, s); });
                        urlChangedTick = GetTickCount();
                        noClickUntil = urlChangedTick + kNavSettleMs;
                        Sleep(1500);
                        continue;
                    }
                }
            }
        }

        if (sawAccessToken || stage == Stage::TokenWait) {
            noteNgmLaunchHint();
            auto harvested = returnIfTicketOk(tryHarvestRunningClassic());
            if (harvested.ok && harvested.ticketFilled) return harvested;
            // 已见 NGM：加快轮询经典版；未到 Classic 成功门禁前不提前返回失败
            if (sawNgmHint ||
                (sawAccessToken && GetTickCount() - accessTokenSeenTick < kAccessTokenGraceMs)) {
                Sleep(sawNgmHint ? 300 : 500);
                continue;
            }
        }

        {
            const std::wstring hrefLower = ToLower(lastUrl);
            if (hrefLower.find(L"maplestoryclassic.beanfun.com") != std::wstring::npos) {
                noteNgmLaunchHint();
                std::string ev;
                cdp.Evaluate(JsScrapeTicketOtt(), ev, nullptr);
                const std::string scraped = StripJsonStringValue(ev);
                if (scraped.size() >= 16 && scraped.find("OTT:") == 0) {
                    std::wstring ott(scraped.begin(), scraped.end());
                    Log(log, L"[gamapass-cdp] 从 Main 页扫到 OTT");
                    auto scrapedR = returnIfTicketOk(TryFetchFromOtt(ott, log));
                    if (scrapedR.ok && scrapedR.ticketFilled) return scrapedR;
                    // ★ 禁止换票失败就 return：过期 OTT 残留在 DOM 时会整段登录误杀（守护重拉常见）
                    Log(log, L"[gamapass-cdp] 扫到的 OTT 换票失败，继续等待/点选…");
                }
                auto harvested = returnIfTicketOk(tryHarvestRunningClassic());
                if (harvested.ok && harvested.ticketFilled) return harvested;
                if (sawNgmHint) {
                    Sleep(300);
                    continue;
                }
            }
        }

        // 步骤墙钟：必须用「当前」GetTickCount；勿用本轮开头的 nowTick——
        // 同轮 enterStage 会刷新 stepWallAt，若 stepWallAt > nowTick 则 DWORD 下溢 → 瞬间「阶段超时」
        {
            const DWORD wallNow = GetTickCount();
            const Step curStep = stepOf(stage);
            const DWORD budget = stepBudgetMs(curStep);
            if (budget > 0 && wallNow - stepWallAt > budget) {
                return failStepTimeout(curStep);
            }
            if (curStep == Step::Token && wallNow - lastStageLog > 8000) {
                lastStageLog = wallNow;
                Log(log, sawNgmHint
                             ? L"[gamapass-cdp] TokenWait：已见 NGM，仍等经典版 cmdline 票…"
                             : L"[gamapass-cdp] TokenWait 仍在等 OTT/经典版进程（受总超时约束）…");
            }
        }

        // AwaitLeave*：ack 超时同样用 wallNow，避免同轮 enterStage 后误触发重点
        if (stage == Stage::AwaitLeaveGalaxy || stage == Stage::AwaitLeaveAcc ||
            stage == Stage::AwaitLeaveNick) {
            const DWORD ackNow = GetTickCount();
            if (ackNow - stageEnteredAt > kClickAckMs) {
                if (stage == Stage::AwaitLeaveGalaxy) {
                    if (gpClickRetry < 1) {
                        ++gpClickRetry;
                        Log(log, L"[gamapass-cdp] GamaPass 点击未确认离开，重试一次");
                        ackFromUrl.clear();
                        noClickUntil = GetTickCount() + 500;
                        enterStage(Stage::GalaxyWaitGp, L"ack-timeout-reclick");
                    } else {
                        return failStepTimeout(Step::Galaxy);
                    }
                } else if (stage == Stage::AwaitLeaveAcc) {
                    if (accClickRetry < 1) {
                        ++accClickRetry;
                        Log(log, L"[gamapass-cdp] 选账号点击未确认离开，重试一次");
                        ackFromUrl.clear();
                        noClickUntil = GetTickCount() + 500;
                        enterStage(Stage::AccWait, L"ack-timeout-reclick");
                    } else {
                        return failStepTimeout(Step::Acc);
                    }
                } else {
                    // AwaitLeaveNick：绝不重开 Galaxy、绝不再点繼續（防冲刷登录态 / SPGA0001）
                    Log(log, L"[gamapass-cdp] 選號後未离开，停止（不重开登录页、不清 Cookie）");
                    return failStepTimeout(Step::Nick);
                }
            }
            Sleep(400);
            continue;
        }

        if (stage == Stage::TokenWait) {
            Sleep(kPollMs);
            continue;
        }

        if (nowTick < noClickUntil || nowTick - urlChangedTick < kNavSettleMs) {
            Sleep(300);
            continue;
        }

        std::string ev;
        auto runJs = [&](const std::wstring& js) {
            ev.clear();
            cdp.Evaluate(js, ev, nullptr);
            return StripJsonStringValue(ev);
        };

        std::wstring hrefLower = ToLower(lastUrl);
        const std::string stageDom = runJs(JsDomStageReady());
        if (stageDom.rfind("wait-", 0) == 0) {
            if (nowTick - lastStageLog > 5000) {
                lastStageLog = nowTick;
                Log(log, L"[gamapass-cdp] 等待页面就绪… " +
                             std::wstring(stageDom.begin(), stageDom.end()) + L" @" +
                             stageName(stage));
            }
            Sleep(kPollMs);
            continue;
        }

        if (stage == Stage::GalaxyWaitGp &&
            hrefLower.find(L"galaxy.games.gamania.com") != std::wstring::npos &&
            hrefLower.find(L"access_token=") == std::wstring::npos &&
            stageDom.find("ready-gp") == 0) {
            auto r = runJs(JsClickGamaPassProvider());
            if (r.find("click-gamapass") == 0) {
                ackFromUrl = lastUrl;
                noClickUntil = GetTickCount() + kAfterGpClickMs;
                enterStage(Stage::AwaitLeaveGalaxy, L"clicked-gp");
                Log(log, L"[gamapass-cdp] " + std::wstring(r.begin(), r.end()));
            }
        } else if (stage == Stage::AccWait && IsSelectAccountUrl(hrefLower) &&
                   stageDom.find("ready-acc") == 0) {
            auto r = runJs(JsSelectAccount(accountSlot));
            if (r.find("select-account|") == 0) {
                ackFromUrl = lastUrl;
                noClickUntil = GetTickCount() + kAfterAccClickMs;
                enterStage(Stage::AwaitLeaveAcc, L"clicked-acc");
                Log(log, L"[gamapass-cdp] " + std::wstring(r.begin(), r.end()));
            } else if (r.find("wait-select-account") == 0 && nowTick - lastStageLog > 5000) {
                lastStageLog = nowTick;
                Log(log, L"[gamapass-cdp] 等待点选账号卡片… " +
                             std::wstring(r.begin(), r.end()));
            }
        } else if (stage == Stage::NickWaitRadio &&
                   hrefLower.find(L"selectgameaccount") != std::wstring::npos) {
            // 已有勾选也可能不是目标槽：一律走选槽 JS（目标已勾选则直接點繼續）
            if (stageDom.find("ready-nick-continue") == 0 ||
                stageDom.find("ready-nick-radio") == 0) {
                auto r = runJs(JsSelectGameNick(nickSlot));
                if (r.find("nick-radio-selected") == 0 || r.find("nick-row") == 0) {
                    nickRadioAt = GetTickCount();
                    noClickUntil = nickRadioAt + kAfterNickRadioMs;
                    enterStage(Stage::NickWaitContinue, L"radio-selected");
                    Log(log, L"[gamapass-cdp] " + std::wstring(r.begin(), r.end()));
                } else if (r.find("nick-continue") == 0) {
                    ackFromUrl = lastUrl;
                    noClickUntil = GetTickCount() + kAfterNickContinueMs;
                    enterStage(Stage::AwaitLeaveNick, L"clicked-continue");
                    Log(log, L"[gamapass-cdp] " + std::wstring(r.begin(), r.end()));
                }
            }
        } else if (stage == Stage::NickWaitContinue &&
                   hrefLower.find(L"selectgameaccount") != std::wstring::npos) {
            if (nickRadioAt != 0 && GetTickCount() - nickRadioAt < kAfterNickRadioMs) {
                Sleep(300);
                continue;
            }
            if (stageDom.find("ready-nick-continue") != 0 &&
                stageDom.find("ready-nick-radio") != 0) {
                if (stageDom.rfind("wait-", 0) == 0 && nowTick - lastStageLog > 5000) {
                    lastStageLog = nowTick;
                    Log(log, L"[gamapass-cdp] 等待选号 DOM… " +
                                 std::wstring(stageDom.begin(), stageDom.end()));
                }
                Sleep(kPollMs);
                continue;
            }
            if (stageDom.find("ready-nick-radio") == 0) {
                auto r = runJs(JsSelectGameNick(nickSlot));
                if (r.find("nick-radio-selected") == 0 || r.find("nick-row") == 0) {
                    nickRadioAt = GetTickCount();
                    noClickUntil = nickRadioAt + kAfterNickRadioMs;
                    Log(log, L"[gamapass-cdp] " + std::wstring(r.begin(), r.end()));
                }
                Sleep(kPollMs);
                continue;
            }
            auto r = runJs(JsSelectGameNick(nickSlot));
            if (r.find("nick-continue") == 0) {
                ackFromUrl = lastUrl;
                noClickUntil = GetTickCount() + kAfterNickContinueMs;
                enterStage(Stage::AwaitLeaveNick, L"clicked-continue");
                Log(log, L"[gamapass-cdp] " + std::wstring(r.begin(), r.end()) +
                             L"（僅一次）");
            } else if (r.find("wait-") == 0 && nowTick - lastStageLog > 5000) {
                lastStageLog = nowTick;
                Log(log, L"[gamapass-cdp] 等待选号 DOM… " +
                             std::wstring(r.begin(), r.end()));
            }
        }

        Sleep(kPollMs);
    }

    {
        auto harvested = returnIfTicketOk(tryHarvestRunningClassic());
        if (harvested.ok && harvested.ticketFilled) return harvested;
    }

    Log(log, std::wstring(L"[gamapass-cdp] 超时 @") + stageName(stage) +
                 L"。若停在登录/选号页，请在当前浏览器窗口手动点一下；"
                 L"程序不会另开登录页、也不会调用 refresh。");
    return Fail(HttpLoginError::OttMissing, "浏览器点选超时，未捕获 OTT");

}

}  // namespace msc::launcher
