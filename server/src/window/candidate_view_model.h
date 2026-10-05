#pragma once

#include <string>

// Built once on the candidate worker. Renderers only adapt this presentation data;
// they do not query dictionaries, derive helpcodes or decide which source gets a badge.
struct CandidateViewItem
{
    std::string text;
    std::string annotation;
    // 来源/状态徽标（☁️/🤖、固定排位 📎）。纯展示字段：D2D（text+badge 拼接）与
    // WebView2（CandidateViewHtml）都只把它渲染进词条文本，page_words 与上屏内容不含它。
    std::string badge;
    std::string translation;
    bool fixed_position = false;
};

inline std::string EscapeCandidateViewHtml(const std::string &text)
{
    std::string result;
    for (char ch : text)
    {
        switch (ch)
        {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        // The existing WebView transport splits on commas before restoring U+F000.
        case ',':
            result += "\xEF\x80\x80";
            break;
        default:
            result += ch;
            break;
        }
    }
    return result;
}

// 固定排位的词条补固定徽标：D2D 原生候选窗原本不显示任何固定标记，WebView2 侧也只有
// 整条变蓝这一种弱提示。复用 badge 的既有展示路径而非新增字段——D2D 拼 text+badge，
// WebView2 经 CandidateViewHtml 把 badge 转义进主文本 run（固定词条落在蓝色 span 内）。
// show/style 由调用方从配置求值后传入：本头是纯展示层，不读配置。样式默认 📎 而非 📌——
// 实心红钉在蓝底词条上像贴了张贴纸，回形针同样是「固定」语义但安静得多。
// 未列出的样式不加徽标，与配置层白名单同宽，手改 TOML 写错也不会渲染出畸形标记。
inline void ApplyFixedPositionBadge(CandidateViewItem &view, bool show, const std::string &style)
{
    if (!view.fixed_position || !show)
        return;
    if (style == "pushpin")
        view.badge += " \xF0\x9F\x93\x8C"; // 📌
    else if (style == "dot")
        view.badge += " \xC2\xB7"; // ·
    else if (style == "paperclip")
        view.badge += " \xF0\x9F\x93\x8E"; // 📎
}

inline std::string CandidateViewHtml(const CandidateViewItem &item)
{
    std::string html = EscapeCandidateViewHtml(item.text + item.annotation + item.badge);
    if (item.fixed_position)
        html = "<span style=\"color:#379AD3\">" + html + "</span>";
    if (!item.translation.empty())
        html += "<span class=\"cand-translation\">" + EscapeCandidateViewHtml(item.translation) + "</span>";
    return html;
}
