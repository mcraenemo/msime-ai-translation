#pragma once

#include "../core/word_item.h"

#include <string>
#include <vector>

namespace metasequoia::local_modes
{
struct LocalDateTime
{
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned weekday = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
};

LocalDateTime current_local_date_time();
bool is_date_time_keyword(const std::string &keyword);
// 每条候选的 WordItem::pinyin 是它的格式 ID（如 "date:ymd_dash"）。调频、置顶、固定位置都按格式 ID
// 记：日期文本每天都变，按文字记，明天就对不上了。
std::vector<WordItem> query_date_time(const std::string &keyword, const LocalDateTime *now = nullptr, int limit = 17);
// 唤醒词所属的一组格式："date" / "time" / "week"，不是唤醒词时为空。rq / riqi / date 同属一组，
// 共用一份学到的顺序。
std::string date_time_category(const std::string &keyword);
// 这组格式的全部 ID，按出厂顺序。包括此刻给不出文本、query_date_time 跳过的那些。
std::vector<std::string> date_time_format_ids(const std::string &keyword);

// 混输里展开全部格式的那个入口候选。它不上屏：选中时候选框换成 query_date_time 的整组格式。
// pinyin 是前缀加唤醒词，展开时据此重查。
inline constexpr const char *kDateTimeMenuPrefix = "menu:";
WordItem date_time_menu_item(const std::string &keyword);
bool is_date_time_menu_item(const WordItem &item);
std::string date_time_menu_keyword(const WordItem &item);
} // namespace metasequoia::local_modes
