# Reconstructed from Nuitka-compiled chrome_login_gui.dll
# Source name in binary: chrome_login_gui.py
# Tkinter GUI. Login worker calls chrome_cdp_login.automate_login with
# (account, password, device_id) only -- mailbox password is kept in memory and not sent.

from __future__ import annotations

import re
import threading
from pathlib import Path

RECORD_SEPARATOR = "----"
HEX32 = "0123456789abcdefABCDEF"


def parse_input(raw: str) -> tuple[str, str, str, str]:
    """Seller / UI line. Four parts. Field 2 is Gama Pass password, field 3 is mailbox password."""
    line = raw.strip()
    parts = line.split(RECORD_SEPARATOR)
    if len(parts) != 4:
        raise ValueError("请输入四段数据：账号----密码----邮箱密码----device_id")
    account, password, mail_password, device_id = parts
    if "@" not in account:
        raise ValueError("账号必须是有效邮箱地址")
    if not password:
        raise ValueError("Gama Pass 密码不能为空")
    if not mail_password:
        raise ValueError("邮箱密码不能为空")
    if len(device_id) != 32 or any(c not in HEX32 for c in device_id):
        raise ValueError("device_id 必须是32位十六进制字符串")
    return account, password, mail_password, device_id


class LoginApp:
    """Small Windows GUI for Gama Pass password login through Chrome CDP.

    Recovered widgets / methods:
      load_profile_registry / save_profile_registry / Profiles/
      current_profile_dir, switch_profile, create_profile, rename_profile
      choose_chrome (chrome.exe)
      accept_disclaimer Checkbutton -> update_start_state
      start -> worker thread name chrome-cdp-login -> automate_login
      open_page -> chrome-cdp-open-page -> open_game_page_only
      show_error_log copies traceback + chrome launch log tail
    """

    def start(self) -> None:
        # 请先阅读并勾选免责声明
        account, password, mail_password, device_id = parse_input(self.input_text.get("1.0", "end"))
        self._mail_password = mail_password  # 邮箱密码只在内存中解析，本次 Gama Pass 登录不会使用。
        chrome_path = self.chrome_path
        profile_dir = self.current_profile_dir()

        def worker() -> None:
            from chrome_cdp_login import automate_login

            automate_login(account, password, device_id, chrome_path, profile_dir)

        threading.Thread(target=worker, name="chrome-cdp-login", daemon=True).start()

    def open_page(self) -> None:
        def worker() -> None:
            from chrome_cdp_login import open_game_page_only

            open_game_page_only(self.chrome_path, self.current_profile_dir())

        threading.Thread(target=worker, name="chrome-cdp-open-page", daemon=True).start()

    def current_profile_dir(self) -> Path:  # pragma: no cover
        raise NotImplementedError

    input_text = None
    chrome_path = None
    _mail_password = None
