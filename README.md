# MSIME AI 翻译社区扩展版

**原项目与原作者： [MetasequoiaIME（水杉输入法）团队及贡献者](https://github.com/metasequoiaime/msime-windows)。**

本仓在原有水杉输入法基础上增加翻译功能。原输入法、引擎、界面及基础代码的归属保留给原作者；本分支仅维护新增功能和兼容修复。沿用 [GPL-3.0 许可证](LICENSE)，保留原作者与第三方许可。

## 功能

- 简 / 繁维持普通中文输入；简译 / 繁译启用跨语言功能。
- **发送翻译：** Space / 数字选词累积中文，Enter 提交中文；只有 Ctrl+Enter 才查缓存、翻译并一次性提交译文。分段输入过程不发翻译 API 请求，失败保留原文。
- **阅读翻译：** 桌面选中文字后右键点击轻量翻译入口；Chrome / Edge 使用独立扩展的原生右键菜单。已取消自动划选翻译，不在拖选过程中读取或请求 AI。
- 发送目标语言与阅读目标语言独立；185 个中文名称的语言选项及自定义语言。语言质量取决于所用模型。
- 本地 `ai-translations.db` 优先，成功译文自动保存；网页与桌面复用缓存，重复内容命中后不调用 API。
- 可选 Google Sheets 每日双向增量同步、立即同步、人工译文优先；每位用户配置自己的表格与 OAuth 凭据。
- OpenAI 新模型参数兼容，保留其他兼容提供商的原有参数行为。

## 使用与构建

这是**源码分享版本**。本仓没有打包本机安装器或上传个人数据，使用者需要按文档自行编译 Windows 版本。

1. 阅读 [完整使用说明](使用说明.md) 与 [官方 Windows 构建说明](server/AGENTS.md)。
2. 在自己的 MSIME 设置中填写自己的 API 配置。
3. 网页右键翻译按照 [浏览器扩展安装说明](browser-extension/README.md) 安装本机 Host 并加载扩展。
4. Google 同步可选，默认关闭。参考 `google-sync.example.json` 配置自己的表格。

本机验证包含发送缓冲回归、选区/桥接隔离测试、本地缓存与 API 去重、已有 Server 测试、设置页测试。用户已完成本机试用；不同桌面软件的选区接口仍有兼容限制，详见 [阅读翻译实现及限制](docs/ai-reading-translation.md)。

## 隐私与分享

本仓没有私人 Git 历史、个人配置、OAuth 客户端文件、访问令牌、翻译库、用户词库、日志、备份或签名私钥。扩展 `manifest.json` 的 `key` 是公开的扩展身份公钥，并非 API Key 或个人凭据。

不要提交自己安装后的配置与缓存。阅读消息的原文和译文会保存本地；开启 Google 同步后也会进入你自己配置的表格。

## 上游归属

- 原项目：[MetasequoiaIME / msime-windows](https://github.com/metasequoiaime/msime-windows)
- 上游官网：[msime.app](https://msime.app)
- [上游 README](README.upstream.md) / [上游英文说明](README.en.md)

感谢原作者团队与所有贡献者。
