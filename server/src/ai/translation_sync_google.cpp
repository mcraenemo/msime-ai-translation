#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <wincred.h>
#include <commdlg.h>
#include <shellapi.h>
#include <bcrypt.h>
#include "translation_sync.h"
#include "ai_translation_language.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace TranslationSync
{
namespace
{
using J = nlohmann::json;
std::wstring CredentialTarget()
{
    const auto id = SpreadsheetId();
    return std::wstring(L"MetasequoiaIME/GoogleSheets/") + std::wstring(id.begin(), id.end());
}
const std::vector<std::string> Headers = {
    "source_text",          "target_language", "translated_text", "source",   "updated_at", "hit_count",
    "source_character_set", "context_key",     "record_id",       "provider", "model"};
J Credentials()
{
    if (SpreadsheetId().empty())
        return J::object();
    const auto target = CredentialTarget();
    PCREDENTIALW p = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &p))
        return J::object();
    J out = J::parse(std::string(reinterpret_cast<char *>(p->CredentialBlob), p->CredentialBlobSize), nullptr, false);
    CredFree(p);
    return out.is_object() ? out : J::object();
}
void StoreCredential(const J &j)
{
    auto blob = j.dump();
    if (blob.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        throw std::runtime_error("Google 凭据超过 Windows 存储限制");
    auto target = CredentialTarget();
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.data();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    c.CredentialBlobSize = static_cast<DWORD>(blob.size());
    c.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    const bool ok = CredWriteW(&c, 0) != 0;
    SecureZeroMemory(blob.data(), blob.size());
    if (!ok)
        throw std::runtime_error("无法保存 Windows Google 凭据");
}
std::string Encode(const std::string &s)
{
    const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out += c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    return out;
}
std::string Decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            char *end = nullptr;
            auto pair = s.substr(i + 1, 2);
            auto c = strtol(pair.c_str(), &end, 16);
            if (!end || *end)
                throw std::runtime_error("Google 授权回调格式无效");
            out += static_cast<char>(c);
            i += 2;
        }
        else
            out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}
struct Transfer
{
    std::string body;
    const std::atomic<bool> *stop;
};
size_t Receive(char *data, size_t size, size_t count, void *p)
{
    auto &t = *static_cast<Transfer *>(p);
    if (count > 8 * 1024 * 1024 || size > 1 || t.body.size() + count > 8 * 1024 * 1024)
        return 0;
    t.body.append(data, count);
    return count;
}
int Cancel(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto stop = static_cast<const std::atomic<bool> *>(p);
    return stop && *stop ? 1 : 0;
}
J Http(const std::string &url, const std::string &method, const std::string &body, const std::string &bearer,
       const std::atomic<bool> *stop = nullptr, bool form = false)
{
    using C = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    C c(curl_easy_init(), curl_easy_cleanup);
    if (!c)
        throw std::runtime_error("无法初始化 Google 网络请求");
    Transfer transfer{{}, stop};
    curl_easy_setopt(c.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c.get(), CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c.get(), CURLOPT_XFERINFOFUNCTION, Cancel);
    curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, stop);
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, form ? "Content-Type: application/x-www-form-urlencoded"
                                              : "Content-Type: application/json");
    if (!bearer.empty())
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned(headers, curl_slist_free_all);
    curl_easy_setopt(c.get(), CURLOPT_HTTPHEADER, headers);
    if (method != "GET")
    {
        curl_easy_setopt(c.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(c.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    const auto rc = curl_easy_perform(c.get());
    long status = 0;
    curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
    // 不回显 Google 响应、请求、授权码或 Token；仅给出可操作的固定错误。
    if (rc != CURLE_OK)
        throw std::runtime_error("Google 网络失败或超时，本地翻译不受影响");
    if (status == 401)
        throw std::runtime_error("Google 授权失效，请重新授权");
    if (status == 403)
        throw std::runtime_error("Google 拒绝访问：请启用 Sheets API 并使用拥有该表的账号授权");
    if (status == 429)
        throw std::runtime_error("Google 请求限额，请稍后同步");
    if (status < 200 || status >= 300)
        throw std::runtime_error("Google 请求失败（HTTP " + std::to_string(status) +
                                 "），请检查 OAuth 授权和 API 配置");
    auto result = J::parse(transfer.body, nullptr, false);
    if (result.is_discarded())
        throw std::runtime_error("Google 返回无效数据");
    return result;
}
std::string Form(const J &values)
{
    std::string out;
    for (auto i = values.begin(); i != values.end(); ++i)
    {
        if (!out.empty())
            out += '&';
        out += Encode(i.key()) + '=' + Encode(i.value().get<std::string>());
    }
    return out;
}
std::string Base64(const unsigned char *data, size_t count)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned bits = 0;
    int n = 0;
    for (size_t i = 0; i < count; ++i)
    {
        bits = (bits << 8) | data[i];
        n += 8;
        while (n >= 6)
        {
            n -= 6;
            out += alphabet[(bits >> n) & 63];
        }
    }
    if (n)
        out += alphabet[(bits << (6 - n)) & 63];
    return out;
}
std::string Random()
{
    unsigned char bytes[32]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("无法生成 Google 授权随机数");
    return Base64(bytes, 32);
}
std::string Challenge(const std::string &verifier)
{
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(verifier.data())),
                   static_cast<ULONG>(verifier.size()), digest, 32) < 0)
        throw std::runtime_error("无法生成 PKCE");
    return Base64(digest, 32);
}
std::string Token(const std::atomic<bool> &stop)
{
    const auto c = Credentials();
    if (!c.contains("refresh_token"))
        throw std::runtime_error("尚未授权 Google，请先在设置中授权");
    J values = {
        {"client_id", c.at("client_id")}, {"refresh_token", c.at("refresh_token")}, {"grant_type", "refresh_token"}};
    if (c.contains("client_secret"))
        values["client_secret"] = c.at("client_secret");
    auto result = Http("https://oauth2.googleapis.com/token", "POST", Form(values), "", &stop, true);
    if (!result.contains("access_token"))
        throw std::runtime_error("Google 授权已撤销，请重新授权");
    return result.at("access_token").get<std::string>();
}
std::string Cell(const J &row, size_t i)
{
    if (i >= row.size() || row[i].is_null())
        return "";
    if (row[i].is_string())
        return row[i].get<std::string>();
    if (row[i].is_number())
        return row[i].dump();
    throw std::runtime_error("表格存在无效单元格类型");
}
int64_t Number(const std::string &s, bool timestamp)
{
    if (s.empty())
        return timestamp ? static_cast<int64_t>(time(nullptr)) : 0;
    if (s.find_first_not_of("0123456789") == std::string::npos)
    {
        try
        {
            return std::stoll(s);
        }
        catch (...)
        {
            throw std::runtime_error("表格数字超出范围");
        }
    }
    if (timestamp)
    {
        tm t{};
        int year, month, day, hour, minute, second;
        int count = 0;
        if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%dZ%n", &year, &month, &day, &hour, &minute, &second, &count) == 6 &&
            count == static_cast<int>(s.size()))
        {
            t.tm_year = year - 1900;
            t.tm_mon = month - 1;
            t.tm_mday = day;
            t.tm_hour = hour;
            t.tm_min = minute;
            t.tm_sec = second;
            const auto result = _mkgmtime64(&t);
            if (result >= 0)
                return result;
        }
    }
    throw std::runtime_error("表格时间/命中数格式无效：时间请使用 Unix 秒或 UTC ISO 时间，命中数请使用整数");
}
std::string Iso(int64_t v)
{
    time_t ts = v;
    tm t{};
    gmtime_s(&t, &ts);
    char out[32]{};
    strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &t);
    return out;
}
J Cells(const Record &r)
{
    auto cells = r.Json();
    cells[4] = Iso(r.updated);
    return cells;
}
std::string GoogleBase()
{
    const auto id = SpreadsheetId();
    if (id.empty())
        throw std::runtime_error("请先在 google-sync.json 中配置自己的 Google 表格 ID");
    return "https://sheets.googleapis.com/v4/spreadsheets/" + id;
}
std::vector<RemoteRow> Read(const std::string &token, const std::atomic<bool> &stop)
{
    // 固定 gid=0；名称允许用户重命名，绝不创建或选择其他表。
    auto metadata = Http(GoogleBase() + "?fields=sheets(properties(sheetId,title,gridProperties(rowCount)))", "GET", "",
                         token, &stop);
    std::string name;
    int rowCount = 0;
    for (const auto &sheet : metadata.at("sheets"))
        if (sheet.at("properties").at("sheetId") == 0)
        {
            name = sheet.at("properties").at("title");
            rowCount = sheet.at("properties").at("gridProperties").at("rowCount");
        }
    if (name.empty())
        throw std::runtime_error("指定表格的 gid=0 工作表已删除，请恢复工作表");
    std::string escaped;
    for (char c : name)
    {
        escaped += c;
        if (c == '\'')
            escaped += '\'';
    }
    auto get = [&](int start, int end) {
        return Http(GoogleBase() + "/values/" +
                        Encode("'" + escaped + "'!A" + std::to_string(start) + ":K" + std::to_string(end)) +
                        "?valueRenderOption=UNFORMATTED_VALUE",
                    "GET", "", token, &stop)
            .value("values", J::array());
    };
    const auto header = get(1, 1);
    if (header.size() != 1 || header[0] != J(Headers))
        throw std::runtime_error("翻译库表头已改变，请恢复 A1:K1 的原表头后同步");
    std::vector<RemoteRow> rows;
    // 有界分块读取，修改仍只写变化单元格；输入时完全不读取 Google。
    for (int start = 2; start <= rowCount; start += 500)
    {
        const auto values = get(start, std::min(start + 499, rowCount));
        for (size_t i = 0; i < values.size(); ++i)
        {
            auto row = values[i];
            bool empty = true;
            for (const auto &v : row)
                if (v != "" && !v.is_null())
                    empty = false;
            if (empty)
                continue;
            const auto source = Cell(row, 0), language = Cell(row, 1), text = Cell(row, 2);
            if (source.empty() || text.empty() || language.empty())
                throw std::runtime_error("表格存在不完整记录，请补齐原文、目标语言和译文后同步");
            if (source.size() > 4096 || text.size() > 16384 || Cell(row, 7).size() > 8192)
                throw std::runtime_error("表格翻译记录过长");
            if (!AiTranslation::IsValidTargetLanguage(language))
                throw std::runtime_error("表格目标语言应为有效语言代码或名称，不得包含换行或超过 80 字节");
            auto origin = Cell(row, 3);
            if (origin.empty())
                origin = "user";
            if (origin != "ai" && origin != "user")
                throw std::runtime_error("表格来源请使用 ai 或 user");
            auto charset = Cell(row, 6);
            if (charset.empty())
                charset = "simplified";
            if (charset != "simplified" && charset != "traditional")
                throw std::runtime_error("表格简繁字段请使用 simplified 或 traditional");
            Record r{source,
                     language,
                     text,
                     origin,
                     charset,
                     Cell(row, 7),
                     Cell(row, 9),
                     Cell(row, 10),
                     Number(Cell(row, 4), true),
                     Number(Cell(row, 5), false)};
            const auto id = Cell(row, 8);
            if (!id.empty() && id != r.Id())
                throw std::runtime_error("表格记录 ID 与原文/语言/上下文不一致；修改键字段后请清空该行 record_id");
            // 保留原始值以验证并发变更及只写差异单元格。
            while (row.size() < 11)
                row.push_back("");
            rows.push_back({r, start + static_cast<int>(i), row});
        }
    }
    return rows;
}
} // namespace
std::string SpreadsheetId()
{
    // 分享版使用用户本机配置，不内置任何私人表格或项目 ID。
    wchar_t local[32768]{};
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!size || size >= 32768)
        return {};
    std::ifstream stream(std::filesystem::path(local) / L"metasequoiaime" / L"google-sync.json", std::ios::binary);
    if (!stream)
        return {};
    stream.seekg(0, std::ios::end);
    if (stream.tellg() > 4096)
        return {};
    stream.seekg(0);
    const auto config = J::parse(stream, nullptr, false);
    if (!config.is_object() || !config.contains("spreadsheet_id") || !config["spreadsheet_id"].is_string())
        return {};
    const auto id = config["spreadsheet_id"].get<std::string>();
    if (id.size() < 16 || id.size() > 200 ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        return {};
    return id;
}
bool HasClient()
{
    return Credentials().contains("client_id");
}
bool HasAuthorization()
{
    return Credentials().contains("refresh_token");
}
bool ImportClient()
{
    wchar_t path[32768]{};
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.lpstrFilter = L"Google Desktop OAuth JSON\0*.json\0\0";
    open.lpstrFile = path;
    open.nMaxFile = 32768;
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    open.lpstrTitle = L"选择 Google 桌面应用 OAuth 客户端 JSON";
    if (!GetOpenFileNameW(&open))
        return true;
    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    auto size = input.tellg();
    if (size <= 0 || size > 65536)
        throw std::runtime_error("Google 客户端 JSON 文件无效或过大");
    input.seekg(0);
    std::string data(static_cast<size_t>(size), '\0');
    input.read(data.data(), size);
    auto json = J::parse(data, nullptr, false);
    SecureZeroMemory(data.data(), data.size());
    if (!json.is_object() || !json.contains("installed"))
        throw std::runtime_error("请下载应用类型为「桌面应用」的 Google OAuth 客户端 JSON");
    const auto &client = json.at("installed");
    auto id = client.value("client_id", std::string());
    const std::string suffix = ".apps.googleusercontent.com";
    if (id.size() <= suffix.size() || id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0)
        throw std::runtime_error("无效的 Google OAuth Client ID");
    J saved = {{"client_id", id}};
    if (client.contains("client_secret"))
        saved["client_secret"] = client.at("client_secret");
    StoreCredential(saved);
    return true;
}
void Authorize()
{
    if (SpreadsheetId().empty())
        throw std::runtime_error("请先配置 google-sync.json 中的 spreadsheet_id");
    auto credentials = Credentials();
    if (!credentials.contains("client_id"))
        throw std::runtime_error("请先导入 Google 桌面 OAuth 客户端 JSON");
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        throw std::runtime_error("无法启动 Google 本地授权回调");
    struct Cleanup
    {
        ~Cleanup()
        {
            WSACleanup();
        }
    } cleanup;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        throw std::runtime_error("无法创建授权回调");
    struct Socket
    {
        SOCKET s;
        ~Socket()
        {
            if (s != INVALID_SOCKET)
                closesocket(s);
        }
    } owner{listener};
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char *>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(listener, 1))
        throw std::runtime_error("无法绑定本机 Google 授权回调");
    int size = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &size);
    const auto host = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port)),
               redirect = "http://" + host + "/oauth2callback", state = Random(), verifier = Random();
    J query = {{"client_id", credentials.at("client_id")},
               {"redirect_uri", redirect},
               {"response_type", "code"},
               {"scope", "https://www.googleapis.com/auth/spreadsheets"},
               {"state", state},
               {"code_challenge", Challenge(verifier)},
               {"code_challenge_method", "S256"},
               {"access_type", "offline"},
               {"prompt", "consent"}};
    const auto url = "https://accounts.google.com/o/oauth2/v2/auth?" + Form(query);
    std::wstring wide(url.begin(), url.end());
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("无法打开 Google 授权页面，请检查默认浏览器");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    std::string code;
    while (code.empty() && std::chrono::steady_clock::now() < deadline)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{1, 0};
        if (select(0, &set, nullptr, nullptr, &timeout) <= 0)
            continue;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;
        Socket connection{client};
        DWORD receiveTimeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&receiveTimeout), sizeof(receiveTimeout));
        std::string request;
        char buffer[1024];
        while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos)
        {
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0)
                break;
            request.append(buffer, n);
        }
        bool valid = false, denied = false;
        std::map<std::string, std::string> params;
        if (request.rfind("GET /oauth2callback?", 0) == 0 &&
            request.find("\r\nHost: " + host + "\r\n") != std::string::npos)
        {
            const auto begin = request.find('?') + 1, end = request.find(' ', begin);
            std::string q = request.substr(begin, end - begin);
            size_t pos = 0;
            while (pos < q.size())
            {
                auto next = q.find('&', pos);
                if (next == std::string::npos)
                    next = q.size();
                auto eq = q.find('=', pos);
                if (eq < next)
                    params[Decode(q.substr(pos, eq - pos))] = Decode(q.substr(eq + 1, next - eq - 1));
                pos = next + 1;
            }
            valid = params["state"] == state;
            denied = valid && params.count("error");
            if (valid && !denied)
                code = params["code"];
        }
        const std::string response =
            "HTTP/1.1 " + std::string(valid ? "200 OK" : "400 Bad Request") +
            "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" +
            (valid ? "You can close this tab and return to MSIME settings." : "Invalid authorization callback.");
        send(client, response.data(), static_cast<int>(response.size()), 0);
        if (denied)
            throw std::runtime_error("Google 授权被取消，本地翻译继续工作");
    }
    if (code.empty())
        throw std::runtime_error("Google 授权超时，请再次点击授权");
    J values = {{"client_id", credentials.at("client_id")},
                {"code", code},
                {"code_verifier", verifier},
                {"redirect_uri", redirect},
                {"grant_type", "authorization_code"}};
    if (credentials.contains("client_secret"))
        values["client_secret"] = credentials.at("client_secret");
    auto result = Http("https://oauth2.googleapis.com/token", "POST", Form(values), "", nullptr, true);
    if (!result.contains("refresh_token"))
        throw std::runtime_error("Google 未提供离线授权，请重新授权并同意访问表格");
    credentials["refresh_token"] = result.at("refresh_token");
    StoreCredential(credentials);
}
Transport GoogleTransport(const std::atomic<bool> &stop)
{
    auto token = std::make_shared<std::string>(Token(stop));
    return {
        [token, &stop] { return Read(*token, stop); },
        [token, &stop](const std::vector<Edit> &edits) {
            const auto current = Read(*token, stop);
            std::map<int, J> rows;
            int last = 1;
            for (const auto &r : current)
            {
                rows[r.row] = r.raw;
                last = std::max(last, r.row);
            }
            J requests = J::array();
            int count = 0;
            for (const auto &e : edits)
            {
                if (e.row && (!rows.count(e.row) || rows.at(e.row) != e.before))
                    throw std::runtime_error("表格在同步期间被修改，已停止写入；请再次立即同步");
                if (!e.row && std::any_of(current.begin(), current.end(),
                                          [&](const RemoteRow &r) { return r.record.Id() == e.record.Id(); }))
                    throw std::runtime_error("云端已新增同一记录，请再次同步后合并");
                const int row = e.row ? e.row : ++last;
                const auto after = Cells(e.record);
                for (size_t col = 0; col < 11; ++col)
                    if (!e.row || e.before[col] != after[col])
                    {
                        J value =
                            after[col].is_number() ? J{{"numberValue", after[col]}} : J{{"stringValue", after[col]}};
                        requests.push_back(
                            {{"updateCells",
                              {{"range",
                                {{"sheetId", 0},
                                 {"startRowIndex", row - 1},
                                 {"endRowIndex", row},
                                 {"startColumnIndex", col},
                                 {"endColumnIndex", col + 1}}},
                               {"rows", J::array({J{{"values", J::array({J{{"userEnteredValue", value}}})}}})},
                               {"fields", "userEnteredValue"}}}});
                    }
                ++count;
            }
            // 扩展行数只扩展网格，不覆盖已有数据；单次原子批次避免半批次合并。
            auto metadata = Http(GoogleBase() + "?fields=sheets(properties(sheetId,gridProperties(rowCount)))", "GET",
                                 "", *token, &stop);
            for (const auto &s : metadata.at("sheets"))
                if (s.at("properties").at("sheetId") == 0 &&
                    last > s.at("properties").at("gridProperties").at("rowCount").get<int>())
                    requests.insert(
                        requests.begin(),
                        J{{"appendDimension",
                           {{"sheetId", 0},
                            {"dimension", "ROWS"},
                            {"length", last - s.at("properties").at("gridProperties").at("rowCount").get<int>()}}}});
            if (!requests.empty())
                Http(GoogleBase() + ":batchUpdate", "POST", J{{"requests", requests}}.dump(), *token, &stop);
        }};
}
} // namespace TranslationSync
