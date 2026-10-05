# 水杉网页阅读翻译（Chrome / Edge）

基于 [MetasequoiaIME 原项目](https://github.com/metasequoiaime/msime) 的扩展功能，原输入法由 MetasequoiaIME 团队和贡献者开发。本扩展沿用仓库 GPL-3.0 许可。

## 安装

1. 安装本分支编译的 MSIME（包含 `MetasequoiaImeReadingHost.exe`）。
2. 运行 `install-native-host.ps1`，将本机桥接注册到当前 Windows 用户。
3. Chrome 打开 `chrome://extensions`，Edge 打开 `edge://extensions`，开启「开发者模式」，点击「加载已解压的扩展程序」，选择 `msime-reading` 文件夹。
4. 将输入法切到「简译 / 繁译」。网页选中文字 → 右键「翻译选中文字（水杉）」→ 网页右下角显示结果。

浏览器不需要重新输入 OpenAI 密钥。阅读目标语言沿用 MSIME 设置。普通简 / 繁会拒绝翻译请求。

## 性能与隐私

没有常驻网页内容脚本、选区轮询或鼠标监听。只有主动点击右键翻译时才注入小浮窗并调用本机桥接。没有 HTTP 端口，不读取 Cookie，不在扩展保存密钥。内容仅传给本机 MSIME，缓存未命中时由 MSIME 使用现有配置请求 AI。网页输入密码框不翻译；数字、标点、极短文本和超长内容被过滤。

一个网页同时只允许一个请求；本机桥接串行处理请求。相同原文/目标语言优先复用本地 `ai-translations.db`，也能复用桌面阅读缓存。模式关闭/配置变化会取消在途请求。需要 Windows 本地 MSIME 后台运行。

浏览器内部页、商店页、部分 PDF/沙盒框架等禁止注入的页面不能显示浮窗；图标会显示 `!`，鼠标悬停可查看提示。复制译文需要网页允许剪贴板写入；失败会显示提示，原网页内容不改变。

## 桌面软件

选中文字 → 在选区上右键 → 点击 MSIME「翻译选中文字」小按钮。原软件菜单保留，小按钮并非修改原菜单。点击之前不读取 UI Automation、剪贴板、缓存或调用 AI。按钮 8 秒未使用自动关闭；Esc 关闭。

第三方软件必须保留选区并通过 UI Automation / 安全的经典 Edit 暴露它。若原软件右键就清空选区，或不提供选区接口，会显示无法读取提示；不会模拟 Copy，也不会强行注入软件菜单。网页由扩展直接取得用户右键选中的内容，不依赖 UIA。

发送翻译不变：Space / 数字键累积中文，Enter 上屏中文，只有 Ctrl+Enter 翻译并上屏。

## 卸载扩展桥接

在浏览器删除扩展，并删除当前用户 `Software\Google\Chrome\NativeMessagingHosts\org.metasequoiaime.reading` 和 `Software\Microsoft\Edge\NativeMessagingHosts\org.metasequoiaime.reading` 注册项（如使用 Brave/Vivaldi 则删除对应项）。不删除 API 配置或本地翻译数据库。
