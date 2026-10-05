# AI 翻译句子缓冲区（Windows 0.9.4 修复版）

简译 / 繁译模式下，中文从一开始就留在 Server 自己的缓冲区，不提前写入外部应用，不用模拟 Backspace 删除已经上屏的中文。

## 操作

- 拼音照常输入；Space / 数字选择将中文词语累计到「待翻译」显示区。
- 分段选择、第一候选变化、整句拼音、标点、缓冲变化和等待，都不会提交翻译任务。
- Enter：取当前已确定的中文候选补入缓冲，一次提交完整中文；不查询翻译 API。
- Ctrl+Enter：补入当前候选，后台先查本地 SQLite 缓存，命中直接用；未命中才调用配置中的 AI，保存成功译文，再一次提交译文。
- 收到 TSF 成功插入回执后才清空缓冲。API 或插入失败保留原文，允许重试 / Enter 提交。
- 连续 Ctrl+Enter 不会产生同一句并发请求。
- Backspace：先编辑尚未选定的拼音；没有拼音时删除缓冲末尾一个 Unicode 字符。
- Esc：先取消当前拼音；当前没有拼音则清空缓冲。
- 常用中文标点加入缓冲。简 / 繁模式保留正常中文上屏路径。
- AI 中文候选与 AI 翻译仍是独立开关；禁用自动翻译不等于关闭 AI 中文候选。

## 关键实现

唯一翻译提交点位于 server/src/ipc/event_listener_keys.cpp 的 Ctrl+Enter 分支。删除了候选刷新时的自动 AI 翻译提交与旧候选自动译文状态。

协议新增可选 TranslationBuffer 能力、缓冲状态回复、分帧完整文本及插入回执。未协商新能力的 DLL 继续旧中文输入路径，需要重开应用以加载升级后的 DLL。

未提交词语不写入「已上屏前文」，避免缓存身份被分段输入自身污染；已有针对短句歧义的上下文缓存规则保留。

候选窗口使用已有 Direct2D / WebView 宿主，增加只读待翻译行。SQLite 本地缓存仍在 %LOCALAPPDATA%\metasequoiaime\ai-translations.db，原同步机制保持。

## 验证结果

- Server CTest：5 / 5 测试目标通过（含 AI 翻译、缓存、失败、OpenAI 参数兼容、原 Server 测试）。
- TSF x64 / x86 CTest：各 9 / 9 目标通过。
- 设置页：107 / 107 测试、TypeScript 检查与 Vite 构建通过。
- 真实独立 Server + 原生命名管道 + 本地模拟 HTTP：简译 / 繁译均通过。首次完整句调用一次，相同句再次缓存命中零请求；分段 / 数字 / 标点 / 等待 / Enter 零翻译请求；重复 Ctrl+Enter 去重、HTTP 失败保留 / 重试、Backspace / Esc、220 字中文原文及 450 字译文分帧上屏通过。
- 普通简 / 繁真实 Server 测试：Space 立即返回中文、翻译请求为零。
- 用独立管道 / 用户数据目录和虚拟凭证测试，不调用用户 OpenAI，不读 / 输出用户密钥。
- 完整手工文本控件提交行为及微信 / Telegram / 浏览器逐应用验证尚未完成，不能据此声称这些应用全部通过。

## 当前限制与实际测试

待翻译内容属于当前输入焦点会话。切换焦点、关闭翻译、停止输入法会取消未提交内容，跨应用前先 Enter / Ctrl+Enter 完成句子。缓冲尾部可 Backspace；不提供已选中文字的任意中间光标编辑。

源文本沿用现有翻译模块 4096 UTF-8 字节限制；一次上屏分帧协议总负载最多 2048 UTF-16 单元（含内部回执编号）。超限会保留原文并失败，不会悄悄截断或清空。

重开要测试的应用，切到水杉的中文状态与「简译」。依次输入 wojuede + Space、wozhegeren + Space、haishitinghaode + Space，应只在候选附近看到累计中文，应用里仍无中文。按 Enter 验证中文完整提交；另一次 Ctrl+Enter 验证仅提交译文，再输入相同句测试缓存。网络失败时确认原文仍在。

## 自动化复测

构建 tests/pipe-probe 的 msime-translation-buffer-probe，并设置 MSIME_PROBE_ASSET_DIR 指向独立字典资源目录（只复制指定字典，不复制配置或凭证）。从仓库运行：

```powershell
python tests/pipe-probe/run-translation-buffer.py
$env:MSIME_PROBE_TRAD = '1'
python tests/pipe-probe/run-translation-buffer.py
$env:MSIME_PROBE_NORMAL = '1'
python tests/pipe-probe/run-translation-buffer.py
```

## 修改文件

- `engine/contracts/ipc_negotiation.h`
- `engine/contracts/ipc_test_endpoints.h`
- `engine/contracts/tests/windows_ipc_contract.cpp`
- `engine/contracts/windows_ipc.h`
- `server/src/ai/ai_translation.cpp`
- `server/src/ai/translation_sentence_buffer.h`
- `server/src/global/globals.h`
- `server/src/ipc/event_listener.cpp`
- `server/src/ipc/event_listener_candidates.cpp`
- `server/src/ipc/event_listener_internal.h`
- `server/src/ipc/event_listener_keys.cpp`
- `server/src/ipc/event_listener_pipes.cpp`
- `server/src/ipc/event_listener_selection.cpp`
- `server/src/ipc/event_listener_worker.cpp`
- `server/src/ipc/ipc.cpp`
- `server/src/ipc/ipc.h`
- `server/src/main.cpp`
- `server/src/webview2/windows_webview2_candidate.cpp`
- `server/src/window/candidate_presenter.cpp`
- `server/src/window/ime_windows_candidate.cpp`
- `server/src/window/ime_windows_candidate_show.cpp`
- `server/tests/src/test_ai_translation.cpp`
- `server/tests/src/test_ipc_protocol_constants.cpp`
- `tests/pipe-probe/CMakeLists.txt`
- `tests/pipe-probe/run-translation-buffer.py`
- `tests/pipe-probe/translation_buffer.cpp`
- `ui-html/webview2/settings/ime-settings/src/partials/ai-settings.html`
- `windows/src/IME/MetasequoiaIME.h`
- `windows/src/IME/MetasequoiaIME_IpcWorker.cpp`
- `windows/src/IME/MetasequoiaIME_WindowProc.cpp`
- `windows/src/IPC/Ipc.cpp`
- `windows/src/IPC/Ipc.h`
- `windows/src/Key/KeyEventSink.cpp`
- `windows/src/Key/KeyHandler.cpp`
- `windows/src/Key/KeyHandlerEditSession.cpp`
- `windows/src/UI/CandidateListUIPresenter.cpp`

