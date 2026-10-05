// 按键处理：按键分类、组合编辑（光标/退格/插入）、译文副候选页、五笔顶字，以及 HandleImeKey 主流程。
#include "ipc/event_listener_internal.h"
#include "ai/translation_sentence_buffer.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include <cstdint>
#include <utility>
#include "ipc.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/candidate_translation_policy.h"
#include "ipc/input_key_policy.h"
#include "engine/contracts/ipc_negotiation.h"
#include "engine/contracts/voice_composition_pipe.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include "global/globals.h"
#include "window/caret_state_indicator_policy.h"
#include "utils/ime_utils.h"
#include "config/ime_config.h"
#include "session/session_factory.h"
#include "mixed/date_time_candidates.h"
#include "log/candidate_diag_log.h"

using namespace event_listener_detail;

namespace
{
bool IsShiftLetterSpecialModeTriggered()
{
    return g_quick_phrase_triggered || g_unicode_mode_triggered || g_date_time_mode_triggered ||
           g_emoji_mode_triggered || g_kaomoji_mode_triggered || g_jianpin_mode_triggered || g_y_mode_triggered ||
           g_r_mode_triggered;
}

// 日语模式由配置项决定，和 R 模式（中文里临时切日语）无关：TSF 侧只能看到配置，
// 两侧必须用同一个判据，否则按键分类会不一致、预编辑会错位。
bool IsJapaneseInputMode()
{
    return GetConfiguredInputMode() == "japanese";
}

// 日语模式下 '-' 不翻页，而是长音符（ー）的输入键。空编码时也要起头组合，
// 候选框第一项是长音符 ー、第二项是普通连字符 '-'（见日语候选提供者）。
bool IsJapaneseLongVowelKey(UINT keycode, WCHAR wch)
{
    return keycode == VK_OEM_MINUS && wch == L'-' && IsJapaneseInputMode() && g_inputSession != nullptr;
}

// 日语模式下 '-' '=' 一律不当翻页键用。
bool IsJapaneseDisabledPagingKey(UINT keycode)
{
    return (keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS) && IsJapaneseInputMode();
}

bool IsCommitWithHighlightedCandidatePunctuationInCandidateMode(UINT keycode, WCHAR wch)
{
    if (keycode == VK_TAB)
    {
        return false;
    }
    if ((keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS) && !IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    // 日语模式下 '-' 走长音符输入，不能当作上屏标点。
    if (IsJapaneseLongVowelKey(keycode, wch))
    {
        return false;
    }
    const bool has_active_composition = g_inputSession != nullptr && !g_inputSession->get_pinyin_sequence().empty();
    if ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled() &&
        has_active_composition)
    {
        return false;
    }
    if ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled() && has_active_composition)
    {
        return false;
    }

    static const std::unordered_set<WCHAR> kCommitWithHighlightedCandidatePunctuation = {
        L'`',  //
        L'!',  //
        L'@',  //
        L'#',  //
        L'$',  //
        L'%',  //
        L'^',  //
        L'&',  //
        L'*',  //
        L'-',  // Numpad arithmetic keys are not candidate paging keys.
        L'+',  //
        L'_',  // 日语模式禁用 -/= 翻页后，这两个字符退回标点上屏。
        L'=',  //
        L'(',  //
        L')',  //
        L'[',  //
        L']',  //
        L'\\', //
        L'/',  //
        L';',  //
        L':',  //
        L'\'', //
        L'"',  //
        L',',  //
        L'<',  //
        L'.',  //
        L'>',  //
        L'?'   //
    };
    return kCommitWithHighlightedCandidatePunctuation.find(wch) != kCommitWithHighlightedCandidatePunctuation.end();
}

bool IsManualPinyinSeparatorKey(UINT keycode, WCHAR wch)
{
    return keycode == VK_OEM_7 && wch == L'\'' && g_inputSession != nullptr &&
           g_inputSession->current_scheme_type() != SchemeType::Wubi && !g_inputSession->get_pinyin_sequence().empty();
}

bool IsMicrosoftShuangpinIngKeyAt(UINT keycode, WCHAR wch, const std::string &raw_input, size_t caret)
{
    if (keycode != VK_OEM_1 || wch != L';' || !IsConfiguredShuangpinSemicolonFinal() || g_inputSession == nullptr ||
        g_inputSession->current_scheme_type() != SchemeType::Shuangpin)
    {
        return false;
    }

    const size_t separator = caret == 0 ? std::string::npos : raw_input.rfind('\'', caret - 1);
    const size_t chunk_start = separator == std::string::npos ? 0 : separator + 1;
    return (caret - chunk_start) % 2 == 1;
}

bool IsMicrosoftShuangpinIngKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    return IsMicrosoftShuangpinIngKeyAt(keycode, wch, raw_input,
                                        (std::min)(GlobalIme::composition.caret_position, raw_input.size()));
}

// 句中辅助码的触发键（反引号或分号，按设置里勾选的）：开关开着、双拼，且光标前这一节能接一段时，
// 它是编码键而不是标点，输入串里一律记成反引号。光标可以在句中（用箭头移回去补辅助码）。TSF 端按
// 同一条形状规则（FanyImeMidSentenceHelpcode::AcceptsMarkerAt）预判吃键，光标也按
// ApplyCompositionEditKey 插字时的同一个位置算。分号先让给 ing 韵母（IsMicrosoftShuangpinIngKey），
// 两侧判断顺序一致。
bool IsMidSentenceHelpcodeMarkerKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    const bool backtick = keycode == VK_OEM_3 && wch == L'`';
    const bool semicolon = keycode == VK_OEM_1 && wch == L';';
    if ((!backtick && !semicolon) || !IsConfiguredMidSentenceHelpcodeTrigger(wch) || g_english_input_mode ||
        g_inputSession == nullptr)
    {
        return false;
    }
    const auto &composition = GlobalIme::composition;
    const size_t caret = composition.raw_input_with_cases != raw_input && composition.caret_position == 0
                             ? raw_input.size()
                             : (std::min)(composition.caret_position, raw_input.size());
    if (semicolon && IsMicrosoftShuangpinIngKeyAt(keycode, wch, raw_input, caret))
    {
        return false;
    }
    return g_inputSession->accepts_mid_sentence_helpcode_marker(caret);
}

bool IsSelectionKey(UINT keycode)
{
    if (keycode == VK_SPACE)
        return true;
    if (keycode >= '0' && keycode <= '9')
    {
        const std::string raw = g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
        if (IsUnicodeCompositionActive(raw))
        {
            // U-mode: bare digits compose hex; Shift+1..9 selects candidates.
            const bool shift_only = (Global::ModifiersDown & 0b00000111u) == 0b00000001u;
            return shift_only && keycode >= '1' && keycode <= '9';
        }
        return true;
    }
    return false;
}

bool IsPagingKey(UINT keycode)
{
    if (IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_LEFT || keycode == VK_RIGHT || keycode == VK_UP || keycode == VK_DOWN ||
           ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled()) ||
           ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled());
}

bool IsCandidateNavigationKey(UINT keycode)
{
    if (IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD ||
           keycode == VK_OEM_4 || keycode == VK_OEM_6 || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_UP || keycode == VK_DOWN;
}

bool ApplyCompositionEditKey(UINT keycode, WCHAR wch, UINT modifiers_down, bool client_supports_restore,
                             bool &composition_restored)
{
    composition_restored = false;
    std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
    auto &composition = GlobalIme::composition;
    if (composition.raw_input_with_cases != raw && composition.caret_position == 0 && !raw.empty())
    {
        composition.caret_position = raw.size();
    }
    composition.caret_position = (std::min)(composition.caret_position, raw.size());

    // R2/R10：光标前缀重算总门控（与 Ctrl+Backspace / Ctrl+方向同一谓词族）。未协商、
    // UILess、专用英文、特殊模式组合一律维持整串转换，光标只是显示层插入点。
    const bool caret_resegmentation = FanyImeIpc::ShouldResegmentCompositionByCaret(
        client_supports_restore, IsUiLessMode(), g_english_input_mode, IsSpecialModeCompositionActive(raw));
    // 箭头与 Ctrl+方向路径不改 raw、没有 pending 序列，喂完光标重解一次即可。串尾
    // 也必须显式喂：引擎 caret_ 只在 set_pinyin_sequence 触发的 apply_pending_sequence
    // 里复位，箭头路径绕过它——串尾不喂 nullopt（与 set_caret(size) 在量化边界上等
    // 价）会残留上一次前缀激活的 caret_，候选停在旧前缀上、空格结算走错前缀路径（R7）。
    const auto resegment_by_caret = [&]() {
        if (!caret_resegmentation)
        {
            return;
        }
        g_inputSession->set_caret(composition.caret_position < raw.size()
                                      ? std::optional<std::size_t>(composition.caret_position)
                                      : std::nullopt);
        g_inputSession->recompute_candidates();
    };

    // Ctrl+Left / Ctrl+Right jump the caret by one segmentation unit instead of
    // one character, consuming the same engine boundaries Ctrl+Backspace
    // deletes. The Server owns the unit model, so it moves the authoritative
    // caret and answers with CompositionRestored; TSF only applies that caret.
    // Everything unnegotiated, UILess or unit-less keeps the single-character
    // move below, so both sides agree on when the jump happens.
    const bool segment_caret = FanyImeIpc::IsSegmentCaretKey(keycode, modifiers_down);
    const bool segment_caret_supported = segment_caret && client_supports_restore && !IsUiLessMode() &&
                                         !g_english_input_mode && !IsSpecialModeCompositionActive(raw);
    if (keycode == VK_LEFT || keycode == VK_RIGHT)
    {
        if (segment_caret_supported)
        {
            const std::vector<std::size_t> boundaries = g_inputSession->segment_raw_boundaries();
            if (!boundaries.empty())
            {
                composition.caret_position =
                    keycode == VK_LEFT ? FanyImeIpc::PreviousSegmentBoundary(boundaries, composition.caret_position)
                                       : FanyImeIpc::NextSegmentBoundary(boundaries, composition.caret_position);
                composition_restored = true;
                resegment_by_caret();
                return true;
            }
        }
        if (keycode == VK_LEFT)
        {
            if (composition.caret_position > 0)
            {
                --composition.caret_position;
            }
        }
        else if (composition.caret_position < raw.size())
        {
            ++composition.caret_position;
        }
        resegment_by_caret();
        return true;
    }

    // A spelling emptied by a segment Backspace below keeps the creating-word
    // state alive with the word alone (PRD R3), so the empty-raw cleanup has to
    // know this key produced that state on purpose.
    bool keep_creating_word_after_empty_raw = false;

    if (keycode == VK_BACK)
    {
        // Ctrl+Backspace deletes one segmentation unit (one character's pinyin)
        // instead of one character. The boundaries are the engine's, so TSF
        // cannot mirror the deletion: it rebuilds from the CompositionRestored
        // reply, and everything unnegotiated or unit-less falls back to the
        // ordinary single-character behavior right below.
        const bool segment_backspace = FanyImeIpc::IsSegmentBackspaceKey(keycode, modifiers_down);
        const bool segment_supported = segment_backspace && client_supports_restore && !IsUiLessMode() &&
                                       !g_english_input_mode && !IsSpecialModeCompositionActive(raw);
        if (segment_supported && FanyImeIpc::ShouldDropCreatingWordSegment(
                                     composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                     composition.caret_position, composition.selection_history.size()))
        {
            // R3: nothing is left before the caret, so the key removes the last
            // selected segment itself. Its spelling is discarded -- unlike the
            // retraction below the user asked to delete the segment, not to
            // edit its pinyin again -- and the raw stays empty.
            composition_restored = composition.drop_last_selection();
            keep_creating_word_after_empty_raw = composition_restored && composition.creating_word.active;
        }
        else if (segment_supported)
        {
            const std::vector<std::size_t> boundaries = g_inputSession->segment_raw_boundaries();
            const std::size_t start = FanyImeIpc::PreviousSegmentBoundary(boundaries, composition.caret_position);
            if (start < composition.caret_position)
            {
                raw.erase(start, composition.caret_position - start);
                composition.caret_position = start;
                FanyImeIpc::DropDanglingSegmentDelimiter(raw, start);
                composition_restored = true;
                // Emptying the raw does not end the word: the accumulated
                // segments stay on screen and the next Ctrl+Backspace drops one
                // of them (R3).
                keep_creating_word_after_empty_raw =
                    raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(
                                       composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                       composition.selection_history.size());
            }
        }

        if (!composition_restored &&
            FanyImeIpc::ShouldRetreatCreatingWordSelection(
                composition.creating_word.active, IsUiLessMode(), client_supports_restore, raw.size(),
                composition.selection_history.size(), composition.last_selection_raw_edited()))
        {
            // This Backspace must not also delete the character: the retraction
            // removes the segment and restores its raw spelling instead. The
            // restore is state only -- the engine sequence, its candidates and
            // the restored-caret prefix are applied exactly once by the tail
            // below, so nothing here may rebuild them; a helper that did cost
            // a second full candidate query on every retraction.
            composition_restored = composition.restore_last_selection();
            if (composition_restored)
            {
                // The retraction already replaced the raw, the word and the
                // caret; the local copy must follow it so the tail below
                // re-applies the restored sequence with its caret prefix
                // recompute instead of clobbering it with the stale raw.
                raw = composition.raw_input_with_cases;
            }
        }
        else if (!composition_restored && composition.caret_position > 0)
        {
            raw.erase(composition.caret_position - 1, 1);
            --composition.caret_position;
            // Deleting the last raw character must not take the accumulated word
            // down with it: the composition stays alive showing the selected
            // segments alone -- the same R3 state a segment Backspace produces --
            // and the reply below tells the client to keep composing instead of
            // cancelling. The next Backspace then retracts the newest selection
            // from that empty raw (the empty-raw override of the edit lock)
            // rather than discarding everything the user picked.
            keep_creating_word_after_empty_raw =
                raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(
                                   composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                   composition.selection_history.size());
        }
    }
    else if (keycode == VK_DELETE)
    {
        if (composition.caret_position < raw.size())
        {
            raw.erase(composition.caret_position, 1);
        }
    }
    else
    {
        char input = 0;
        if (keycode >= 'A' && keycode <= 'Z')
        {
            input = wch >= L'A' && wch <= L'Z' || wch >= L'a' && wch <= L'z' ? static_cast<char>(wch)
                                                                             : static_cast<char>(keycode + ('a' - 'A'));
        }
        else if (keycode == VK_OEM_7 && wch == L'\'')
        {
            input = '\'';
        }
        else if (IsMicrosoftShuangpinIngKey(keycode, wch, raw))
        {
            input = ';';
        }
        else if (IsMidSentenceHelpcodeMarkerKey(keycode, wch, raw))
        {
            input = '`';
        }
        else if (IsJapaneseLongVowelKey(keycode, wch))
        {
            input = '-';
        }
        else if (IsUnicodeCompositionActive(raw) && keycode >= '0' && keycode <= '9')
        {
            input = static_cast<char>(keycode);
        }
        else if (IsUnicodeCompositionActive(raw) && keycode == VK_OEM_PLUS && wch == L'+' && raw == "U")
        {
            input = '+';
        }
        else
        {
            return false;
        }
        if (input == '\'' && ((composition.caret_position > 0 && raw[composition.caret_position - 1] == '\'') ||
                              (composition.caret_position < raw.size() && raw[composition.caret_position] == '\'')))
        {
            return true;
        }
        raw.insert(raw.begin() + static_cast<std::ptrdiff_t>(composition.caret_position), input);
        ++composition.caret_position;
        // Typing locks the newest selection (Rime's selected_before_editing):
        // Backspace must keep deleting these fresh characters instead of
        // retracting the selection out from under them. Caret moves never reach
        // here and deliberately do not lock.
        composition.note_raw_inserted();
    }

    if (raw.empty() && !keep_creating_word_after_empty_raw)
    {
        // Without a kept state, TSF cancels the whole composition as soon as the
        // last remaining character is gone, so the accumulated word and the
        // snapshots a later Backspace could retract from must not survive here:
        // they would let a fresh pinyin composition retract a segment of the
        // previous one. Both Backspaces that legitimately empty the raw keep them
        // on purpose instead: the segment one through R3, the plain one because
        // its reply tells the client to keep composing with the word alone.
        composition.clear_creating_word();
        composition.selection_history.clear();
    }

    g_inputSession->set_pinyin_sequence(raw);
    g_inputSession->set_pinyin_sequence_with_cases(raw);
    if (caret_resegmentation && composition.caret_position < raw.size())
    {
        // apply_pending_sequence() 会复位引擎光标：先让新 raw 生效，再喂光标做前缀
        // 重解（R2/R6）。caret 在串尾时不进这里，上一次 recompute 就是现状整串解码
        // （R7 零回归）。
        g_inputSession->recompute_candidates();
        g_inputSession->set_caret(composition.caret_position);
        g_inputSession->recompute_candidates();
    }
    else
    {
        g_inputSession->recompute_candidates();
    }
    composition.raw_input_with_cases = raw;
    return true;
}
} // namespace

namespace event_listener_detail
{
namespace
{
AiTranslation::SentenceBuffer sentence;
uint64_t sentence_client = 0, sentence_epoch = 0, sentence_generation = 0;
AiTranslation::Request sentence_request;
uint64_t sentence_delivery = 0, next_sentence_delivery = 0;
void ResetSentenceSpelling()
{
    g_inputSession->reset_state();
    GlobalIme::composition.clear();
    Global::candidate_ui.set_items({});
    ClearSpecialModeTriggers();
    UpdateCloudInput("");
    UpdateEnglishInput("");
    UpdateMixedInput({});
    UpdateAiInput("");
}
void PublishSentence(uint64_t client, uint64_t epoch)
{
    PublishCandidateUiOwner(client, epoch);
    if (sentence.text.empty() && g_inputSession->get_pinyin_sequence_with_cases().empty())
        HideCandidateWindowAndDropItems();
    else
        RefreshCandidatePageUi(true);
}
void SelectSentencePart(UINT code, uint64_t client, uint64_t epoch, int index = -1)
{
    if (g_inputSession->get_pinyin_sequence_with_cases().empty())
        return;
    FanyNamedPipe::ProcessSelectionKey(code, client, epoch, index);
    if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::NeedToCreateWord)
    {
        sentence.Append(CandidateTextForOutput(GlobalIme::composition.creating_word.word));
        GlobalIme::composition.clear_creating_word();
        GlobalIme::composition.selection_history.clear();
    }
    if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
    {
        sentence.Append(wstring_to_string(Global::candidate_ui.selected_text));
        ResetSentenceSpelling();
    }
}
bool FinishSentenceSpelling(uint64_t client, uint64_t epoch)
{
    // Consume the highlighted resolved candidate, including partial selections.
    // Never translate raw pinyin or manufacture Chinese from it.
    for (int i = 0; i < 128 && !g_inputSession->get_pinyin_sequence_with_cases().empty(); ++i)
    {
        const auto before = g_inputSession->get_pinyin_sequence_with_cases();
        SelectSentencePart(VK_SPACE, client, epoch);
        if (before == g_inputSession->get_pinyin_sequence_with_cases())
            return false;
    }
    return g_inputSession->get_pinyin_sequence_with_cases().empty();
}
} // namespace
bool TranslationBufferDeliveryPending(uint64_t client, uint64_t epoch)
{
    return sentence_delivery && sentence_client == client && sentence_epoch == epoch;
}
void ClearTranslationSentenceBuffer()
{
    if (sentence.translating)
        AiTranslation::Instance().Cancel();
    sentence.Clear();
    sentence_client = sentence_epoch = sentence_generation = sentence_delivery = 0;
}
std::wstring TranslationBufferDisplay()
{
    std::string text = sentence.text;
    if (GlobalIme::composition.creating_word.active)
        text += CandidateTextForOutput(GlobalIme::composition.creating_word.word);
    if (text.empty() && !sentence.translating && sentence.status.empty())
        return {};
    return L"待翻译：" + string_to_wstring(text) +
           (sentence.status.empty() ? L"" : L"\n" + string_to_wstring(sentence.status));
}
void BufferSelectedCandidate(uint64_t client, uint64_t epoch)
{
    if (!ClientNegotiatedTranslationBuffer(client) || !GetConfiguredAiAssistant().translation_enabled)
        return;
    if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::NeedToCreateWord)
    {
        sentence.Append(CandidateTextForOutput(GlobalIme::composition.creating_word.word));
        GlobalIme::composition.clear_creating_word();
        GlobalIme::composition.selection_history.clear();
    }
    if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
    {
        sentence.Append(wstring_to_string(Global::candidate_ui.selected_text));
        ResetSentenceSpelling();
    }
    sentence_client = client;
    sentence_epoch = epoch;
    PublishSentence(client, epoch);
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::TranslationBufferState;
    Global::candidate_ui.selected_text = L"1";
}
bool DeliverSentence(const std::wstring &text)
{
    const uint64_t delivery = ++next_sentence_delivery;
    const auto payload = std::to_wstring(delivery) + L"\t" + text;
    if (payload.size() > FanyImeVoiceCompositionPipe::kMaxSnapshotChars ||
        !SendVoiceCompositionToTsfWorker(sentence_client, sentence_epoch,
                                         Global::DataFromServerMsgTypeToTsfWorkerThread::CommitTranslationBuffer,
                                         payload))
        return false;
    sentence_delivery = delivery;
    sentence.translating = true;
    sentence.status = "正在上屏…";
    return true;
}
bool ApplyTranslationBufferResult(AiTranslation::Result result)
{
    if (!GetConfiguredAiAssistant().translation_enabled || !sentence.translating ||
        result.generation != sentence_generation || !AiTranslation::Instance().IsCurrent(result.generation) ||
        result.request.client_id != sentence_client || result.request.activation_epoch != sentence_epoch ||
        !IsPipeActivationCurrent(sentence_client, sentence_epoch))
        return true;
    if (result.translation.empty())
        sentence.Fail();
    else if (DeliverSentence(string_to_wstring(result.translation)))
    {
        // Original stays intact until the TSF text store confirms insertion.
    }
    else
        sentence.Fail();
    PublishSentence(sentence_client, sentence_epoch);
    return true;
}
bool HandleTranslationBufferKey(uint64_t client, uint64_t epoch, uint64_t request)
{
    const auto &config = GetConfiguredAiAssistant();
    if (Global::Keycode == FanyImeProtocol::TranslationCommitAckKey)
    {
        if (sentence_delivery && request == sentence_delivery && client == sentence_client && epoch == sentence_epoch)
        {
            sentence_delivery = 0;
            if (Global::Wch == 1)
            {
                sentence.Clear();
                ResetSentenceSpelling();
            }
            else
                sentence.Fail();
            PublishSentence(client, epoch);
        }
        return true;
    }
    if (!ClientNegotiatedTranslationBuffer(client) || !g_inputSession || !config.translation_enabled ||
        g_authoritative_cn_mode == 0 || g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji)
        return false;
    if (sentence_client && (sentence_client != client || sentence_epoch != epoch))
        ClearTranslationSentenceBuffer();
    sentence_client = client;
    sentence_epoch = epoch;
    // Each key carries a fresh caret extent: no local TSF composition is necessary.
    ::ReadDataFromNamedPipe(0b001000);
    const UINT code = Global::Keycode;
    const WCHAR wch = Global::Wch;
    const bool ctrl_enter = FanyImeIpc::IsTranslationCommitKey(code, Global::ModifiersDown);
    const bool final_command = code == VK_RETURN;
    const auto cancel_request = [&] {
        if (sentence.translating)
        {
            AiTranslation::Instance().Cancel();
            sentence.CancelRequest();
        }
    };
    if (sentence_delivery)
    {
        // Do not edit or resend while an insertion is awaiting its receipt.
    }
    else if (sentence.translating && ctrl_enter)
    {
        // A repeated command is only an acknowledgement, never another Submit.
    }
    else if (code == VK_ESCAPE)
    {
        cancel_request();
        if (!g_inputSession->get_pinyin_sequence_with_cases().empty())
            ResetSentenceSpelling(); // first Esc drops the current unselected spelling
        else
            sentence.Clear(); // second Esc cancels the selected sentence
    }
    else if (final_command)
    {
        if (!ctrl_enter)
            cancel_request();
        if (FinishSentenceSpelling(client, epoch))
        {
            if (ctrl_enter)
            {
                if (!AiTranslation::IsChineseSource(sentence.text))
                    sentence.Fail();
                else if (sentence.Begin(true))
                {
                    sentence_request.config = config;
                    sentence_request.config.translation_enabled = true;
                    sentence_request.source = sentence.text;
                    sentence_request.target = config.translation_target_language;
                    sentence_request.character_set = GetConfiguredCharacterSet();
                    sentence_request.context = SnapshotAiTranslationContext();
                    sentence_request.client_id = client;
                    sentence_request.activation_epoch = epoch;
                    // The sole translation submission point in the entire input pipeline.
                    sentence_generation = AiTranslation::Instance().Submit(sentence_request);
                }
            }
            else if (!sentence.text.empty())
            {
                const std::wstring original = string_to_wstring(sentence.text);
                if (DeliverSentence(original))
                {
                }
                else
                    sentence.status = "上屏失败，原文已保留";
            }
        }
    }
    else
    {
        cancel_request();
        const auto raw = g_inputSession->get_pinyin_sequence_with_cases();
        const bool plain_digit = code >= '1' && code <= '9' && (Global::ModifiersDown & 7u) == 0;
        if (code == VK_SPACE || (plain_digit && !raw.empty()))
        {
            if (!raw.empty())
                SelectSentencePart(code, client, epoch);
            else
                sentence.Append(code == VK_SPACE ? " " : wstring_to_string(std::wstring(1, wch)));
        }
        else if (code == VK_BACK && raw.empty())
            sentence.Backspace();
        else if (code == VK_UP || code == VK_DOWN)
        {
            Global::candidate_ui.move_selection(code == VK_UP ? -1 : 1);
        }
        else if (code == VK_TAB || code == VK_PRIOR || code == VK_NEXT)
            FanyNamedPipe::MoveCandidatePage(code == VK_PRIOR || (code == VK_TAB && (Global::ModifiersDown & 1u)) ? -1
                                                                                                                  : 1);
        else if ((code >= 'A' && code <= 'Z') || code == VK_BACK || code == VK_DELETE || code == VK_LEFT ||
                 code == VK_RIGHT || IsManualPinyinSeparatorKey(code, wch) ||
                 IsMicrosoftShuangpinIngKey(code, wch, raw) || IsMidSentenceHelpcodeMarkerKey(code, wch, raw))
        {
            bool restored = false;
            ApplyCompositionEditKey(code, wch, Global::ModifiersDown, true, restored);
            GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
            SyncShuangpinPreeditForms();
            Global::PinyinString = string_to_wstring(g_inputSession->get_pinyin_sequence_with_cases());
            if (!g_inputSession->get_pinyin_sequence_with_cases().empty())
                FanyNamedPipe::PrepareCandidateList(client, epoch);
            else
                Global::candidate_ui.set_items({});
        }
        else if (wch >= L' ')
        {
            if (FinishSentenceSpelling(client, epoch))
            {
                wchar_t punctuation = wch;
                switch (wch)
                {
                case L',':
                    punctuation = L'，';
                    break;
                case L'.':
                    punctuation = L'。';
                    break;
                case L'?':
                    punctuation = L'？';
                    break;
                case L'!':
                    punctuation = L'！';
                    break;
                case L';':
                    punctuation = L'；';
                    break;
                case L':':
                    punctuation = L'：';
                    break;
                case L'\\':
                    punctuation = L'、';
                    break;
                }
                sentence.Append(wstring_to_string(std::wstring(1, punctuation)));
            }
        }
    }
    PublishSentence(client, epoch);
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::TranslationBufferState;
    Global::candidate_ui.selected_text =
        sentence.text.empty() && g_inputSession->get_pinyin_sequence_with_cases().empty() ? L"0" : L"1";
    FanyNamedPipe::SendCurrentDataToClient(client, epoch, request);
    return true;
}
} // namespace event_listener_detail

namespace FanyNamedPipe
{
struct ScopedServerKeyLatency
{
    uint64_t client_id;
    uint64_t activation_epoch;
    uint64_t request_id;
    ULONGLONG started_at_ms = GetTickCount64();

    ~ScopedServerKeyLatency()
    {
        const ULONGLONG elapsed_ms = GetTickCount64() - started_at_ms;
        if (elapsed_ms >= 8)
        {
            DIAG_LOGF(L"[key-latency] side=server stage=handle request={} client={} epoch={} elapsed_ms={}", request_id,
                      client_id, activation_epoch, elapsed_ms);
        }
    }
};

// 把候选框换回 Ctrl+Enter 或选中「📅日期」之前的那一屏。子页期间输入串一个字都没动，session
// 里的候选还是原来那批，所以这里直接把存下来的 items / 页码 / 高亮位放回去即可；随后这颗按键
// 继续走它本来的流程，就像子页从来没出现过一样。
void ExitCandidateSubPage()
{
    if (!IsCandidateSubPageActive())
    {
        return;
    }
    g_translation_candidates_active = false;
    g_date_time_page_active = false;
    g_date_time_page_keyword.clear();
    auto &ui = Global::candidate_ui;
    ui.set_items(std::move(g_translation_saved_items));
    g_translation_saved_items.clear();
    ui.page_index = g_translation_saved_page_index;
    ui.selected_index_in_page = g_translation_saved_selected_index;
    g_translation_saved_page_index = 0;
    g_translation_saved_selected_index = 0;
    RefreshCandidatePageUi(false);
}

// 混输里的「📅日期」入口：把候选框换成这组的全部格式（顺序与 Shift+T 模式同一份），空格/数字键
// 照常选一条上屏，其它键先换回原来那一屏（见 HandleImeKey）。
bool EnterDateTimeCandidatePage(const std::string &keyword)
{
    if (IsCandidateSubPageActive())
    {
        return false;
    }
    auto items = OrderedDateTimeCandidates(keyword);
    if (items.empty())
    {
        return false;
    }
    auto &ui = Global::candidate_ui;
    g_translation_saved_items = ui.items;
    g_translation_saved_page_index = ui.page_index;
    g_translation_saved_selected_index = ui.selected_index_in_page;
    ui.set_items(std::move(items));
    g_date_time_page_active = true;
    g_date_time_page_keyword = keyword;
    RefreshCandidatePageUi(true);
    return true;
}

void RebuildDateTimeCandidatePage()
{
    if (!g_date_time_page_active)
    {
        return;
    }
    auto &ui = Global::candidate_ui;
    const int page_index = ui.page_index;
    const int selected_index = ui.selected_index_in_page;
    ui.set_items(OrderedDateTimeCandidates(g_date_time_page_keyword));
    if (page_index > 0 && page_index * ui.page_size < static_cast<int>(ui.items.size()))
    {
        ui.page_index = page_index;
    }
    ui.selected_index_in_page = selected_index;
    RefreshCandidatePageUi(true);
}

// Ctrl+Enter：上屏高亮候选右边的那条译文（副候选）。只有一条译义就直接上屏；有多条时把
// 候选框整个换成这几条译义，空格/数字键照常选一条上屏（见 ProcessSelectionKey 的译文分支）。
void HandleTranslationCommitKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    // 拿不到译文时回 NavigationIgnored：这颗键已经被 TSF 吃掉了，必须给一条回复，
    // 而且这条回复既不上屏也不给 wch 补标点。
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::NavigationIgnored;
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    if (IsCandidateSubPageActive() || IsUiLessMode() || japanese ||
        (!GetConfiguredCandidateTranslationsEnabled() && !GetConfiguredAiAssistant().translation_enabled) ||
        Global::candidate_ui.items.empty())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    // 和数字/空格选词一样，先等画面追上已发布的那一页，否则取到的是用户没看见的那条译文。
    WaitForCandidateRenderSync(VK_RETURN);
    EnsureCandidatePageReady();

    auto &ui = Global::candidate_ui;
    const size_t index = static_cast<size_t>((std::max)(0, ui.selected_index_in_page));
    if (index >= ui.page_glosses.size())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }
    const auto senses = FanyImeIpc::SplitTranslationGloss(wstring_to_string(ui.page_glosses[index]));
    if (senses.empty())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    if (senses.size() == 1)
    {
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
        ui.selected_text = string_to_wstring(CandidateTextForOutput(senses.front()));
        // Normal/CommitExactText 的回复由 SendCurrentDataToClient 负责收尾（ClearState）。
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    g_translation_saved_items = ui.items;
    g_translation_saved_page_index = ui.page_index;
    g_translation_saved_selected_index = ui.selected_index_in_page;
    std::vector<WordItem> translation_items;
    translation_items.reserve(senses.size());
    for (const auto &sense : senses)
    {
        translation_items.emplace_back(std::string{}, sense, 0, CandidateSource::Fallback);
    }
    ui.set_items(std::move(translation_items));
    g_translation_candidates_active = true;
    RefreshCandidatePageUi(true);
    SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

// 真顶字与自动上屏的推送负载："<消费字符数>\t<上屏文本>"。TSF 拿这个数字裁自己的
// 组合缓冲，所以服务端看到的是四码、用户已抢敲第五个字母时，第五个字母不会被旧快照覆盖。
// 消费数就是五笔完整码的字母数（engine/schemes/wubi_scheme.h 的 kMaxCodeLength）。
constexpr std::size_t kWubiCompleteCodeLength = 4;
std::wstring BuildWubiCommitAndContinuePayload(const std::wstring &text)
{
    return std::to_wstring(kWubiCompleteCodeLength) + L"\t" + text;
}

// Bring the candidate page in step with the CompositionRestored frame just sent.
// TSF applies that payload without touching its candidate presenter, so the page
// on screen has to follow here -- and that covers every frame the reply branch
// sends, not just the retraction: the unit Backspace deletion shortens the raw
// and the Ctrl+arrow unit jump moves the caret prefix, and both would otherwise
// leave the pre-key page (and its selection) on screen.
//   - the raw is gone: nothing to show. The selected segments may still be on
//     screen (a segment Backspace emptied the raw but kept the word), where TSF
//     deliberately leaves its presenter alone because ending it would send
//     HideCandidateWnd and reset this very composition; and a Backspace that
//     ended the word must never fall through to a rebuild that would publish the
//     empty-input fallback candidate;
//   - the caret prefix is empty: hide too (R4, the same rule as the ShowCandidate
//     task and the caret-arrow path);
//   - otherwise rebuild from the engine -- the old page holds the pre-frame
//     items -- and re-highlight the restored pick.
void PublishRestoredCompositionCandidates(uint64_t client_id, uint64_t activation_epoch)
{
    // One-shot: consume it even when this outcome hides instead of rebuilding, so
    // a position recorded on a page that no longer exists cannot leak into a
    // later frame.
    const GlobalIme::RestoredSelectionHighlight restored_highlight =
        GlobalIme::composition.take_restored_selection_highlight();
    if (GlobalIme::composition.raw_input_with_cases.empty())
    {
        HideCandidateWindowAndDropItems();
        return;
    }
    if (FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                       g_inputSession->get_pinyin_sequence_with_cases().size()))
    {
        HideCandidateWindowAndDropItems();
        return;
    }

    PrepareCandidateList(client_id, activation_epoch);
    if (restored_highlight.absolute_index >= 0)
    {
        // A retraction re-highlights the item the user had picked on the rebuilt
        // page: no frequency update ran during the creating word, so a page
        // rebuilt for the same prefix still holds the same items in the same
        // order. A page for another prefix (the suffix was edited between the
        // pick and the retraction) does not, and the recorded position would land
        // on an unrelated candidate -- apply it only after the prefixes match.
        const std::string rebuilt_page_prefix = FanyImeIpc::NormalizeCandidatePagePrefix(
            g_inputSession->get_pinyin_sequence_with_cases(), g_inputSession->prefix_end());
        auto &ui = Global::candidate_ui;
        if (rebuilt_page_prefix == restored_highlight.page_prefix && ui.item_total_count > 0 && ui.page_size > 0)
        {
            const int position = std::min(restored_highlight.absolute_index, ui.item_total_count - 1);
            ui.page_index = position / ui.page_size;
            ui.selected_index_in_page = position % ui.page_size;
            RefreshCandidatePageUi(false);
        }
    }
    RequestShowCandidateWindow();
}

// Esc inside the creating-word shape (see HasEscapeCreatingWordShape). TSF is
// holding for a CompositionRestored frame, so it is sent in both outcomes:
//   - keep (ShouldEscapeKeepSelectedWord): drop only the unselected spelling
//     and leave the selected word alone on screen -- the R3 state a segment
//     Backspace already produces, except that the candidate window stays up
//     showing just the word as its preedit. The selection history stays, so a
//     later Backspace can still retract the word;
//   - otherwise reset everything like any other Esc; the empty payload makes
//     TSF cancel its composition.
void HandleCreatingWordEscape(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id,
                              const std::string &input_before_key)
{
    if (FanyImeIpc::ShouldEscapeKeepSelectedWord(GetConfiguredEscapeKeepsSelectedWord(), input_before_key.size(),
                                                 g_r_mode_triggered))
    {
        g_inputSession->set_pinyin_sequence("");
        g_inputSession->set_pinyin_sequence_with_cases("");
        g_inputSession->recompute_candidates();
        UpdateCloudInput("");
        UpdateEnglishInput("");
        UpdateMixedInput({});
        UpdateAiInput("");
        g_dedicated_english_answer_pending = false;
        ClearSpecialModeTriggers();
        auto &composition = GlobalIme::composition;
        composition.raw_input_with_cases.clear();
        composition.segmented_pinyin.clear();
        composition.caret_position = 0;
        composition.restored_selection_highlight = {};
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
        Global::candidate_ui.selected_text =
            BuildCreateWordPipePayload(std::string{}, composition.creating_word.word) + L"\t0";
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        if (GetConfiguredCandidateWindowPreeditStyle() == "empty")
        {
            // The window draws no preedit, so an empty page would be a blank card.
            HideCandidateWindowAndDropItems();
            return;
        }
        // Keep the window up with the word alone as its preedit (Weasel style): an
        // empty page, so the stale candidates of the dropped spelling go away.
        Global::CandidateString.clear();
        Global::candidate_ui.set_items({});
        RefreshCandidatePageUi(true);
        return;
    }

    PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
    ClearState();
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
    Global::candidate_ui.selected_text = L"\t\t\t0";
    SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

/**
 * @brief
 *
 * 调频、造词也都在这里处理。
 *
 */
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    const ScopedServerKeyLatency latency{client_id, activation_epoch, request_id};
    /* 先清理一下状态 */
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
    ::ReadDataFromNamedPipe(0b000111);

    // TSF classifies VK_NUMPAD0..9 as candidate digit keys. Keep the IPC
    // contract symmetric before any selection/composition predicates run.
    Global::Keycode = FanyImeIpc::NormalizeNumpadDigitKey(Global::Keycode);

    if (FanyImeProtocol::IsCharacterSetShortcut(Global::Keycode, Global::ModifiersDown))
    {
        if (g_authoritative_cn_mode != 0 && GetConfiguredCharacterSetShortcutEnabled())
        {
            const std::string previous = GetConfiguredCharacterSet();
            const std::string next = previous == "traditional" ? "simplified" : "traditional";
            // The packet's point[] is this key's badge anchor, not the
            // candidate anchor: read it locally and leave Global::Point alone.
            // Legacy clients leave the struct-default point there instead.
            if (SetConfiguredCharacterSet(next) && GetConfiguredCharacterSet() != previous &&
                ClientNegotiatedCaretStateIndicator(client_id))
            {
                PostCaretStateBadge(FanyImeUi::SingleStateBadge(FanyImeUi::CaretStateKind::CharacterSet,
                                                                GetConfiguredCharacterSet() == "traditional"),
                                    namedpipeData.point[0], namedpipeData.point[1]);
            }
        }
        return;
    }

    if (FanyImeIpc::IsEnglishModeToggleKey(Global::Keycode, Global::ModifiersDown))
    {
        SetEnglishInputMode(!g_english_input_mode);
        ClearState();
        return;
    }

    if (HandleTranslationBufferKey(client_id, activation_epoch, request_id))
        return;
    if (FanyImeIpc::IsTranslationCommitKey(Global::Keycode, Global::ModifiersDown))
    {
        HandleTranslationCommitKey(client_id, activation_epoch, request_id);
        return;
    }
    // 译文页、日期页只认选词和翻页/移动高亮。其它任何键都先把候选框换回原来那一屏，然后照常处理，
    // 所以退格、字母、回车、标点在子页上的表现和没打开过它时完全一致。
    if (IsCandidateSubPageActive())
    {
        // Shift 放行是给 Shift+Tab 上一页留的；Ctrl/Alt 组合一律退出。
        const bool stays_on_sub_page = (Global::ModifiersDown & 0b00000110u) == 0 &&
                                       (IsSelectionKey(Global::Keycode) || IsCandidateNavigationKey(Global::Keycode));
        if (!stays_on_sub_page)
        {
            ExitCandidateSubPage();
        }
    }

    if (g_r_mode_triggered && !GlobalIme::composition.raw_input_with_cases.empty() &&
        GlobalIme::composition.raw_input_with_cases.front() == 'R' && GlobalIme::composition.caret_position > 0)
    {
        // The published preedit has one extra display-only prefix. Normalize
        // the caret before every R-mode key, including paging and selection.
        --GlobalIme::composition.caret_position;
    }

    const std::string input_before_key =
        g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
    // 顶字要的是「插入之前」的原始串长度与光标位置：ApplyCompositionEditKey 会把第五个字母插进
    // 本地 raw 并把光标推到 5，之后再问就分不清「用户又敲了一个字母」和「本来就停在别处」。引擎
    // 随后会把 raw 裁回四码，这个快照是唯一能区分两者的地方（raw_length_before_key == 4 且光标
    // 在末尾 = 用户正在往后打，不是回来改码）。
    const std::size_t raw_length_before_key = input_before_key.size();
    const std::size_t caret_before_key = GlobalIme::composition.caret_position;
    const bool shift_only = (Global::ModifiersDown & 0b00000111u) == 0b00000001u;
    const bool chinese_scheme = g_inputSession && (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                                                   g_inputSession->current_scheme_type() == SchemeType::Shuangpin);
    if (Global::Keycode == VK_RETURN && !input_before_key.empty() && GetConfiguredEnterLearnsEnglishWord())
    {
        std::string english_word;
        const bool shift_letter_special_mode = IsShiftLetterSpecialModeTriggered();
        if (FanyImeIpc::ShouldLearnEnteredEnglishWord(g_english_input_mode, shift_letter_special_mode, chinese_scheme,
                                                      g_inputSession->is_all_complete_pure_pinyin()))
            english_word = g_r_mode_triggered ? "R" + input_before_key : input_before_key;
        EnqueueLearnEnteredEnglishWordTask(english_word);
    }
    if (chinese_scheme && !g_english_input_mode && GetConfiguredQuickPhraseEnabled() && input_before_key.empty() &&
        Global::Keycode == 'K' && Global::Wch == L'K' && shift_only)
        g_quick_phrase_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredUnicodeModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'U' && Global::Wch == L'U' && shift_only)
        g_unicode_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredDateTimeModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'T' && Global::Wch == L'T' && shift_only)
        g_date_time_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredEmojiModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'E' && Global::Wch == L'E' && shift_only)
        g_emoji_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredKaomojiModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'M' && Global::Wch == L'M' && shift_only)
        g_kaomoji_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredJianpinModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'J' && Global::Wch == L'J' && shift_only)
        g_jianpin_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredYModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'Y' && Global::Wch == L'Y' && shift_only)
        g_y_mode_triggered = true;
    const bool r_mode_trigger_key = chinese_scheme && !g_english_input_mode && GetConfiguredRModeEnabled() &&
                                    input_before_key.empty() && Global::Keycode == 'R' && Global::Wch == L'R' &&
                                    shift_only;
    if (r_mode_trigger_key)
    {
        g_r_mode_original_session = g_inputSession;
        g_inputSession = CreateTemporaryJapaneseInputSession();
        g_r_mode_triggered = true;
    }

    if (FanyImeIpc::HasEscapeCreatingWordShape(Global::Keycode, GlobalIme::composition.creating_word.active,
                                               IsUiLessMode(), ClientNegotiatedCompositionRestore(client_id)))
    {
        HandleCreatingWordEscape(client_id, activation_epoch, request_id, input_before_key);
        return;
    }

    if (FanyImeIpc::IsBackendIndependentCompositionResetKey(Global::Keycode))
    {
        // TSF completes/cancels the composition locally. Keep every backend in
        // lockstep, invalidate async candidates, and do not manufacture a
        // reply for this locally consumed key.
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        ClearState();
        return;
    }

    const bool unicode_composition_active = IsUnicodeCompositionActive(input_before_key);
    const bool is_paging_key = IsPagingKey(Global::Keycode);
    const bool is_manual_pinyin_separator = IsManualPinyinSeparatorKey(Global::Keycode, Global::Wch);
    const bool is_microsoft_shuangpin_ing_key =
        IsMicrosoftShuangpinIngKey(Global::Keycode, Global::Wch, input_before_key);
    const bool is_mid_sentence_helpcode_marker =
        IsMidSentenceHelpcodeMarkerKey(Global::Keycode, Global::Wch, input_before_key);
    // 日语模式下 '-' 是长音符输入键，既不翻页也不做词转字。
    const bool is_japanese_long_vowel = IsJapaneseLongVowelKey(Global::Keycode, Global::Wch);
    const int word_character_direction =
        FanyImeIpc::WordToCharacterDirection(Global::Keycode, Global::Wch, Global::ModifiersDown,
                                             GetConfiguredWordToCharacterEnabled() && !is_japanese_long_vowel,
                                             GetConfiguredWordToCharacterKeys() == "minus_equal");
    const bool is_commit_with_highlighted_candidate_punctuation =
        word_character_direction != 0 ||
        (!is_manual_pinyin_separator && !is_microsoft_shuangpin_ing_key && !is_mid_sentence_helpcode_marker &&
         IsCommitWithHighlightedCandidatePunctuationInCandidateMode(Global::Keycode, Global::Wch));
    const bool is_selection_key = IsSelectionKey(Global::Keycode);
    const bool is_unicode_shift_digit_selection =
        unicode_composition_active && shift_only && Global::Keycode >= '1' && Global::Keycode <= '9';
    const bool is_unicode_hex_digit = unicode_composition_active && !is_unicode_shift_digit_selection &&
                                      Global::Keycode >= '0' && Global::Keycode <= '9';
    const bool is_unicode_plus = unicode_composition_active && Global::Keycode == VK_OEM_PLUS && Global::Wch == L'+';
    const bool is_composition_edit_key =
        Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT || Global::Keycode == VK_BACK ||
        Global::Keycode == VK_DELETE || (Global::Keycode >= 'A' && Global::Keycode <= 'Z') ||
        is_manual_pinyin_separator || is_microsoft_shuangpin_ing_key || is_mid_sentence_helpcode_marker ||
        is_unicode_hex_digit || is_unicode_plus || is_japanese_long_vowel;
    const bool should_forward_key_to_session = !is_commit_with_highlighted_candidate_punctuation && !is_selection_key &&
                                               !is_paging_key && !is_composition_edit_key;

    // Punctuation needs a synchronous highlighted-candidate response on the TSF pipe.
    // Reply before cloud-query and candidate recomputation work so the TSF-side
    // timeout sentinel keeps its original meaning instead of masking latency here.
    if (is_commit_with_highlighted_candidate_punctuation)
    {
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
        const bool has_active_composition = g_inputSession != nullptr && !g_inputSession->get_pinyin_sequence().empty();
        if (has_active_composition)
        {
            EnsureCandidatePageReady();
            auto &ui = Global::candidate_ui;
            ui.selected_text = FanyImeIpc::HighlightedCandidateText(ui.page_words, ui.selected_index_in_page);

            WordItem highlighted_item;
            const bool highlighted_resolved = ResolveCandidateItem(ui.selected_index_in_page + 1, highlighted_item);
            if (highlighted_resolved && metasequoia::local_modes::is_date_time_menu_item(highlighted_item))
            {
                // 「📅日期」入口不是要上屏的字：高亮停在它上面时，标点带出本页首个候选。
                ui.selected_text = FanyImeIpc::HighlightedCandidateText(ui.page_words, 0);
            }
            else if (word_character_direction != 0 && highlighted_resolved)
            {
                const auto edge = word_character_direction < 0 ? FanyImeIpc::HanCharacterEdge::First
                                                               : FanyImeIpc::HanCharacterEdge::Last;
                const auto character =
                    FanyImeIpc::ExtractHanCharacter(CandidateTextForOutput(highlighted_item.word), edge);
                if (character)
                {
                    Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
                    ui.selected_text = string_to_wstring(*character);
                }
            }
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            ClearState();
        }
        return;
    }

    /* 先处理一下通用的按键，包括所有可能的按键，如普通的拼音字符按键、空格、Tab
     * 等等，然后再在下面处理其中的特殊的按键 */
    bool composition_restored = false;
    // The client arms its Backspace reply hold from its own creating-word mirror,
    // which mirrors the state before this key. Capture that shape here so the
    // reply below can still answer a Backspace that ends the word (the post-key
    // shape is gone by then, and the hold would otherwise burn its full timeout).
    bool retreat_backspace_shape_before_key = false;
    // 前缀重算让所有编辑键（含字母/Delete）都需要协商结果；段操作（Ctrl+Backspace /
    // Ctrl+方向）的键位与修饰键条件仍由各自的 chord 判定把守，这里放宽键位限制不影
    // 响它们。
    const bool client_supports_restore = is_composition_edit_key && ClientNegotiatedCompositionRestore(client_id);
    const bool r_mode_prefix_backspace = g_r_mode_triggered && Global::Keycode == VK_BACK && input_before_key.empty();
    if (r_mode_prefix_backspace)
    {
        ClearState();
    }
    else if (is_composition_edit_key && !r_mode_trigger_key)
    {
        retreat_backspace_shape_before_key =
            Global::Keycode == VK_BACK &&
            FanyImeIpc::HasRetreatBackspaceShape(GlobalIme::composition.creating_word.active, IsUiLessMode(),
                                                 client_supports_restore);
        ApplyCompositionEditKey(Global::Keycode, Global::Wch, Global::ModifiersDown, client_supports_restore,
                                composition_restored);
    }
    else if (should_forward_key_to_session)
    {
        g_inputSession->handle_key(Global::Keycode, Global::ModifiersDown, Global::Wch);
    }
    GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
    GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
    if (g_english_input_mode)
    {
        // English candidates are queried by the raw spelling. Do not expose
        // the Chinese pinyin session's syllable boundaries in the preedit.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (g_inputSession->get_pinyin_sequence_with_cases().empty() && !g_r_mode_triggered)
    {
        ClearSpecialModeTriggers();
    }
    if (!g_english_input_mode && g_r_mode_triggered)
    {
        // R is a visible mode prefix but is not part of the romaji sent to the
        // temporary Japanese engine. Keep both TSF and candidate-window preedit
        // aligned, including their caret coordinates.
        GlobalIme::composition.segmented_pinyin.insert(0, 1, 'R');
        GlobalIme::composition.raw_input_with_cases.insert(0, 1, 'R');
        ++GlobalIme::composition.caret_position;
    }
    if (!g_english_input_mode && IsUnicodeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed U/+hex sequence.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsDateTimeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsQuickPhraseCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed K-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsEmojiCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed E-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsKaomojiCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed M-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsJianpinCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed J-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsYModeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed Y-prefixed English.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    SyncShuangpinPreeditForms();

    // 五笔四码唯一自动上屏：敲满四码且码表只给一个候选时，直接走与空格完全相同的提交路径，
    // 用户不必再按一次空格。判定只发生在字母键插入之后（上面的 ApplyCompositionEditKey）：
    // 退格、方向键、composition_restored 等路径都不会到这里，所以「打满第四键就上屏」只有
    // 这一个入口。这是无条件行为，不读配置。
    const bool letter_key = Global::Keycode >= 'A' && Global::Keycode <= 'Z';
    if (!g_english_input_mode && letter_key &&
        FanyImeIpc::ShouldAutoCommitCompleteWubiCode(g_inputSession->wubi_unique_four_code(),
                                                     GlobalIme::composition.creating_word.active))
    {
        // 候选页是异步发布的：此刻 ui.items / ui.page_words 可能还停在第 3 码那一拍，而提交
        // 路径读的正是这两份数据。先按当前组合同步重建一次，否则会把上一拍的候选上屏。
        // forced_index_in_page = 0 让结算不进入渲染等待（与鼠标点击同类），自动上屏的语义
        // 是「这个码只有一个候选」，必须显式取 0 而不是跟随页内选择。
        PrepareCandidateList(client_id, activation_epoch);
        Global::candidate_ui.select_first_on_page();
        ProcessSelectionKey(VK_SPACE, client_id, activation_epoch, /*forced_index_in_page=*/0);
        if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
        {
            // 真上屏只能靠 worker 管道推送：字母键在默认 raw 预编辑样式下不读请求-回复管道，
            // 回一帧 Normal 既不会上屏，还会被 TSF 当成「不属于本次请求」的帧缓存起来，
            // 而本函数返回前 Server 已经清掉组合，两边就此分叉。推送携带消费的 4 个字符，
            // TSF 裁自己的缓冲；快打时用户已多敲的字母因此不会被旧快照覆盖。
            if (SendToTsfWorkerThreadClientViaNamedpipe(
                    client_id, activation_epoch,
                    Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidateAndContinue,
                    BuildWubiCommitAndContinuePayload(Global::candidate_ui.selected_text)))
            {
                ClearState();
                // 推送同样会引来 HideCandidateWnd；用户若已抢敲下一个字母，那时服务端组合
                // 就是这个字母，不能被这次 Hide 清掉。
                NoteTopCommitPushed(client_id, activation_epoch);
            }
        }
        // UILess 与 pinyin 预编辑样式会为字母键等一帧回复（上限 50ms）：给它们一帧免得空等；
        // raw 样式不读回复，塞一帧反而变成死帧。
        if (IsUiLessMode() || GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        return;
    }

    // 真顶字：完整四码（不论是否唯一）之后再敲一个字母时，先上屏该码的首选候选，再把这个字母
    // 留作下一次组合的开头——用户已经在打下一个字，字母绝不能丢。它不看自动上屏开关：开关
    // 只决定「唯一码要不要多敲一键才上屏」，不决定丢不丢输入。判定复用同一份引擎事实，
    // 但不要求唯一；上屏取候选 0（首选），不进入 30ms 渲染等待。
    if (!g_english_input_mode && letter_key && raw_length_before_key == kWubiCompleteCodeLength &&
        caret_before_key == raw_length_before_key &&
        FanyImeIpc::ShouldCommitCompleteWubiCodeOnNextKey(g_inputSession->wubi_four_code_is_complete(),
                                                          /*key_is_letter=*/true, /*caret_at_end=*/true,
                                                          GlobalIme::composition.creating_word.active))
    {
        PrepareCandidateList(client_id, activation_epoch);
        Global::candidate_ui.select_first_on_page();
        ProcessSelectionKey(VK_SPACE, client_id, activation_epoch, /*forced_index_in_page=*/0);
        const std::wstring committed_text = Global::candidate_ui.selected_text;

        // 用刚敲下的这个字母重建服务端组合。ProcessSelectionKey 已经把引擎与组合清空，这里
        // 把字母写回去；引擎此刻的 raw 仍是被裁回的四码，所以必须显式设置而不是继续追加。
        // 大小写照 ApplyCompositionEditKey 的同一套规则取，保持 preedit 与用户敲键一致。
        char next_char = static_cast<char>(Global::Keycode + ('a' - 'A'));
        if (Global::Wch >= L'A' && Global::Wch <= L'Z')
        {
            next_char = static_cast<char>(Global::Wch);
        }
        else if (Global::Wch >= L'a' && Global::Wch <= L'z')
        {
            next_char = static_cast<char>(Global::Wch);
        }
        const std::string next_raw(1, next_char);
        GlobalIme::composition.clear_creating_word();
        GlobalIme::composition.selection_history.clear();
        g_inputSession->set_pinyin_sequence(next_raw);
        g_inputSession->set_pinyin_sequence_with_cases(next_raw);
        g_inputSession->recompute_candidates();
        GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
        GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
        SyncShuangpinPreeditForms();
        GlobalIme::composition.caret_position = GlobalIme::composition.raw_input_with_cases.size();
        PrepareCandidateList(client_id, activation_epoch);
        // 组合被提交时 TSF 会送 HideCandidateWnd 把候选窗藏起来；顶字重建的新组合必须
        // 显式把窗口再请出来，否则后续整词的候选（xyyf 的统计）用户永远看不到。
        RequestShowCandidateWindow();

        // 推送与用户下一个按键是两条独立路径：TSF 裁的是它自己那一刻的缓冲，所以「服务端说
        // 消费 4 个、TSF 手里已经有 5 个」时，第 5 个自然留下来继续组词。服务端的组合也正好
        // 是同一批多出来的字母，两边都从同一条按键流派生，不会错位。不要 ClearState：重建的
        // 组合正是下一次按键要用的状态。
        if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
        {
            // 推送会让 DLL 结束旧组合，TSF 随之发来 HideCandidateWnd；标记本客户端的余码
            // 组合仍然存活，HideCandidate 处理器据此跳过 ClearState。推送失败就不会有这次 Hide，
            // 也就不记账。
            if (SendToTsfWorkerThreadClientViaNamedpipe(
                    client_id, activation_epoch,
                    Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidateAndContinue,
                    BuildWubiCommitAndContinuePayload(committed_text)))
            {
                NoteTopCommitPushed(client_id, activation_epoch);
            }
        }
        // UILess 与 pinyin 预编辑样式会为字母键等一帧回复（上限 50ms）。这里绝不能回 Normal
        // （SendCurrentDataToClient 会 ClearState，把刚重建的组合再清掉），只能回渲染帧。
        if (IsUiLessMode())
        {
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
            Global::candidate_ui.selected_text = GetTsfPreedit();
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        return;
    }

    //
    // 先判断要不要触发云联想
    // 判断依据：
    //  - 拼音序列长度是偶数
    //  - 最后一个字符不是大写字母
    //
    // Paging / selection must not bump async generations or re-apply cached
    // cloud/AI results (that previously reset page_index and re-cached duplicates).
    const bool suppress_async_lookup = is_paging_key || is_selection_key || is_unicode_shift_digit_selection;

    const auto cloud_query_state = g_inputSession->get_cloud_query_state();
    if (!g_english_input_mode && !suppress_async_lookup &&
        !IsSpecialModeCompositionActive(g_inputSession->get_pinyin_sequence_with_cases()) &&
        cloud_query_state.should_query)
    {
        UpdateCloudInput(cloud_query_state.query_text, client_id, activation_epoch);
    }

    const bool ai_eligible = !g_english_input_mode &&
                             !IsSpecialModeCompositionActive(g_inputSession->get_pinyin_sequence_with_cases()) &&
                             (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                              g_inputSession->current_scheme_type() == SchemeType::Shuangpin) &&
                             g_inputSession->is_all_complete_pure_pinyin() && !g_inputSession->has_active_helpcode() &&
                             !GlobalIme::composition.creating_word.active;
    if (!suppress_async_lookup)
    {
        UpdateAiInput(ai_eligible ? g_inputSession->get_pinyin_segmentation() : std::string{}, client_id,
                      activation_epoch);
    }

    // The shape the key leaves behind: a Backspace that keeps the creating word
    // alive still owes the client's hold a frame even when nothing was restored
    // (a plain deletion behind the edit lock, or a no-op).
    const bool retreat_backspace_shape_after_key =
        Global::Keycode == VK_BACK && FanyImeIpc::HasRetreatBackspaceShape(GlobalIme::composition.creating_word.active,
                                                                           IsUiLessMode(), client_supports_restore);

    //
    // 普通的拼音字符，发送 preedit 到 TSF 端
    //
    if (FanyImeIpc::ShouldSendCompositionReply(Global::Keycode >= 'A' && Global::Keycode <= 'Z',
                                               is_manual_pinyin_separator, is_microsoft_shuangpin_ing_key,
                                               is_unicode_hex_digit, is_unicode_plus, is_japanese_long_vowel))
    {
        if (IsUiLessMode())
        {
            PrepareCandidateList(client_id, activation_epoch);
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
            {
                std::wstring preedit = GetTsfPreedit();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
                Global::candidate_ui.selected_text = preedit;
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
        }
    }
    else if (Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE || composition_restored)
    {
        if (IsUiLessMode())
        {
            if (g_inputSession->get_pinyin_sequence().empty())
            {
                ClearState();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::UiLessComposition;
                Global::candidate_ui.selected_text = L"\t";
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
            else
            {
                PrepareCandidateList(client_id, activation_epoch);
                SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
            }
        }
        else if (FanyImeIpc::ShouldAnswerRetreatBackspace(composition_restored, retreat_backspace_shape_before_key,
                                                          retreat_backspace_shape_after_key))
        {
            // Unlike an ordinary deletion, the retraction and the unit caret
            // jump are not mirrored by TSF on its own: for a deletion TSF
            // rebuilds its keystroke buffer from this payload, and for a jump
            // it applies the caret field. It must therefore be sent in both
            // preedit styles, and the trailing caret field pins the
            // authoritative caret.
            //
            // Every Backspace the client may be holding for gets this frame --
            // retreat, plain character deletion behind the edit lock, or a
            // no-op behind an empty history: the DLL arms its hold from the
            // creating-word mirror it saw before the key (word_for_creating_word),
            // and without this frame the default raw preedit style would send no
            // reply at all, burning the hold's full 50 ms timeout. The pre-key
            // shape keeps that promise for the Backspace that deletes the last
            // raw character and ends the word: the post-key shape is gone by
            // then, while the client is still holding. The payload then
            // describes the state the key left behind -- restored, unchanged, or
            // one character shorter when the caret deletion in
            // ApplyCompositionEditKey ran -- which is what the hold applies in
            // every outcome. The candidate page follows it below.
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
            Global::candidate_ui.selected_text = BuildCreateWordPipePayload(GlobalIme::composition.raw_input_with_cases,
                                                                            GlobalIme::composition.creating_word.word) +
                                                 L'\t' + std::to_wstring(GlobalIme::composition.caret_position);
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
            PublishRestoredCompositionCandidates(client_id, activation_epoch);
        }
        else if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            if (!g_inputSession->get_pinyin_sequence().empty())
            {
                std::wstring preedit = GetTsfPreedit();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
                Global::candidate_ui.selected_text = preedit;
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
        }
    }
    else if (IsUiLessMode() && is_composition_edit_key && Global::Keycode != VK_LEFT && Global::Keycode != VK_RIGHT &&
             Global::Keycode != VK_BACK)
    {
        PrepareCandidateList(client_id, activation_epoch);
        SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
    }

    //
    // 在以下情况下，TSF 端会请求候选字符串
    //  - 空格，会上屏第一个候选项
    //  - 数字，会上屏相应序号对应的候选项
    //
    // 空格和数字键可能会触发造词，如果数字键上屏的汉字字符串所对应的拼音比实际的拼音要短的话，
    // 那么，就可能会触发造词事件，那么，就要适时改变候选框的状态
    //
    /* VK_SPACE, Digits (U-mode: Shift+1..9) */
    if (Global::Keycode == VK_SPACE || is_unicode_shift_digit_selection ||
        (!IsUnicodeCompositionActive(GlobalIme::composition.raw_input_with_cases) && Global::Keycode > '0' &&
         Global::Keycode <= '9'))
    {
        ProcessSelectionKey(Global::Keycode, client_id, activation_epoch);
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
    }
    else if (Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT)
    {
        if (IsUiLessMode())
        {
            PrepareCandidateList(client_id, activation_epoch);
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            // R2/R4：前缀重算生效时光标移动会改变候选内容——前缀为空则收起候选窗，
            // 其余（含移回串尾）一律从引擎重读重建页面。移回串尾时引擎已按整串重算，
            // 但页面 items 还是旧前缀候选，只刷新页面会让那批旧候选参与结算（真机回归：
            // ni'hao'ya 右移回串尾后空格只上屏「你好」+「ya」）。整串解码（未启用或未协商）
            // 维持只刷新页面的现状（R7/AC8 零差异）。
            const std::string caret_raw = g_inputSession->get_pinyin_sequence_with_cases();
            const std::size_t prefix_end = g_inputSession->prefix_end();
            const bool caret_resegmentation = FanyImeIpc::ShouldResegmentCompositionByCaret(
                client_supports_restore, IsUiLessMode(), g_english_input_mode,
                IsSpecialModeCompositionActive(caret_raw));
            switch (FanyImeIpc::ResolveCaretArrowCandidatePublish(caret_resegmentation, prefix_end, caret_raw.size()))
            {
            case FanyImeIpc::CaretArrowCandidatePublish::Hide:
                HideCandidateWindowAndDropItems();
                break;
            case FanyImeIpc::CaretArrowCandidatePublish::RebuildFromEngine:
                // 窗口可能因之前的前缀为空状态被收起（单音节后缀从 caret=0 右移两次），
                // 必须显式请求显示；PrepareCandidateList 末尾自带 RefreshCandidatePageUi(false)。
                PrepareCandidateList(client_id, activation_epoch);
                RequestShowCandidateWindow();
                break;
            case FanyImeIpc::CaretArrowCandidatePublish::RefreshPageOnly:
                RefreshCandidatePageUi(true);
                break;
            }
        }
    }
    else if (IsCandidateNavigationKey(Global::Keycode) && !is_unicode_plus)
    {
        auto &ui = Global::candidate_ui;
        UINT result = Global::DataFromServerMsgType::NavigationIgnored;
        bool refresh = false;

        const auto move_page = [&](int offset, UINT response_type) {
            result = response_type;
            // Keyboard paging keeps the in-page selection where it is; the wheel
            // path in WorkerThread is the one that restarts it at the top.
            if (MoveCandidatePage(offset) != PageMoveResult::Unchanged)
            {
                refresh = true;
            }
        };
        const auto move_selection = [&](int offset, UINT response_type) {
            result = response_type;
            if (offset > 0 && (ui.is_selection_at_last_candidate() ||
                               (ui.is_selection_at_current_page_end() && ui.is_next_page_partial_last_page())))
            {
                ExpandCandidatesKeepingPagePosition();
            }
            if (ui.move_selection(offset))
            {
                refresh = true;
            }
        };

        const bool shift_down = (Global::ModifiersDown & 0b00000001u) != 0;
        if (Global::Keycode == VK_OEM_MINUS && GetConfiguredPagingMinusEqualEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_PLUS && GetConfiguredPagingMinusEqualEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_OEM_COMMA && GetConfiguredPagingCommaPeriodEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_PERIOD && GetConfiguredPagingCommaPeriodEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_OEM_4 && GetConfiguredPagingBracketsEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_6 && GetConfiguredPagingBracketsEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_TAB && GetConfiguredPagingTabEnabled())
        {
            move_page(shift_down ? -1 : 1, shift_down ? Global::DataFromServerMsgType::MovePagePrevious
                                                      : Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_PRIOR && GetConfiguredPagingPageUpDownEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_NEXT && GetConfiguredPagingPageUpDownEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (GetConfiguredCandidateArrowNavigationEnabled() &&
                 (Global::Keycode == VK_UP || Global::Keycode == VK_DOWN))
        {
            if (Global::Keycode == VK_UP)
            {
                move_selection(-1, Global::DataFromServerMsgType::MoveSelectionPrevious);
            }
            else
            {
                move_selection(1, Global::DataFromServerMsgType::MoveSelectionNext);
            }
        }

        if (IsUiLessMode())
        {
            if (refresh)
            {
                RefreshCandidatePageUi(false);
            }
            else
            {
                EnsureCandidatePageReady();
            }
            // Prefer selection index in page for host-drawn lists.
            if (!ui.page_words.empty())
            {
                ui.selected_index_in_page =
                    std::clamp(ui.selected_index_in_page, 0, static_cast<int>(ui.page_words.size()) - 1);
            }
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            Global::MsgTypeToTsf = result;
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
            if (refresh)
            {
                RefreshCandidatePageUi(true);
            }
        }
    }
}
} // namespace FanyNamedPipe
