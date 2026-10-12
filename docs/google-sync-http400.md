# Google 同步 HTTP 400 排查与修复

Google OAuth 令牌刷新接口可能返回 `invalid_grant` 和 `Token has been expired or revoked.`。这表示授权过期或撤销，而不是 Sheets 表格错误。Testing 状态的外部 OAuth 应用请求 Sheets 离线授权时，刷新令牌通常七天过期。设置中重新授权可恢复；长期使用应按 Google 控制台要求切换正式发布并重新授权，不新建表格。

错误界面区分 OAuth 刷新/授权交换、Sheets 表格信息读取、单元格读取和增量写入，解析 Google 错误代码、说明及原因。只显示经过凭据脱敏的错误字段，不输出请求体、授权码、客户端密钥、Access Token 或 Refresh Token。

阅读翻译的 source_character_set=auto 表示自动识别源语言，与 simplified/traditional 一样是合法缓存身份。Google 同步必须保留 auto，并维持 record_id，不应要求用户改为 simplified，否则会造成身份改变或重复记录。

独立测试覆盖上传、下载、重复同步不写入、人工译文保护和 auto 记录往返。真实云端验证只按现有记录增量合并，不修改人工译文来做测试。本地缓存无需清空，OAuth 客户端和表格无需重新创建。

官方说明：https://developers.google.com/identity/protocols/oauth2
