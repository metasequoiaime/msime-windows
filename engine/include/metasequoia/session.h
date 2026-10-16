#pragma once

#include "../../core/input_session_types.h"
#include "../../core/runtime_paths.h"
#include "../../shuangpin/shuangpin_profile.h"
#include "../../core/fuzzy_pinyin_options.h"
#include <memory>

namespace metasequoia
{
struct SessionOptions
{
    RuntimePaths paths;
    SchemeType scheme = SchemeType::Quanpin;
    ShuangpinProfile shuangpin_profile = GetXiaoheShuangpinProfile();
    std::string helpcode_schema = "lantian";
    // Quanpin autocorrection type mask (quanpin::kAutocorrect* bits); 0 keeps the
    // user's spelling untouched, which is the default for a fresh install.
    unsigned autocorrect_types = 0;
    bool helpcode = true;
    bool chinese_punctuation = true;
    bool learning = true;
    FuzzyPinyinOptions fuzzy_pinyin;
    FrequencyAdjustmentOptions frequency;
    LocalModeOptions local_modes;
    EnglishInputOptions english;
    MixedExpressiveOptions expressive;
    WubiInputOptions wubi;
};

struct SessionSnapshot
{
    SchemeType scheme;
    LocalInputMode local_mode;
    std::string preedit;
    std::string raw_segmentation;
    std::string normalized_segmentation;
    std::vector<WordItem> candidates;
    bool dedicated_english = false;
    // ASCII source text and offset, separate from rendered preedit (e.g. Japanese kana).
    std::string editing_text;
    std::size_t caret_position = 0;
};

// Stable platform entry point. One host serializes calls to its session; distinct sessions
// can run concurrently. Snapshots and online requests are values safe to hand to other threads.
// No SQLite, provider registry, raw key codes or mutable composition internals are exposed.
class Session
{
  public:
    explicit Session(SessionOptions options);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    KeyResult character(char value, bool shift_only = false);
    KeyResult command(Command value);
    KeyResult candidate_key(char value);
    KeyResult punctuation(char value);
    // Live host mode override; preserves composition, caret and punctuation pairing.
    // When disabled punctuation() is unhandled; the host owns ASCII passthrough.
    void set_chinese_punctuation_enabled(bool enabled);
    KeyResult select(std::size_t index);
    KeyResult select_edge(std::size_t index, CandidateEdge edge);
    // Explicit user action: promote a dictionary candidate without committing input.
    // Invalid/unsupported candidates are unhandled; persistence failures carry a diagnostic.
    KeyResult pin(std::size_t index);
    // Remove a dictionary phrase without committing; single-character non-English
    // candidates are protected. Invalid/unsupported selections are unhandled.
    KeyResult remove(std::size_t index);
    // Fix a dictionary candidate to slot 1..5 in this input context, or clear it.
    KeyResult fix_position(std::size_t index, int position);
    KeyResult clear_position(std::size_t index);
    KeyResult finish();
    // Finish the whole composition, starting with the host-highlighted candidate.
    // Remaining segments use their leading candidate; an invalid index commits raw input.
    KeyResult finish(std::size_t first_index);
    void switch_scheme(SchemeType scheme);
    static bool is_supported_helpcode_schema(const std::string &schema);
    bool set_helpcode_schema(const std::string &schema);
    // Apply the same enable flag to quanpin and shuangpin, as SessionOptions::helpcode does.
    // Hosts with per-scheme preferences apply the selected preference after switching.
    void set_helpcode_enabled(bool enabled);
    void set_dedicated_english(bool enabled);
    // 五笔拼音混输：同一串字母同时交给五笔码表和全拼，五笔候选在前、拼音候选追加在后。
    void set_wubi_mixed_pinyin(bool enabled);
    SessionSnapshot snapshot() const;
    std::optional<OnlineQuery> online_query() const;
    bool apply_online_candidate(const OnlineQuery &query, std::string candidate, CandidateSource source);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace metasequoia
