# MSIME AI 翻译使用说明（干净源码分享版）

此包基于 Windows 0.9.4，增加 AI 长句翻译、本地缓存、Google 每日双向同步及 OpenAI 新模型输出参数兼容。它是社区修改版源码包，不是官方安装包。包内保留上游 LICENSE、作者信息和第三方许可；没有原开发机的 Git 历史、个人账号配置、密钥或翻译内容。

## 目标语言与输入状态

设置 → AI 辅助 → 发送目标语言，可选择 185 个中文名称的语言选项。默认英语；菲律宾语/他加禄语、日语、韩语、简体、繁体等可直接选。列表之外选择「其他语言（自定义）」，填写语言名称或代码，例如粤语、宿务语、ceb、sr-Latn。支持程度及译文质量取决于模型，不能保证所有语言效果相同。

状态栏四种状态：简、繁、简译、繁译。简繁与翻译开关独立，AI 联想开关也独立。简译 / 繁译使用翻译句子缓冲区，不发送原始拼音。Space / 数字将选定中文累计到「待翻译」，不提交给应用，不调用翻译 API。Enter 一次上屏完整中文；仅主动 Ctrl+Enter 才先查缓存、未命中请求 AI，再一次上屏译文。Backspace 先编辑拼音、无拼音时删除缓冲末尾；Esc 先取消当前拼音、再清空缓冲。失败保留原文供重试。可以自行配置切换 AI 翻译快捷键。

## 填写自己的 AI 配置

在设置中选择提供商、模型，填写自己的 API Token 和 Endpoint，点击测试配置。不要把 Token 填入仓库中的模板或上传到 GitHub。长句翻译复用这些配置。OpenAI 需要 max_completion_tokens 的新模型使用对应参数，其他兼容模型保留原参数；翻译请求输出预算为 4096。

有效译文生成后立即保存本地，不需要上屏才保存。相同条件的原文先查本地，有缓存不再请求 AI、不会重复新增，只累加命中数；目标语言、简繁或必要的上下文不同则分别保存。翻译关闭时不发翻译 API 请求。

## Google 云同步（可选）

不使用云同步也能完成本地翻译。使用云同步时：

1. 创建或选择属于你自己的 Google 表格，使用其第一个工作表（gid=0）。把本包「Google翻译库表头.tsv」的唯一一行复制到 A1。不要改表头顺序。
2. 在 `%LOCALAPPDATA%\metasequoiaime` 下创建 `google-sync.json`，内容参考 `google-sync.example.json`，将 spreadsheet_id 填成自己表格网址中 `/d/` 和 `/edit` 之间的 ID。这个文件只保存表格 ID，不保存令牌。切换表格后重新导入客户端并授权。
3. 在自己的 Google Cloud 项目中启用 Google Sheets API。配置 OAuth 受众；测试模式把自己的账号加入测试用户。创建「桌面应用」OAuth 客户端并下载 JSON。
4. 设置 → AI 辅助 → 翻译库 Google 同步：导入自己的客户端 JSON，再授权自己的 Google 账号，最后点击立即同步。

客户端与离线令牌保存在当前 Windows 用户的凭据管理器，按表格 ID 隔离；不读取 Chrome Cookie。OAuth 通过默认浏览器、本机随机端口、PKCE/state 完成。Google 授权请求涉及 Sheets 权限，但此实现只操作配置的表格，不能把 scope 自身限制到单个表格。

每天成功同步一次；未运行时下次启动补同步，失败后每小时重试。关闭每日同步后可以手动立即同步。Google 不参与输入时的查询。同步只追加新行或修改变化单元格，优先保护人工译文，原冲突译文保存在本地冲突表。当前不传播删除：清空本地不删除云端，下次可能重新导入。

人工新增至少填 source_text、target_language、translated_text；source 可填 user，时间和记录 ID 可留空。已有记录直接修改 translated_text 即可。目标语言可用语言代码或名称；上下文、简繁字段用于保持缓存身份。修改原文、语言、简繁、上下文后应清空该行旧 record_id。

外部受众的 Testing 离线授权通常七天过期。长期使用按 Google 控制台要求切换 In production 后重新授权。

官方 OAuth 说明：https://developers.google.com/identity/protocols/oauth2/native-app
官方受众说明：https://support.google.com/cloud/answer/15549945

## 安全分享

只上传这个干净源码目录。不要上传自己的 `%LOCALAPPDATA%\metasequoiaime`、Downloads 客户端 JSON、Credential Manager 导出、API Token、翻译数据库、日志、备份和代码签名私钥。此包没有 Git 历史；上传前重新初始化自己的 Git 仓库。不要把原私人开发仓库的历史推到公开仓库。

## 编译与安装

源码包已包含两个 vendor 子模块的已提交源文件，无需从原私人开发机复制。需要 Windows 10/11、Visual Studio C++ 工具链、Windows SDK、CMake、vcpkg、Boost、Node.js/pnpm。按仓库 server/AGENTS.md、scripts/lcompile-release.ps1 和 installer/README.md 编译。根据自己安装的 VS 版本调整 preset 的 generator。

设置页在 ui-html/webview2/settings/ime-settings，执行 pnpm install --ignore-workspace，然后 pnpm exec tsc、pnpm exec vite build。契约检查可运行 python ui-html/scripts/sync-contracts.py --check。

完整安装器还需要编译 TSF DLL、按 product-lock.json 下载词库，并按 installer/README.md 打包。公开发布需使用发布者自己的代码签名证书及发布流程；此包没有原开发机的签名私钥，也没有打包用户数据。不应将源码包描述成已经可直接安装的 Release。

## 验收

重开输入应用并切到中文状态和简译。依次输入 wojuede + Space、wozhegeren + Space、haishitinghaode + Space，应在待翻译区累计中文，应用尚无中文。Enter 上屏原文；另一次 Ctrl+Enter 才翻译上屏。重复相同句确认命中缓存；更换目标语言确认分别缓存。配置自己的 Google 表格后立即同步，修改云端译文再同步，确认同一输入使用人工译文。断网后中文输入与已缓存译文应继续工作。

缓冲属于当前输入焦点：失焦、关闭翻译或输入法退出会取消未提交内容，切换前先完成当前句子。源文本最多 4096 UTF-8 字节，上屏协议总负载最多 2048 UTF-16 单元。新增功能与测试详情见 docs/ai-translation-sentence-buffer.md。


## 阅读对方消息的右键翻译

阅读语言在设置 → AI 辅助 → 阅读目标语言单独设置，默认简体中文。简 / 繁无阅读翻译；简译 / 繁译才允许翻译。

桌面软件：选中文字 → 右键 → 点击旁边的「翻译选中文字」。右键本身不读取文字、不调用 API；实际点击才异步读取、查本地缓存、未命中请求 AI，并用轻量浮窗显示。部分软件右键会清空选区或不提供安全的选区接口，此时提示无法读取，不模拟 Copy。

网页：参考 browser-extension/README.md 安装本机桥接，在 chrome://extensions / edge://extensions 加载 browser-extension/msime-reading。选中文字后使用网页右键菜单「翻译选中文字（水杉）」。扩展不保存 API Token，也没有常驻选区监听。

网页和桌面阅读翻译共用本地翻译库。有效结果生成后立即保存，不需复制才保存；相同原文、目标语言与必要上下文命中缓存时不再请求 AI。开启 Google 同步后，阅读翻译也会进入你自己配置的表格。

最新回归与兼容限制见 docs/ai-reading-translation.md。源代码并不保证第三方软件全部支持选区读取。
