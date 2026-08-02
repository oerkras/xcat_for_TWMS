# 经典版一键启动器

## 用法

1. 运行 `bin\MscLauncher.exe`
2. 「账号串」粘贴：`邮箱-密码-其它…`（连续任意个 `-` 都当分隔，只使用前两项）——会自动保存，下次打开自动填入
3. 点 **一键启动游戏**

## 本地文件

| 路径 | 说明 |
|---|---|
| `bin\account.txt`（与 exe 同级） | 账号串（明文，勿外传；映射盘时可宿主/虚拟机共用） |
| `%LocalAppData%\xcat_msc\webview_profile` | WebView 登录态（**必须本机盘**，不跟 exe 走映射盘） |
| Edge WebView2 Runtime | 缺失时启动会弹窗引导下载；安装后需**重启本程序** |
| `bin\launcher.log` | 运行日志 |
