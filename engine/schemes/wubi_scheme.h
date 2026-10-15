#pragma once

#include "input_scheme.h"
#include <string>
#include <vector>

class WubiScheme : public IInputScheme
{
  public:
    void reset() override;
    void handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch) override;
    QueryRequest build_request() const override;
    std::string get_preedit() const override;
    SchemeType type() const override;
    void set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases) override;
    // A wubi code is four letters, and a fifth is normally refused. Mixed input lifts that only
    // once the table has failed the code in hand, so a spelling long enough to need it -- nihao,
    // women, zhongguo -- can be finished. A code the table answers keeps the four-letter limit and
    // a fluent wubi typist sees no change.
    void set_extended_length_allowed(bool allowed);
    // No wubi code uses z, so the key is normally dropped. Mixed input has to accept it, because z
    // is an ordinary pinyin letter: dropping it does not refuse the spelling, it silently turns it
    // into a different one -- zhongguo would arrive as hongguo and zi as i. Unlike the length, this
    // follows the setting rather than the last query, since the letter can open a composition.
    void set_mixed_pinyin_allowed(bool allowed);
    // The wildcard mode wants the same key for the opposite reason: no code contains z, so a z
    // stands for a letter the typist does not know. Same acceptance, different query -- see
    // QueryRequest::wubi_z_wildcard. Independent of mixed input: a host can enable either, both or
    // neither.
    void set_z_wildcard(bool enabled);
    // The host says the next raw string is a special mode's literal input (T mode's Tdate,
    // V mode's V1+2), not a code. The raw string with cases is then kept exactly as typed, and the
    // code derived from it is never complete and never queried: a mode's keyword can normalize to a
    // real code (Tdate -> tdat), and treating that as wubi would commit a word and end the mode.
    // Cleared by reset(); a host that drives the session by raw string sets it with every string.
    void set_literal_input(bool literal);
    bool literal_input() const
    {
        return literal_input_;
    }
    // A complete wubi code is the four letters the table is indexed by. Mixed input only lifts the
    // length limit after the table has already failed the code in hand, so "complete" stays the
    // fixed four letters rather than max_code_length() (32 while extended input is on): a longer
    // spelling is a pinyin answer and is never a complete wubi code.
    bool has_complete_code() const
    {
        return !literal_input_ && raw_input_.size() == kMaxCodeLength;
    }

  private:
    static constexpr size_t kMaxCodeLength = 4;
    // Long enough for any spelling, short enough to stay a composition rather than a paragraph.
    static constexpr size_t kMaxMixedCodeLength = 32;

    size_t max_code_length() const;

    // For a code: the code letters as typed, cases kept, so lowercasing it gives raw_input_ exactly
    // -- same letters, same length, same clipping. A host that counts this string to find the fourth
    // letter counts code letters. For literal input: the string as typed, so a host reading it back
    // still sees the uppercase T or V prefix and the digits its mode decisions match on. Queries
    // never see it: they use raw_input_, the normalized code below.
    std::string raw_input_with_cases_;
    std::string raw_input_;
    std::vector<KeyStroke> key_strokes_;
    bool extended_length_allowed_ = false;
    bool mixed_pinyin_allowed_ = false;
    bool z_wildcard_ = false;
    bool literal_input_ = false;
};
