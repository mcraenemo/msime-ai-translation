#pragma once
#include "../core/runtime_paths.h"
#include "../core/pinyin_decoder.h"
#include "../core/data_path.h"
#include "../contracts/assets/assets.h"

#include "../common/cache.h"
#include "../core/key_event.h"
#include "../core/word_item.h"
#include "../core/sentence_association_options.h"
#include "../neural/neural_decoder.h"
#include "lattice_rerank.h"
#include "quanpin_query.h"
#include "engine/ngram/language_model.h"
#include "engine/ngram/octagram/octagram_gram.h"
#include "../core/fuzzy_pinyin_options.h"
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

// Result of the autocorrect k-best search for one query: the primary corrected
// segmentation plus the ranked alternative readings. Purely a function of
// (raw_input, segmentation, autocorrect_types) and the static correction tables
// -- independent of dictionary contents -- so it is safe to memoize without
// tying it to the database-version invalidation the other caches use.
struct SeriesQueryResolution
{
    std::string segmentation;
    std::string cache_key;
    quanpin::Segments corrected_segments;
    // Alternative k-best readings that share the primary cut's edit cost (same
    // corrected-edge count and summed weight). These are genuinely ambiguous
    // among equally-likely corrections, so they frequency-compete with the
    // primary in merge_alternative_segmentations (the uanli -> {quan,cuan,...}
    // disambiguation). Empty when the primary reading is unique at its cost.
    std::vector<quanpin::Segments> alternative_corrected_cuts;
    // Readings that are strictly costlier than the primary (e.g. a neighbor
    // reading gau -> gai, weight 13, when the primary is the transposition
    // gau -> gua, weight 10). They stay visible but must never outrank the
    // cheaper tier by dictionary frequency, so they are appended after the
    // primary tier rather than merged into it.
    std::vector<quanpin::Segments> costlier_corrected_cuts;
    bool corrected_input = false;
};

class QuanpinDictionary
{
  public:
    static const int OK = 0;
    static const int ERROR_CODE = -1;

    explicit QuanpinDictionary(std::string db_path = {},
                               metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    ~QuanpinDictionary();

    // Autocorrection is gated by a quanpin::kAutocorrect* type mask (0 = off).
    std::vector<WordItem> query(const std::string &raw_input, const std::string &segmentation = "",
                                unsigned autocorrect_types = 0, metasequoia::FuzzyPinyinOptions fuzzy = {});
    std::vector<WordItem> fuzzy_candidates(const std::string &segmentation, metasequoia::FuzzyPinyinOptions options);
    bool expand_initial_candidates(const std::string &code, std::vector<WordItem> &candidates);
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value);
    int handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch = 0);

    int create_word(std::string pinyin, std::string word);
    int create_word_from_canonical_pinyin(std::string pinyin, std::string word);
    int update_weight_by_word(std::string word);
    int update_weight_by_pinyin_and_word(std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(std::string pinyin, std::string word);
    int insert_word_to_series_cache(const std::string &pinyin, const std::string &word, CandidateSource source);
    int insert_word_to_series_cache(const std::string &raw_input, const std::string &segmentation,
                                    unsigned autocorrect_types, const std::string &word, CandidateSource source);

    std::string search_sentence_from_ime_engine(const std::string &user_pinyin);

    // 设置整句候选来源与去重补位选项。值发生变化时清一次缓存，避免旧候选被继续复用。
    void set_sentence_association(const SentenceAssociationOptions &options);

    // 光标前已上屏的文本，作为神经重排给模型看的上下文。宿主能拿到应用里的前文就给它，
    // 拿不到就不设——空上下文照样能排，只是模型只看句子本身、看不到上文。
    // 神经重排生效时，变化会清一次缓存：同一串拼音在不同上文下的排序可以不同。
    void set_rescoring_context(const std::string &context);

    void reset_state();
    void reset_cache();
    // 只清装着整句排序的那层（series_cache_）。神经重排的结果与上文只影响词格整句的位置，
    // 逐前缀的词库查询和切分都与之无关，全清会让接下来几键都变成冷查询。
    void reset_sentence_cache();

    const std::string &get_pinyin_sequence() const
    {
        return pinyin_sequence_;
    }

    const std::string &get_pinyin_segmentation() const
    {
        return pinyin_segmentation_;
    }

    const std::vector<WordItem> &get_current_candidate_list() const
    {
        return current_candidate_list_;
    }

  private:
    std::vector<WordItem> query_exact(const std::string &raw_input, const std::string &segmentation,
                                      unsigned autocorrect_types);
    std::vector<WordItem> query_series(const std::string &raw_input, const std::string &segmentation,
                                       const quanpin::Segments &segments);
    std::vector<WordItem> query_single_path(const std::string &raw_input, const std::string &segmentation,
                                            const quanpin::Segments &segments);
    quanpin::Segments resolve_segments(const std::string &raw_input, const std::string &segmentation);
    quanpin::Segments get_or_compute_segments(const std::string &raw_input);
    std::vector<WordItem> query_database(const quanpin::Segments &segments, const std::string &segmentation);
    std::vector<WordItem> query_initial(const std::string &code, int limit);
    std::vector<WordItem> append_ime_fallback(const std::string &raw_input, const std::string &segmentation,
                                              std::vector<WordItem> result);
    std::vector<WordItem> append_sparse_pinyin_fallbacks(const quanpin::Segments &segments,
                                                         std::vector<WordItem> result);
    std::vector<WordItem> merge_alternative_segmentations(
        const std::string &raw_input, const std::string &primary_segmentation,
        const quanpin::Segments &primary_segments, const std::vector<quanpin::Segments> &alternative_segmentations,
        std::vector<WordItem> result);
    static void append_unique_words(std::vector<WordItem> &result, const std::vector<WordItem> &extra);
    void mark_autocorrect_candidates(std::vector<WordItem> &candidates, const std::string &raw_input);

    std::vector<std::string> select_data(const std::string &sql_str);
    std::vector<WordItem> select_complete_data(const std::string &sql_str);
    int check_data(const std::string &sql_str);
    int insert_data(const std::string &sql_str);
    int update_data(const std::string &sql_str);
    int delete_data(const std::string &sql_str);

    std::string build_sql_for_creating_word(const std::string &pinyin);
    std::string build_sql_for_checking_word(const std::string &key, const std::string &value);
    std::string build_sql_for_inserting_word(const std::string &key, const std::string &jp, const std::string &value);
    std::string build_sql_for_updating_word(const std::string &word);
    std::string build_sql_for_updating_word(std::string pinyin, const std::string &word);
    std::string build_sql_for_deleting_word(std::string pinyin, const std::string &word);
    bool do_validate(const std::string &key, const std::string &jp, const std::string &value);
    void reset_cache_if_database_changed();
    int insert_word_to_series_cache_key(const std::string &cache_key, const std::string &pinyin,
                                        const std::string &word, CandidateSource source);

  private:
    CircularBuffer<std::string, std::vector<WordItem>> cache_;
    CircularBuffer<std::string, std::vector<WordItem>> series_cache_;
    CircularBuffer<std::string, quanpin::Segments> segmentation_cache_;
    // Memoizes the k-best correction search keyed on its full input tuple so a
    // repeated keystroke (backspace / re-type) never re-runs the k=9 beam. Not
    // cleared with the dictionary caches: its value depends only on the input
    // and the static correction tables, never on dictionary rows.
    CircularBuffer<std::string, SeriesQueryResolution> resolution_cache_;
    sqlite3 *db_ = nullptr;
    sqlite3_int64 data_version_ = -1;
    metasequoia::RuntimePaths paths_;
    metasequoia::PinyinDecoder decoder_;
    // 词格整句的打分模型（kenlm 三元），按资源路径进程内共享、只读。缺模型时
    // valid() 为假，word_lattice 退回旧的启发式打分：整句候选只是变差，不会消失。
    const ngram::LanguageModel *language_model_ = nullptr;
    // 万象语法模型（octagram .gram），随 set_sentence_association 的模型路径解析，
    // 进程内按路径共享、mmap 只读。路径为空或无效时为空，词格退回无搭配打分。
    // shared_ptr 持有：设置侧删除模型包 evict 缓存并清掉这里的引用后，映射即可
    // 释放，被删的文件能被物理清扫。
    std::shared_ptr<const gram::GramDb> collocation_db_ = nullptr;
    // 神经整句模型（chinese-ime-lm），按资源路径进程内共享、只读。两档可同时参与打分；
    // 缺文件时对应的整句候选不出，不影响其它候选。对应开关第一次生效时才载入，
    // 关着的那档不占内存。
    neural::LazySentenceModel neural_desktop_model_;
    neural::LazySentenceModel neural_keyboard_model_;
    // 整句候选来源与去重补位选项，默认全关；由 set_sentence_association 随请求更新。
    SentenceAssociationOptions sentence_association_;
    // 神经重排的上下文（光标前已上屏的文本）。宿主没给就是空串。
    std::string rescoring_context_;
    std::unordered_map<std::string, sqlite3_stmt *> statement_cache_;
    std::string db_path_;

    std::string pinyin_sequence_;
    std::string pinyin_segmentation_;
    // Joined alternative correction cuts (k-best readings after the primary)
    // backing mark_autocorrect_candidates on every query, cached or not.
    std::vector<std::string> pinyin_alternative_segmentations_;
    std::vector<WordItem> current_candidate_list_;
};
