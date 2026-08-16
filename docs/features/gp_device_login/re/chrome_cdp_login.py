# Reconstructed from Nuitka-compiled chrome_login_gui.dll
# GamaPassLogin-keep-login-fixed-v15.exe -> chrome_login_gui.dll
# SHA256(dll)=71295f3397f39f45d06d4b6c635c65a90d8b9b30781df2d2c59b3a3000e1b64d
# Source name in binary: chrome_cdp_login.py
# This is const-table / co_names reconstruction, not Hex-Rays of every helper.
# Timeout numeric literals were not recovered; names are original.

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any
from pathlib import Path

RECORD_SEPARATOR = "----"
SITE_ORIGIN = "https://accounts.gamania.com"
BEANFUN_START_URL = "https://maplestoryclassic.beanfun.com/Main"
GAME_LOGIN_INIT = "https://galaxy.games.gamania.com/webapi/view/login/init"

# Names present as co_consts. Values not recovered from immediates.
CDP_TIMEOUT_SECONDS = None  # aCDP_TIMEOUT_SECONDS
PAGE_READY_TIMEOUT_SECONDS = None  # aPAGE_READY_TIMEOUT_SECONDS
TARGET_TIMEOUT_SECONDS = None  # aTARGET_TIMEOUT_SECONDS
GAME_START_TIMEOUT_SECONDS = None  # aGAME_START_TIMEOUT_SECONDS

HEX32 = "0123456789abcdefABCDEF"


@dataclass
class LoginRecord:
    email: str
    mail_password: str
    gama_password: str
    device_id: str
    user_token: str
    refresh_token: str


def parse_record(line: str) -> LoginRecord:
    """CLI / file import. Exactly six parts. Field 2 is mailbox password, field 3 is Gama password."""
    parts = line.strip().split(RECORD_SEPARATOR)
    if len(parts) != 6:
        raise ValueError(
            "数据必须正好包含六段：mail_address----mail_password----gama_password----device_id----login_user_token----login_refresh_token"
        )
    email, mail_password, gama_password, device_id, user_token, refresh_token = parts
    if "@" not in email:
        raise ValueError("mail_address 格式无效")
    if len(device_id) != 32 or any(c not in HEX32 for c in device_id):
        raise ValueError("device_id 必须是32位十六进制字符串")
    # login_user_token 不是有效的 JWT 格式 / payload 校验在后续 decode
    if not refresh_token:
        raise ValueError("login_refresh_token 不能为空")
    return LoginRecord(email, mail_password, gama_password, device_id, user_token, refresh_token)


# Chrome launch flags recovered as unicode constants:
CHROME_FLAGS = [
    "--remote-debugging-port=",  # + port, default 9222
    "--remote-debugging-address=",
    "--remote-allow-origins=",
    "--user-data-dir=",
    "--no-default-browser-check",
    "--disable-background-networking",
    "--disable-component-update",
    "--disable-default-apps",
]

# Registry lookup for chrome.exe:
# HKLM SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe
# HKLM SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe
# plus Google/Chrome/Application/chrome.exe and Beta/SxS/Chrome for Testing.


JS_BOOT_DEVICE = (
    "if (location.hostname === 'accounts.gamania.com') {"
    "try { localStorage.setItem('device_id', %s); } catch (_) {}}"
)

JS_SET_DEVICE_AND_READ = "localStorage.setItem('device_id', %s); localStorage.getItem('device_id')"

JS_READ_DEVICE = "localStorage.getItem('device_id')"

JS_ACCOUNTS_READY = (
    "location.hostname === 'accounts.gamania.com' && "
    "(document.readyState === 'interactive' || document.readyState === 'complete')"
)

JS_DOC_READY = "document.readyState === 'interactive' || document.readyState === 'complete'"

JS_FILL_ACCOUNT_PICK = """
          const all = [...document.querySelectorAll('input')].filter(visible);
          input = all.find(el => ['email', 'tel'].includes((el.type || '').toLowerCase()))
               || all.find(el => ['username', 'email'].includes((el.autocomplete || '').toLowerCase()))
               || all.find(el => (el.type || 'text').toLowerCase() === 'text');
"""

JS_FILL_PASSWORD_PICK = """
          input = [...document.querySelectorAll('input')].find(el =>
            visible(el) && ((el.type || '').toLowerCase() === 'password'
              || (el.autocomplete || '').toLowerCase() === 'current-password'));
"""

JS_FILL_SETTER_HEAD = """
      (() => {
        const visible = el => {
          const style = getComputedStyle(el), rect = el.getBoundingClientRect();
          return !el.disabled && style.display !== 'none' && style.visibility !== 'hidden'
            && rect.width > 0 && rect.height > 0;
        };
        let input = null;
"""

JS_FILL_SETTER_TAIL = """
        if (!input) return false;
        const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
        setter.call(input, %s);
        input.dispatchEvent(new InputEvent('input', {bubbles: true, inputType: 'insertText', data: null}));
        input.dispatchEvent(new Event('change', {bubbles: true}));
        input.focus();
        return true;
      })()
"""

JS_CLICK_PRIMARY = r"""
    const visible = el => {
      const style = getComputedStyle(el), rect = el.getBoundingClientRect();
      return !el.disabled && style.display !== 'none' && style.visibility !== 'hidden'
        && rect.width > 0 && rect.height > 0;
    };
    const controls = [...document.querySelectorAll('button, input[type="submit"], [role="button"]')]
      .filter(visible);
    const words = ['登入', '登录', '下一步', '繼續', '继续', 'login', 'sign in', 'continue', 'next'];
    const preferred = controls.find(el => el.matches('button[type="submit"], input[type="submit"]'))
      || controls.find(el => words.some(word => (el.innerText || el.value || '').toLowerCase().includes(word)))
      || controls[0];
    if (!preferred) return false;
    preferred.click();
"""

JS_PASSWORD_EXISTS = r"""
  (() => [...document.querySelectorAll('input')].some(el => {
    const rect = el.getBoundingClientRect(), style = getComputedStyle(el);
    return (el.type || '').toLowerCase() === 'password' && !el.disabled
      && rect.width > 0 && rect.height > 0 && style.display !== 'none' && style.visibility !== 'hidden';
  }))()
"""

JS_KEEP_SIGNED_IN_FIND = r"""
    const visible = el => {
      const rect = el.getBoundingClientRect(), style = getComputedStyle(el);
      return !el.disabled && rect.width > 0 && rect.height > 0
        && style.display !== 'none' && style.visibility !== 'hidden';
    };
    const normalized = value => (value || '').replace(/\s+/g, ' ').trim().toLowerCase();
    const keywords = [
      '保持登入', '保持登录', '保持登入狀態', '保持登录状态',
      '記住我', '记住我', 'remember me', 'stay signed in',
      'keep me signed in', 'keep signed in'
    ];
    const description = el => {
      const values = [
        el.textContent,
        el.getAttribute('aria-label'), el.getAttribute('name'), el.id,
        el.getAttribute('data-testid'), el.getAttribute('data-test')
      ];
      if (el.id) {
        const label = document.querySelector(`label[for="${CSS.escape(el.id)}"]`);
        if (label) values.push(label.textContent);
      }
      const wrappingLabel = el.closest('label');
      if (wrappingLabel) values.push(wrappingLabel.textContent);
      if (el.parentElement) values.push(el.parentElement.textContent);
      return normalized(values.filter(Boolean).join(' '));
    };
    const controls = [...document.querySelectorAll('input[type="checkbox"], [role="checkbox"]')]
      .filter(visible);
    const control = controls.find(el => {
      const text = description(el);
      return keywords.some(keyword => text.includes(keyword));
    });
    if (!control) {
      return {found: false, checked: false, candidates: controls.map(description).slice(0, 8)};
    }
    const isChecked = el => el.matches('input[type="checkbox"]')
      ? Boolean(el.checked)
      : el.getAttribute('aria-checked') === 'true';
    control.scrollIntoView({block: 'center', inline: 'center'});
    const rect = control.getBoundingClientRect();
    return {
      found: true,
      checked: isChecked(control),
      description: description(control),
      x: rect.left + rect.width / 2,
      y: rect.top + rect.height / 2
    };
"""

JS_KEEP_SIGNED_IN_CHECKED = r"""
    const normalized = value => (value || '').replace(/\s+/g, ' ').trim().toLowerCase();
    const keywords = [
      '保持登入', '保持登录', '保持登入狀態', '保持登录状态',
      '記住我', '记住我', 'remember me', 'stay signed in',
      'keep me signed in', 'keep signed in'
    ];
    return [...document.querySelectorAll('input[type="checkbox"], [role="checkbox"]')]
      .some(el => {
        const text = normalized(el.textContent + ' ' + (el.getAttribute('aria-label') || ''));
        const matched = keywords.some(keyword => text.includes(keyword));
        const checked = el.matches('input[type="checkbox"]')
          ? Boolean(el.checked)
          : el.getAttribute('aria-checked') === 'true';
        return matched && checked;
      });
"""

JS_GAMESTART_CLICK = r"""
    const control = document.getElementById('gamestart');
    if (!control || control.disabled) return false;
    control.scrollIntoView({block: 'center', inline: 'center'});
    control.click();
"""

JS_INJECT_SESSION = """
          localStorage.setItem('device_id', data.deviceId);
          localStorage.setItem('current_user_open_id', data.openId);
          localStorage.setItem('save_user_token_way', '0');
          localStorage.setItem('track_id', data.trackId);
          localStorage.setItem(`user_${data.openId}`, JSON.stringify(data.userStorage));
"""

JS_HARVEST = """
            (() => ({
              url: location.href,
              currentOpenId: localStorage.getItem('current_user_open_id') || '',
              hasPasswordInput: [...document.querySelectorAll('input')]
                .some(el => (el.type || '').toLowerCase() === 'password'
                  && el.getBoundingClientRect().width > 0)
            }))()
"""


def set_input_expression(kind: str, value: str) -> str:
    """kind is 'account' or 'password'. Value is JSON-string-literal inserted into setter.call."""
    pick = JS_FILL_ACCOUNT_PICK if kind == "account" else JS_FILL_PASSWORD_PICK
    return JS_FILL_SETTER_HEAD + pick + (JS_FILL_SETTER_TAIL % json.dumps(value))


def force_device_storage(cdp, device_id: str) -> None:
    """Write device_id through both the page and CDP's origin storage API."""
    cdp.command("DOMStorage.enable", {})
    cdp.command(
        "DOMStorage.setDOMStorageItem",
        {
            "storageId": {"securityOrigin": SITE_ORIGIN, "isLocalStorage": True},
            "key": "device_id",
            "value": device_id,
        },
    )
    got = cdp.evaluate(JS_SET_DEVICE_AND_READ % json.dumps(device_id))
    if got != device_id:
        raise RuntimeError("当前 Accounts 页面无法持久化 device_id")


def automate_login(account: str, password: str, device_id: str, chrome_path: str, profile_dir: str) -> None:
    """GUI worker. Does NOT take mail_password.

    Recovered sequence from status strings + function names:
      launch_chrome / use_existing_profile_cdp
      wait_for_cdp -> find_or_create_target
      Page.enable, Runtime.enable
      optional: beanfun Main / galaxy login/init, click Sign in with Gama Pass
      Page.addScriptToEvaluateOnNewDocument (JS_BOOT_DEVICE)
      wait accounts ready
      force_device_storage + reload  ("正在写入 device_id 缓存并刷新")
      verify localStorage.getItem('device_id')
      set_input_expression('account')  ("正在填写登录账号")
      ENSURE_KEEP_SIGNED_IN: evaluate find-js, Input.dispatchMouseEvent at (x,y)
      KEEP_SIGNED_IN_CHECKED
      CLICK_PRIMARY_BUTTON  ("已勾选保持登录，正在进入密码页面")
      wait PASSWORD_INPUT_EXISTS
      set_input_expression('password')
      CLICK_PRIMARY_BUTTON
      post_login_outcome (two-factor / galaxy / still on password)
    """
    raise NotImplementedError("CDP session wrapper not reconstructed; sequence above is the recovered control flow")


def post_login_outcome(url: str, has_password_input: bool) -> str:
    if "two-factor" in (url or "").lower() or "input-password" in (url or ""):
        return "账号密码已通过，当前需要邮箱二次验证，请在已打开的浏览器中继续。"
    if "galaxy.games.gamania.com" in (url or "") or "/login/init/" in (url or ""):
        return "登录成功，已经返回新楓之谷：經典版游戏启动流程。"
    if has_password_input:
        return "登录未完成，页面仍停留在密码步骤，请检查页面上的错误提示。"
    return "登录成功，官网已经写入当前用户登录态。"


def inject_session_from_tokens(cdp, record: LoginRecord) -> None:
    """CLI path when login_user_token / refresh_token are present.

    JWT payload must contain sub/openId. Expired access token:
    "access token 已过期，将由官网尝试 refresh" -- they still write LS, do not call /v1/refresh/token themselves.
    """
    raise NotImplementedError("JWT decode + JS_INJECT_SESSION evaluate")


def open_game_page_only(chrome_path: str, profile_dir: str) -> None:
    """Open the game homepage in the dedicated Chrome without touching login data."""
    raise NotImplementedError("navigate beanfun Main, click #gamestart")
