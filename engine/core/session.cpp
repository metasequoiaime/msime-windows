#include "../include/metasequoia/session.h"
#include "input_session.h"
#include <stdexcept>

namespace metasequoia
{
class Session::Impl
{
  public:
    explicit Impl(const SessionOptions &options) : session(options.scheme, options.shuangpin_profile, options.paths)
    {
        session.set_quanpin_autocorrect_types(options.autocorrect_types);
        session.set_fuzzy_pinyin_options(options.fuzzy_pinyin);
        session.set_quanpin_helpcode_enabled(options.helpcode);
        session.set_shuangpin_helpcode_enabled(options.helpcode);
        if (!session.set_helpcode_schema(options.helpcode_schema) ||
            !session.set_frequency_adjustment(options.frequency) || !session.set_english_input_options(options.english))
            throw std::invalid_argument("Invalid session options");
        session.set_local_mode_options(options.local_modes);
        session.set_mixed_expressive_options(options.expressive);
        session.set_wubi_input_options(options.wubi);
        session.enable_fixed_positions();
    }
    InputSession session;
};

Session::Session(SessionOptions options)
{
    options.paths.validate();
    impl_ = std::make_unique<Impl>(options);
    impl_->session.set_chinese_punctuation_enabled(options.chinese_punctuation);
    impl_->session.set_candidate_learning_enabled(options.learning);
}
Session::~Session() = default;
KeyResult Session::character(char value, bool shift_only)
{
    return impl_->session.handle_character(value, shift_only);
}
KeyResult Session::command(Command value)
{
    return impl_->session.handle_command(value);
}
KeyResult Session::candidate_key(char value)
{
    return impl_->session.handle_candidate_key(value);
}
KeyResult Session::punctuation(char value)
{
    return impl_->session.handle_punctuation(value);
}
void Session::set_chinese_punctuation_enabled(bool enabled)
{
    impl_->session.set_chinese_punctuation_enabled(enabled);
}
KeyResult Session::select(std::size_t index)
{
    return impl_->session.select_candidate(index);
}
KeyResult Session::select_edge(std::size_t index, CandidateEdge edge)
{
    return impl_->session.select_candidate_edge(index, edge);
}
KeyResult Session::pin(std::size_t index)
{
    return impl_->session.pin_candidate(index);
}
KeyResult Session::remove(std::size_t index)
{
    return impl_->session.remove_candidate(index);
}
KeyResult Session::fix_position(std::size_t index, int position)
{
    if (position < 1 || position > 5)
        return {};
    return impl_->session.set_candidate_position(index, position);
}
KeyResult Session::clear_position(std::size_t index)
{
    return impl_->session.set_candidate_position(index, 0);
}
KeyResult Session::finish()
{
    return finish(0);
}
KeyResult Session::finish(std::size_t first_index)
{
    return impl_->session.finish_composition(first_index);
}
void Session::switch_scheme(SchemeType scheme)
{
    impl_->session.switch_scheme(scheme);
}
bool Session::is_supported_helpcode_schema(const std::string &schema)
{
    return InputSession::is_supported_helpcode_schema(schema);
}
bool Session::set_helpcode_schema(const std::string &schema)
{
    return impl_->session.set_helpcode_schema(schema);
}
void Session::set_helpcode_enabled(bool enabled)
{
    impl_->session.set_quanpin_helpcode_enabled(enabled);
    impl_->session.set_shuangpin_helpcode_enabled(enabled);
}
void Session::set_dedicated_english(bool enabled)
{
    impl_->session.set_dedicated_english_mode(enabled);
}
void Session::set_wubi_mixed_pinyin(bool enabled)
{
    // 只改混输这一个开关：整份构造 WubiInputOptions 会把运行期已生效的 z 通配一起复位。
    metasequoia::WubiInputOptions options = impl_->session.wubi_input_options();
    options.mixed_pinyin = enabled;
    impl_->session.set_wubi_input_options(options);
}
SessionSnapshot Session::snapshot() const
{
    const auto &session = impl_->session;
    SessionSnapshot view{session.scheme(),
                         session.local_input_mode(),
                         session.preedit(),
                         session.raw_segmentation(),
                         session.normalized_segmentation(),
                         session.candidates(),
                         session.dedicated_english_mode(),
                         session.editing_text(),
                         session.caret_position()};
    return view;
}
std::optional<OnlineQuery> Session::online_query() const
{
    return impl_->session.online_query();
}
bool Session::apply_online_candidate(const OnlineQuery &query, std::string candidate, CandidateSource source)
{
    return impl_->session.apply_online_candidate(query, std::move(candidate), source);
}
} // namespace metasequoia
