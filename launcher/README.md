# 经典版启动 / 换票（嵌入 xcat.exe）

主入口是 `bin\xcat.exe`（`msc_launch` 静态库）。换票：**GAMA PASS CDP** 或 **gamania (HK) HTTP**；**无 WebView2**。

## 用法（产品 GUI）

1. 运行 `bin\xcat.exe`
2. 启动模式选 **GAMA PASS自动登录**（默认浏览器点选）或 **gamania (HK)**（粘贴账密）
3. 点一键 / 等冷启自动换票

## 本地文件

| 路径 | 说明 |
|---|---|
| `bin\account.txt`（与 exe 同级） | HK 账密串（明文，勿外传；**不写** `%LocalAppData%`） |
| `bin\auth_strategy.txt` | `gama_pass` / `http_first`（历史 `http_only`/`webview_only` 读盘归一为 HTTP） |
| `bin\captcha_ui.txt` | `popup_on_captcha`（开浏览器）/ `silent`（不开） |
| `bin\launcher.log` | 换票文本日志 |

冒烟：`msc_launch_smoke`（仅 NGM/HTTP 核心，非 GUI）。
