# 阅读划词翻译（Windows 0.9.4 社区修改版）

## 使用

设置 → AI 辅助有独立的「发送目标语言」与「阅读目标语言（鼠标划词）」。发送语言保持用户现值；新增阅读语言默认简体中文，支持现有 185 种语言及自定义名称。只有简译 / 繁译安装选区监听；简 / 繁没有鼠标、键盘或前台窗口监听，不读取选区，不提交阅读翻译任务。

在译模式用鼠标拖选或双击选择文本，松开鼠标后稳定 350 ms，再读取选区。缓存先于 API，译文在鼠标附近的无激活轻量 Win32 浮窗显示，不上屏、不改变原应用选中文字。浮窗提供复制译文、关闭及滚轮浏览长译文；开始新选择、点其他位置、Esc 或切换前台窗口关闭旧浮窗。复制按钮是唯一有意写入剪贴板的动作。

发送翻译保持不变：Space / 数字选择只累计缓冲，Enter 一次上屏中文，Ctrl+Enter 才查缓存并请求发送翻译，输入、候选变化和等待都不触发发送翻译。

## 捕获与生命周期

先查鼠标释放位置的 UI Automation 元素，再向父级查 TextPattern.GetSelection，读取唯一选区并检查 GetBoundingRectangles 是否靠近鼠标。只读取选区，不读取全篇文档。多段不连续选区被跳过。

UI Automation 工作在独立 MTA 捕获线程，使用 UIAutomation2 的连接/事务超时；鼠标回调仅排队，不做 COM、数据库或网络操作。UIA 捕获、浮窗与阅读翻译 Worker 分离，不与发送 Worker 争用任务。普通模式线程休眠，不轮询选区。

UIA TextPattern 不可用时，已通过安全检查的经典 Unicode Edit 控件可使用 EM_GETSEL + 有界 WM_GETTEXT 获取所选片段，SendMessageTimeout 防止窗口无响应。不会模拟 Ctrl+C，不需要剪贴板获取 fallback，也不会读取 Chrome Cookie。

当前元素及祖先都检查 UIA IsPassword；属性无法可靠取得时跳过。原生 Edit/RichEdit 的 ES_PASSWORD 也拒绝。安全桌面不在当前监听桌面范围内。未正确暴露安全属性的第三方控件仍依赖其 accessibility provider 的正确实现，不能保证识别所有自绘安全输入框。

模式/配置发布使用独立线程安全快照和原子纪元。退出译模式立刻禁止捕获与网络任务，通知窗口线程取消 Worker、关闭浮窗和卸载所有 hooks；在途网络通过 cancellation callback 中止，旧纪元结果被丢弃。

## 去重与缓存

只在鼠标松开后调度，拖动和 mouse move 不调度。350 ms debounce 合并同一动作的重复结束事件。相同缓存身份在请求未完成期间复用在途任务，不并发；完成后两秒内直接复用结果，两秒后先查 SQLite。新选区覆盖旧任务，失败允许重新选择重试。空白、纯数字、纯标点、单字母以及不合法 UTF-8 被过滤。

阅读原文上限为 8192 UTF-16 单元且最多 16384 UTF-8 字节，不悄悄截断超限选区。浮窗源文本预览最多 512 个 UTF-16 单元，完整译文可以滚轮浏览和复制。只读浮窗不抢焦点。

复用 `%LOCALAPPDATA%\metasequoiaime\ai-translations.db`，不创建新的正式翻译数据库，不清空旧记录或改变同步格式。缓存沿用原文、目标语言、源文字形式与必要上下文。源文本完全相同、目标相同且上下文为空时，发送/阅读可跨源文字形式复用已有记录；优先保留已有人工修改规则。命中计数写入实际命中记录的身份。API 成功立即持久化，输出预算仍为 4096，Google 同步沿用现有流程。

阅读 prompt 自动识别源语言，只翻译所选原文；文本中的指令不应被执行。测试程序、日志和源码不包含真实用户凭证；真实 API 验证只将已配置凭证读入内存，并使用固定的非私人测试句。

## 验证与限制

- Server CTest 5/5 目标通过；翻译/同步子测试 29 项，零失败。
- 设置页 108 项通过；TypeScript/Vite 构建通过。
- 隔离真实 Win32 控件 + UI Automation + 模拟网络的 16 项验证通过：普通模式无 hooks/请求、译模式启用、拖动零请求、英文选择、原文不改、去重、SQLite 命中零请求、Tagalog、长段、数字标点过滤、密码拒绝、异步、在途去重、模式退出取消/卸载、普通模式持续静默、失败不改原文。
- 真实 API：English → 简体中文、Tagalog → 简体中文返回有效中文译文；重复原文缓存复用，两次 HTTP 请求。测试缓存独立，运行结束删除；现有 API 配置及正式缓存不改。
- 真实隔离 Server / 命名管道发送回归验证简译及繁译；普通模式回归零 API，发送缓冲逻辑未改。
- Chrome 原生鼠标实测被电脑操作工具中止：无法可靠确认当前浏览器网址。没有绕过工具限制。Chrome、Telegram、微信、Discord、Office、真正的记事本应用目前均不能列为实测通过；通过的是独立经典 Windows 文本控件测试。
- 第三方软件若不暴露 TextPattern/选区，且不是可安全读取的经典 Edit，当前会跳过。没有危险的全局 Copy fallback，不声称覆盖所有 Qt/Electron/Office 控件。
- 窗口代码处理 DPI 变化、字体与尺寸缩放以及工作区边界；100/125/150/200% 和跨屏的人工视觉验收尚未完成。

手工验收时先切到简译 / 繁译，在上述应用选择普通英文或他加禄语段落，确认显示译文且原文不变；同样段落重复选择应命中缓存。切回简 / 繁，选择文本应无浮窗。再检查 Space 累积、Enter 中文与 Ctrl+Enter 发送译文。

## 回滚

本机升级仅替换 Server、设置程序、设置静态资源与出厂模板，TSF DLL 未修改。保留旧二进制、模板、私密配置和 SQLite 在线备份在用户数据下的 backups/reading-translation-*。使用相应 Restore-previous.ps1 可回滚程序；不覆盖当前 API 配置，不回退当前翻译数据库。私密备份不应上传仓库。

## 本次修改文件

- `docs/ai-reading-translation.md`
- `installer/default_config/config.default.toml`
- `server/CMakeLists.txt`
- `server/assets/config/config.toml`
- `server/src/ai/ai_translation.cpp`
- `server/src/ai/ai_translation.h`
- `server/src/ai/ai_translation_request.cpp`
- `server/src/ai/reading_translation.cpp`
- `server/src/ai/reading_translation.h`
- `server/src/config/ime_config.cpp`
- `server/src/config/ime_config.h`
- `server/src/config/ime_config_services.cpp`
- `server/src/main.cpp`
- `server/src/settings/settings_app.cpp`
- `server/src/webview2/windows_webview2_settings.cpp`
- `server/tests/CMakeLists.txt`
- `server/tests/src/ai_translation_live_probe.cpp`
- `server/tests/src/reading_selection_probe.cpp`
- `server/tests/src/test_ai_translation.cpp`
- `ui-html/webview2/settings/ime-settings/src/modules/ai-settings.test.ts`
- `ui-html/webview2/settings/ime-settings/src/modules/ai-settings.ts`
- `ui-html/webview2/settings/ime-settings/src/partials/ai-settings.html`

## 2026-10-05 卡顿与崩溃修复

- 将全局鼠标/键盘 hooks 放到专用消息线程，回调仅投递事件，禁止窗口查询、UIA、绘制和加锁；mouse move 不产生任务。
- UIA 按需初始化；事务超时 500 ms、连接超时 1000 ms，捕获流程检查 2500 ms 时间预算及模式取消。安全检查到选中应用窗口为止，不遍历无关桌面祖先。
- Windows 崩溃事件偏移 0x84239 经本地 PDB 定位到 PatternSelection：不支持 TextPattern 的控件可能返回成功但接口为空。为 pattern/range 补空指针检查。
- 实际拖选结束显示“正在读取选区”；安全读取失败/超时显示明确提示。无效数字标点不调用 API。读取失败不启用模拟 Copy。
- 新增 unsupported TextPattern 回归与浮窗线程阻塞时鼠标回调延迟回归。隔离原生控件测试 18 项通过，CTest 5/5 通过。第三方应用实际兼容仍需验收，不能据此宣称全部支持。

## 2026-10-05 主动右键阅读翻译

已取消自动划选翻译。全局鼠标回调仅处理右键松开，移动/左键拖选/左键松开不投递选区任务。右键只显示非激活、8 秒自动关闭的轻量按钮，不读取选区、不查缓存、不请求 AI；用户点击「翻译选中文字」后才异步 UIA 捕获。原生菜单未被修改；控件不支持或右键清空选区时安全失败。

Chrome/Edge/Brave/Vivaldi 网页走独立 MV3 扩展：只有用户点击原生右键翻译菜单才注入浮窗。没有常驻网页内容脚本、网页选择监听或 HTTP 端口。浏览器由 Native Messaging 启动按需 Host，再使用当前 Windows 用户 SID 隔离、拒绝远程访问的命名管道连接 Server；host 仅允许固定扩展 origin。Host 不读取 API 配置。消息大小限制 64 KiB，阅读原文仍最多 16 KiB UTF-8。Server 验证模式/格式/文本，异步 Worker 复用现有 SQLite；配置/模式变化取消请求。一个网页同时只允许一个任务，桥接串行并设置超时。IPC 契约在 engine/contracts/browser。

真正的浏览器按进程名区分，因此 Discord 等 Electron 桌面软件不会仅因 Chrome_WidgetWin 类名被跳过。第三方软件最终读取能力仍依赖其 accessibility provider，不声称微信/Telegram/Discord 等全部实测通过。

验证：隔离原生 UIA/桌面按钮/桥接回归通过，确认拖选和右键入口 0 API、普通模式拒绝桥接、网页/桌面共享缓存且命中 0 API、网络失败保留原应用。发送命名管道回归通过，Ctrl+Enter 唯一触发规则不变；CTest 5/5，设置页 108 项测试及 TypeScript/Vite 编译通过；扩展文本过滤与显式点击/在途去重的 Node 测试通过。安装后的真实 Host/Server 消息往返、无效文本拒绝、非授权 origin、超长消息拒绝通过；未消耗真实翻译 API。

浏览器扩展仍需用户从 chrome://extensions / edge://extensions 开发者模式加载 browser-extension/msime-reading，未声称浏览器页面端到端验收完成。安装与使用详见 ../browser-extension/README.md。

### 主动触发版本修改文件

- `browser-extension/README.md`
- `browser-extension/install-native-host.ps1`
- `browser-extension/msime-reading/background.js`
- `browser-extension/msime-reading/background.test.js`
- `browser-extension/msime-reading/manifest.json`
- `browser-extension/msime-reading/package.json`
- `browser-extension/msime-reading/selection.js`
- `browser-extension/msime-reading/selection.test.js`
- `docs/ai-reading-translation.md`
- `engine/contracts/browser/reading.h`
- `engine/contracts/browser/reading_extension.h`
- `server/CMakeLists.txt`
- `server/src/ai/reading_translation.cpp`
- `server/src/ai/reading_translation.h`
- `server/src/browser/native_host.cpp`
- `server/src/browser/reading_bridge.cpp`
- `server/src/browser/reading_bridge.h`
- `server/src/browser/reading_transport.h`
- `server/tests/CMakeLists.txt`
- `server/tests/src/reading_selection_probe.cpp`
- `tests/browser-reading/test-native-host.py`
- `ui-html/webview2/settings/ime-settings/src/partials/ai-settings.html`
- `browser-extension/msime-reading/LICENSE` 与 `NOTICE.txt`（扩展独立分发的原作者归属及许可）。
