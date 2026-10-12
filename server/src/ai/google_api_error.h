#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <vector>
namespace TranslationSync
{
inline std::string GoogleApiError(long http, const std::string &stage, const nlohmann::json &response,
                                  std::vector<std::string> sensitive)
{
    auto string = [](const nlohmann::json &j, const char *key) {
        return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string{};
    };
    std::string code, message, reason;
    if (response.is_object() && response.contains("error"))
    {
        const auto &error = response["error"];
        if (error.is_string())
        {
            code = error.get<std::string>();
            message = string(response, "error_description");
        }
        else if (error.is_object())
        {
            code = string(error, "status");
            message = string(error, "message");
            if (error.contains("errors") && error["errors"].is_array())
                for (const auto &item : error["errors"])
                    if (reason.empty())
                        reason = string(item, "reason");
            if (error.contains("details") && error["details"].is_array())
                for (const auto &item : error["details"])
                    if (reason.empty())
                        reason = string(item, "reason");
        }
    }
    std::sort(sensitive.begin(), sensitive.end(), [](const auto &a, const auto &b) { return a.size() > b.size(); });
    auto clean = [&](std::string s) {
        for (const auto &secret : sensitive)
            if (!secret.empty())
                for (size_t pos = 0; (pos = s.find(secret, pos)) != std::string::npos; pos += 10)
                    s.replace(pos, secret.size(), "[REDACTED]");
        for (auto &c : s)
            if (static_cast<unsigned char>(c) < 32 || c == 127)
                c = ' ';
        if (s.size() > 600)
        {
            size_t end = 600;
            while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80)
                --end;
            s.resize(end);
        }
        return s;
    };
    auto out = "Google " + stage + "失败（HTTP " + std::to_string(http) + "）";
    if (!code.empty())
        out += "：" + clean(code);
    if (!reason.empty())
        out += " / " + clean(reason);
    if (!message.empty())
        out += "；" + clean(message);
    if (code == "invalid_grant")
        out += "。授权已过期或撤销，请重新授权 Google 账号；Testing 状态通常七天过期，长期使用请检查 Google Cloud "
               "发布状态。";
    else if (code == "invalid_client")
        out += "。请检查桌面 OAuth 客户端并重新导入客户端 JSON。";
    else if (reason == "SERVICE_DISABLED" || reason == "accessNotConfigured")
        out += "。请在对应 Google Cloud 项目启用 Google Sheets API。";
    else if (http == 401)
        out += "。请重新授权 Google 账号。";
    else if (http == 403)
        out += "。请检查 Sheets 授权范围、API 是否启用，以及账号对指定表格的权限。";
    else if (http == 404)
        out += "。请检查指定表格是否存在及账号访问权限。";
    return out;
}
} // namespace TranslationSync
