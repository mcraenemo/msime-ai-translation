#include "ai/ai_translation.h"
#include "ai/translation_sentence_buffer.h"
#include "ai/ai_translation_language.h"
#include "ai/ai_translation_hotkey.h"
#include "tests/includes/test_framework.h"
#include <filesystem>
#include <sqlite3.h>
#include <vector>

using namespace std::chrono_literals;
TEST_CASE(ai_translation_languages_are_extensible)
{
    for (const auto language : {"en", "es", "sw", "zh-Hant", "ceb", "sr-Latn", u8"粤语", u8"宿务语"})
        REQUIRE(AiTranslation::IsValidTargetLanguage(language));
    REQUIRE(!AiTranslation::IsValidTargetLanguage(""));
    REQUIRE(!AiTranslation::IsValidTargetLanguage(" en"));
    REQUIRE(!AiTranslation::IsValidTargetLanguage("en\nfr"));
    REQUIRE(!AiTranslation::IsValidTargetLanguage(std::string(81, 'a')));
}
namespace
{
struct Fixture
{
    std::string path;
    AiTranslation::Worker worker;
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<AiTranslation::Result> results;
    Fixture()
    {
        static std::atomic<int> id{0};
        path = (std::filesystem::temp_directory_path() /
                std::filesystem::path("msime-translation-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                      std::to_string(++id) + ".db"))
                   .u8string();
        Start();
    }
    void Start(AiTranslation::Fetcher fetcher = {})
    {
        if (!fetcher)
            fetcher = [this](const auto &, const auto &) {
                ++calls;
                return std::string("The complete translation; with punctuation.");
            };
        worker.Start(
            path,
            [this](auto result) {
                std::lock_guard lock(mutex);
                results.push_back(std::move(result));
                cv.notify_all();
            },
            std::move(fetcher), 10ms);
    }
    AiTranslation::Result Next(const AiTranslation::Request &r)
    {
        size_t before;
        {
            std::lock_guard lock(mutex);
            before = results.size();
        }
        worker.Submit(r);
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 2s, [&] { return results.size() > before; }));
        return results.back();
    }
    ~Fixture()
    {
        worker.Stop();
        for (const auto suffix : {"", "-wal", "-shm"})
        {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path + suffix), ec);
        }
    }
};
AiTranslation::Request Chinese()
{
    AiTranslation::Request r;
    r.source = u8"你今天上班了吗？";
    r.target = "en";
    r.character_set = "simplified";
    r.config.translation_enabled = true;
    r.config.enabled = false;
    r.config.provider = "openai";
    r.config.model = "gpt-6-luna";
    return r;
}
} // namespace
TEST_CASE(ai_translation_cache_hit_and_restart_never_fetch_again)
{
    Fixture f;
    auto r = Chinese();
    REQUIRE(!f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 1);
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 1);
    f.worker.Stop();
    f.Start();
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 1);
}
TEST_CASE(ai_translation_disabled_and_pinyin_never_fetch)
{
    Fixture f;
    auto r = Chinese();
    r.config.translation_enabled = false;
    f.worker.Submit(r);
    std::this_thread::sleep_for(60ms);
    REQUIRE_EQ(f.calls.load(), 0);
    r.config.translation_enabled = true;
    r.source = "ni jin tian shang ban le ma";
    f.worker.Submit(r);
    std::this_thread::sleep_for(60ms);
    REQUIRE_EQ(f.calls.load(), 0);
    REQUIRE(f.results.empty());
}
TEST_CASE(ai_translation_target_charset_and_ambiguous_context_are_distinct)
{
    Fixture f;
    auto r = Chinese();
    r.source = u8"方便";
    r.context = u8"软件界面";
    f.Next(r);
    r.target = "tl";
    f.Next(r);
    r.character_set = "traditional";
    r.source = u8"方便";
    f.Next(r);
    r.context = u8"考试题目";
    f.Next(r);
    REQUIRE_EQ(f.calls.load(), 4);
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 4);
}
TEST_CASE(ai_translation_long_sentence_cache_ignores_unnecessary_context)
{
    Fixture f;
    auto r = Chinese();
    r.source = u8"我今天下班以后准备去超市买一些水果和蔬菜。";
    r.context = u8"前文一";
    f.Next(r);
    r.context = u8"前文二";
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 1);
}
TEST_CASE(ai_translation_clear_noun_and_question_reuse_after_context_changes)
{
    Fixture f;
    auto r = Chinese();
    r.context = u8"上一句话";
    f.Next(r);
    r.context = u8"上一句话你今天上班了吗？";
    REQUIRE(f.Next(r).cache_hit);
    r.source = u8"选项";
    f.Next(r);
    r.context += u8"选项";
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 2);
}
TEST_CASE(ai_translation_clear_cache_forces_next_fetch)
{
    Fixture f;
    auto r = Chinese();
    f.Next(r);
    REQUIRE(AiTranslation::ClearCache(f.path));
    REQUIRE(!f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 2);
}
TEST_CASE(ai_translation_submission_is_nonblocking_and_off_cancels)
{
    Fixture f;
    f.worker.Stop();
    std::atomic<bool> entered{false};
    f.Start([&](const auto &, const auto &cancelled) {
        ++f.calls;
        entered = true;
        while (!cancelled())
            std::this_thread::sleep_for(1ms);
        return std::string("stale");
    });
    auto r = Chinese();
    auto start = std::chrono::steady_clock::now();
    f.worker.Submit(r);
    REQUIRE(std::chrono::steady_clock::now() - start < 30ms);
    for (int i = 0; i < 1000 && !entered; ++i)
        std::this_thread::sleep_for(1ms);
    REQUIRE(entered);
    r.config.translation_enabled = false;
    f.worker.Submit(r);
    std::this_thread::sleep_for(50ms);
    REQUIRE(f.results.empty());
    REQUIRE_EQ(f.calls.load(), 1);
}
TEST_CASE(ai_translation_failures_are_not_cached)
{
    Fixture f;
    f.worker.Stop();
    f.Start([&](const auto &, const auto &) {
        ++f.calls;
        return std::string{};
    });
    f.worker.Submit(Chinese());
    std::this_thread::sleep_for(60ms);
    f.worker.Submit(Chinese());
    std::this_thread::sleep_for(60ms);
    REQUIRE_EQ(f.calls.load(), 2);
    REQUIRE_EQ(f.results.size(), 2u);
    REQUIRE(f.results.back().translation.empty());
}
TEST_CASE(ai_translation_cache_clear_during_fetch_does_not_repopulate)
{
    Fixture f;
    f.worker.Stop();
    std::atomic<bool> entered{false}, finish{false};
    f.Start([&](const auto &, const auto &) {
        ++f.calls;
        entered = true;
        while (!finish)
            std::this_thread::sleep_for(1ms);
        return std::string("old result");
    });
    f.worker.Submit(Chinese());
    for (int i = 0; i < 1000 && !entered; ++i)
        std::this_thread::sleep_for(1ms);
    REQUIRE(entered);
    REQUIRE(AiTranslation::ClearCache(f.path));
    finish = true;
    std::this_thread::sleep_for(40ms);
    f.worker.Stop();
    f.Start();
    REQUIRE(!f.Next(Chinese()).cache_hit);
    REQUIRE_EQ(f.calls.load(), 2);
}
TEST_CASE(ai_translation_modes_and_configurable_shortcut)
{
    REQUIRE_EQ(AiTranslation::ModeLabel(false, false), L"简");
    REQUIRE_EQ(AiTranslation::ModeLabel(true, false), L"繁");
    REQUIRE_EQ(AiTranslation::ModeLabel(false, true), L"简译");
    REQUIRE_EQ(AiTranslation::ModeLabel(true, true), L"繁译");
    REQUIRE(AiTranslation::ParseHotkey("").valid);
    REQUIRE_EQ(AiTranslation::ParseHotkey("").key, 0u);
    const auto h = AiTranslation::ParseHotkey("Ctrl+Alt+T");
    REQUIRE(h.valid);
    REQUIRE_EQ(h.key, static_cast<UINT>('T'));
    REQUIRE_EQ(h.modifiers, static_cast<UINT>(MOD_CONTROL | MOD_ALT));
    REQUIRE(!AiTranslation::ParseHotkey("T").valid);
    REQUIRE(!AiTranslation::ParseHotkey("Ctrl+Ctrl+T").valid);
    REQUIRE(!AiTranslation::ParseHotkey("Ctrl+Shift+F").valid);
    REQUIRE(!AiTranslation::ParseHotkey("Ctrl+F25").valid);
    REQUIRE(AiTranslation::ParseHotkey("Alt+F24").valid);
}

TEST_CASE(sentence_buffer_segments_punctuation_editing_and_failure)
{
    AiTranslation::SentenceBuffer b;
    b.Append(u8"我觉得");
    b.Append(u8"我这个人");
    b.Append(u8"还是挺好的");
    REQUIRE_EQ(b.text, std::string(u8"我觉得我这个人还是挺好的"));
    b.Append(u8"。😀");
    b.Backspace();
    REQUIRE_EQ(b.text, std::string(u8"我觉得我这个人还是挺好的。"));
    REQUIRE(b.Begin(true));
    REQUIRE(!b.Begin(true));
    b.Fail();
    REQUIRE_EQ(b.text, std::string(u8"我觉得我这个人还是挺好的。"));
    REQUIRE(b.Begin(true));
    b.CancelRequest();
    REQUIRE(!b.translating);
    b.Clear();
    REQUIRE(b.text.empty());
}
TEST_CASE(sentence_buffer_no_requests_until_ctrl_enter_and_cache_hit_is_zero_requests)
{
    Fixture f;
    AiTranslation::SentenceBuffer b;
    for (const auto part : {u8"我觉得", u8"我这个人", u8"还是挺好的", u8"。"})
    {
        b.Append(part); // Space, number selection and punctuation all append only.
        REQUIRE(!b.Begin(false));
    }
    b.Backspace();
    b.Append(u8"？");
    std::this_thread::sleep_for(80ms); // Waiting never starts translation.
    REQUIRE_EQ(f.calls.load(), 0);
    auto r = Chinese();
    r.source = b.text;
    REQUIRE(b.Begin(true));
    REQUIRE(!b.Begin(true)); // rapid duplicate command
    REQUIRE(!f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 1);
    b.Clear();
    b.Append(r.source);
    REQUIRE(b.Begin(true));
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE_EQ(f.calls.load(), 1);
    b.Clear();
    b.Append(u8"按回车只上屏中文");
    REQUIRE(!b.Begin(false));
    b.Clear();
    REQUIRE_EQ(f.calls.load(), 1);
}

TEST_CASE(reading_translation_accepts_languages_and_rejects_noise)
{
    for (const auto text : {"I have to work tomorrow.", "Kailangan kong magtrabaho bukas.", u8"明日は仕事です。",
                            u8"내일 일해야 해요", u8"我明天得上班。"})
        REQUIRE(AiTranslation::IsReadingSource(text));
    for (const auto text : {"", "  \r\n", "123456.00", "!?,.;", "a", u8"。？！", "\xFF\xFE"})
        REQUIRE(!AiTranslation::IsReadingSource(text));
    REQUIRE(!AiTranslation::IsReadingSource(std::string(16385, 'a')));
    REQUIRE(AiTranslation::IsReadingSource(std::string(12000, 'a')));
}
TEST_CASE(reading_translation_caches_english_tagalog_long_text_and_never_submits_when_disabled)
{
    Fixture f;
    auto r = Chinese();
    r.reading = true;
    r.target = "zh-Hans";
    r.character_set = "auto";
    for (const auto source : {std::string("I have to work tomorrow."), std::string("Kailangan kong magtrabaho bukas."),
                              std::string(12000, 'a')})
    {
        r.source = source;
        const int before = f.calls;
        REQUIRE(!f.Next(r).cache_hit);
        REQUIRE(f.calls == before + 1);
        REQUIRE(f.Next(r).cache_hit);
        REQUIRE(f.calls == before + 1);
    }
    r.source = "A new uncached sentence.";
    r.config.translation_enabled = false;
    f.worker.Submit(r);
    std::this_thread::sleep_for(100ms);
    REQUIRE(f.calls == 3);
    r.config.translation_enabled = true;
    r.source = "1234 !?";
    f.worker.Submit(r);
    std::this_thread::sleep_for(100ms);
    REQUIRE(f.calls == 3);
}
TEST_CASE(reading_and_sending_share_identical_translation_cache_without_direction_namespace)
{
    Fixture f;
    auto r = Chinese();
    REQUIRE(!f.Next(r).cache_hit);
    r.reading = true;
    r.character_set = "traditional";
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE(f.calls == 1);
    r.reading = false;
    r.character_set = "simplified";
    REQUIRE(f.Next(r).cache_hit);
    REQUIRE(f.calls == 1);
}
TEST_CASE(reading_translation_cancel_preserves_app_and_suppresses_stale_result)
{
    Fixture f;
    f.worker.Stop();
    std::atomic<bool> started{false}, cancelled{false};
    f.Start([&](const auto &, const auto &cancel) {
        ++f.calls;
        started = true;
        while (!cancel())
            std::this_thread::sleep_for(1ms);
        cancelled = true;
        return std::string{};
    });
    auto r = Chinese();
    r.reading = true;
    r.source = "Read this only.";
    f.worker.Submit(r);
    for (int i = 0; i < 500 && !started; ++i)
        std::this_thread::sleep_for(1ms);
    REQUIRE(started);
    f.worker.Cancel();
    for (int i = 0; i < 500 && !cancelled; ++i)
        std::this_thread::sleep_for(1ms);
    REQUIRE(cancelled);
    REQUIRE(f.results.empty());
}
