#include "../date_time_input.h"
#include "../direct_helpcode.h"
#include "../ipc_negotiation.h"
#include "../mid_sentence_helpcode.h"
#include "../trilingual_input.h"
#include "../v_mode_input.h"
#include "../voice_composition_pipe.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

#define CHECK(expression)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            std::cerr << #expression << " at " << __LINE__ << '\n';                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

int main()
{
    // Wire bytes, not just matching C++ declarations: existing v1 DLLs write
    // these offsets on both x86 and x64. Changes must not move UTF-16 data.
    std::array<unsigned char, 304> bytes{};
    bytes[0] = 10; // ClientHello
    bytes[8] = 7;  // client id
    FanyImeNamedpipeData legacy{};
    std::memcpy(&legacy, bytes.data(), bytes.size());
    CHECK(legacy.client_id == 7);
    CHECK(FanyImePipeEventType::HideCaretState == 16);
    CHECK(FanyImePipeEventType::HideCaretState != FanyImePipeEventType::HideCandidateWnd);
    CHECK(!FanyImePipeEventType::IsRouteDeactivation(FanyImePipeEventType::HideCaretState));
    CHECK(FanyImeProtocol::CaretStateIndicator == (1u << 5));
    CHECK((FanyImeProtocol::CaretStateIndicator & FanyImeProtocol::RequiredCapabilities) == 0);
    // CapsLockEdge keeps the VK_CAPITAL value the first Servers matched on.
    CHECK(FanyImeCaretStateTrigger::UserToggle == 0);
    CHECK(FanyImeCaretStateTrigger::CapsLockEdge == 0x14);
    CHECK(FanyImeCaretStateTrigger::FocusEntered != FanyImeCaretStateTrigger::UserToggle);
    CHECK(FanyImeCaretStateTrigger::FocusEntered != FanyImeCaretStateTrigger::CapsLockEdge);
    CHECK(FanyImeProtocol::Negotiate(legacy).legacy);
    CHECK(FanyImeProtocol::Negotiate(legacy).accepted);

    const auto capsOffSnapshot = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(false);
    const auto capsOnSnapshot = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(true);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotPresent & FanyImePipeFlags::UiLess) == 0);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled & FanyImePipeFlags::UiLess) == 0);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotPresent & 0xffu) == 0);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled & 0xffu) == 0);
    CHECK(FanyImePipeFlags::HasImeSwitchCapsLockSnapshot(capsOffSnapshot));
    CHECK(!FanyImePipeFlags::ImeSwitchCapsLockSnapshotEnabled(capsOffSnapshot));
    CHECK(FanyImePipeFlags::HasImeSwitchCapsLockSnapshot(capsOnSnapshot));
    CHECK(FanyImePipeFlags::ImeSwitchCapsLockSnapshotEnabled(capsOnSnapshot));
    CHECK(!FanyImePipeFlags::HasImeSwitchCapsLockSnapshot(0)); // old DLL packet
    CHECK(!FanyImePipeFlags::ImeSwitchCapsLockSnapshotEnabled(FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled));
    CHECK(FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOffSnapshot).has_value());
    CHECK(!*FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOffSnapshot));
    CHECK(*FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOnSnapshot));
    CHECK(!FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(0).has_value());
    CHECK(
        !FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled).has_value());

    auto hello = FanyImeProtocol::Hello(7, 19);
    auto result = FanyImeProtocol::Negotiate(hello);
    CHECK(result.accepted && !result.legacy);
    auto reply = FanyImeProtocol::Reply(hello, result);
    CHECK(FanyImeProtocol::AcceptReply(reply, 19));
    CHECK(!FanyImeProtocol::AcceptReply(reply, 18)); // stale reconnect ACK
    CHECK(!FanyImeProtocol::AcceptReply(reply, 0));

    const auto shortcutCapabilities = FanyImeProtocol::Capabilities | FanyImeProtocol::CharacterSetShortcut;
    const auto shortcutHello = FanyImeProtocol::Hello(7, 20, shortcutCapabilities);
    const auto oldServer = FanyImeProtocol::Negotiate(shortcutHello);
    CHECK(oldServer.accepted);
    CHECK((oldServer.capabilities & FanyImeProtocol::CharacterSetShortcut) == 0);
    const auto newServer = FanyImeProtocol::Negotiate(shortcutHello, shortcutCapabilities);
    CHECK(newServer.accepted);
    CHECK((newServer.capabilities & FanyImeProtocol::CharacterSetShortcut) != 0);
    const auto shortcutReply = FanyImeProtocol::Reply(shortcutHello, newServer);
    CHECK(FanyImeProtocol::AcceptReply(shortcutReply, 20));
    CHECK(FanyImeProtocol::ReplyCapabilities(shortcutReply) == shortcutCapabilities);
    CHECK((FanyImeProtocol::Negotiate(hello, shortcutCapabilities).capabilities &
           FanyImeProtocol::CharacterSetShortcut) == 0); // old client/new server
    CHECK(FanyImeProtocol::IsCharacterSetShortcut('F', 3));
    CHECK(FanyImeProtocol::IsCharacterSetShortcut('F', 0x80000003u));
    for (unsigned modifiers = 0; modifiers < 8; ++modifiers)
        CHECK(FanyImeProtocol::IsCharacterSetShortcut('F', modifiers) == (modifiers == 3));
    CHECK(!FanyImeProtocol::IsCharacterSetShortcut('E', 3));

    // Backspace retraction is negotiated separately: an old Server must keep
    // its current behavior, and an old DLL must never receive CompositionRestored.
    const auto restoreCapabilities = FanyImeProtocol::Capabilities | FanyImeProtocol::CompositionRestore;
    const auto restoreHello = FanyImeProtocol::Hello(7, 21, restoreCapabilities);
    const auto oldServerForRestore = FanyImeProtocol::Negotiate(restoreHello);
    CHECK(oldServerForRestore.accepted);
    CHECK((oldServerForRestore.capabilities & FanyImeProtocol::CompositionRestore) == 0);
    const auto newServerForRestore = FanyImeProtocol::Negotiate(restoreHello, restoreCapabilities);
    CHECK(newServerForRestore.accepted);
    CHECK((newServerForRestore.capabilities & FanyImeProtocol::CompositionRestore) != 0);
    CHECK(FanyImeProtocol::AcceptReply(FanyImeProtocol::Reply(restoreHello, newServerForRestore), 21));
    CHECK(FanyImeProtocol::ReplyCapabilities(FanyImeProtocol::Reply(restoreHello, newServerForRestore)) ==
          restoreCapabilities);
    CHECK((FanyImeProtocol::Negotiate(hello, restoreCapabilities).capabilities & FanyImeProtocol::CompositionRestore) ==
          0); // old client/new server
    CHECK(FanyImeReplyType::CompositionRestored == 14);
    CHECK(FanyImeReplyType::TrilingualCycle == 15);
    CHECK(FanyImeReplyType::MaxKnown == FanyImeReplyType::TrilingualCycle);
    CHECK(FanyImeReplyType::TransportUnavailable > FanyImeReplyType::MaxKnown);

    const auto cycleCapabilities = FanyImeProtocol::Capabilities | FanyImeProtocol::TrilingualCycle;
    const auto cycleHello = FanyImeProtocol::Hello(7, 22, cycleCapabilities);
    const auto oldServerForCycle = FanyImeProtocol::Negotiate(cycleHello);
    CHECK(oldServerForCycle.accepted);
    CHECK((oldServerForCycle.capabilities & FanyImeProtocol::TrilingualCycle) == 0);
    const auto newServerForCycle = FanyImeProtocol::Negotiate(cycleHello, cycleCapabilities);
    CHECK(newServerForCycle.accepted && !newServerForCycle.legacy);
    CHECK((newServerForCycle.capabilities & FanyImeProtocol::TrilingualCycle) != 0);
    CHECK(FanyImeProtocol::AcceptReply(FanyImeProtocol::Reply(cycleHello, newServerForCycle), 22));
    CHECK((FanyImeProtocol::Negotiate(hello, cycleCapabilities).capabilities & FanyImeProtocol::TrilingualCycle) == 0);
    CHECK((FanyImeProtocol::TrilingualCycle & FanyImeProtocol::RequiredCapabilities) == 0);

    {
        using FanyImeTrilingualInput::Mode;
        CHECK(FanyImeTrilingualInput::Next(true, false) == Mode::Japanese);
        CHECK(FanyImeTrilingualInput::Next(true, true) == Mode::English);
        CHECK(FanyImeTrilingualInput::Next(false, true) == Mode::Chinese);
        CHECK(FanyImeTrilingualInput::Next(false, false) == Mode::Chinese);
        // A plain VK_SHIFT is the binary toggle; only the explicit request bit cycles.
        CHECK(!FanyImeTrilingualInput::IsCycleRequest(0x10, 0));
        CHECK(!FanyImeTrilingualInput::IsCycleRequest(0x10, FanyImePipeFlags::UiLess));
        for (unsigned modifiers = 0; modifiers < 8; ++modifiers)
        {
            const auto request = FanyImeTrilingualInput::EncodeCycleRequest(true, false) | modifiers;
            CHECK(FanyImeTrilingualInput::IsCycleRequest(0x10, request) == (modifiers == 0));
            CHECK(!FanyImeTrilingualInput::IsCycleRequest(0x09, request));
        }
        CHECK(FanyImeTrilingualInput::IsCycleRequest(0x10, FanyImeTrilingualInput::EncodeCycleRequest(false, true) |
                                                               FanyImePipeFlags::UiLess));
        CHECK(!FanyImeTrilingualInput::IsCycleRequest(0x20, FanyImeTrilingualInput::EncodeCycleRequest(true, true)));
        // The request bits stay clear of the Shift/Ctrl/Alt bits and every pipe flag.
        CHECK((FanyImeTrilingualInput::CycleRequestMask & 7u) == 0);
        CHECK((FanyImeTrilingualInput::CycleRequestMask &
               (FanyImePipeFlags::UiLess | FanyImePipeFlags::ImeSwitchCapsSnapshotPresent |
                FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled)) == 0);
        for (const bool ime : {false, true})
        {
            for (const bool japanese : {false, true})
            {
                const auto request = FanyImeTrilingualInput::EncodeCycleRequest(ime, japanese);
                CHECK(FanyImeTrilingualInput::CycleRequestImeEnabled(request) == ime);
                CHECK(FanyImeTrilingualInput::CycleRequestJapanese(request) == japanese);
                // The requester's state, not a Server global, picks the destination.
                CHECK(FanyImeTrilingualInput::Destination(ime, japanese, true, !japanese) ==
                      FanyImeTrilingualInput::Next(ime, japanese));
            }
        }
        // Turned off while queued: answer as the binary toggle.
        CHECK(FanyImeTrilingualInput::Destination(true, true, false, false) == Mode::English);
        CHECK(FanyImeTrilingualInput::Destination(true, false, false, true) == Mode::English);
        CHECK(FanyImeTrilingualInput::Destination(false, true, false, false) == Mode::Chinese);
        CHECK(FanyImeTrilingualInput::Destination(false, false, false, true) == Mode::Japanese);
        Mode destination = Mode::Chinese;
        std::wstring text;
        CHECK(FanyImeTrilingualInput::ParsePayload(FanyImeTrilingualInput::BuildPayload(Mode::Japanese, L"你好"),
                                                   destination, text));
        CHECK(destination == Mode::Japanese && text == L"你好");
        CHECK(FanyImeTrilingualInput::ParsePayload(L"2\t", destination, text));
        CHECK(destination == Mode::English && text.empty());
        CHECK(FanyImeTrilingualInput::ParsePayload(L"0\tかな\tword", destination, text));
        CHECK(destination == Mode::Chinese && text == L"かな\tword");
        for (const auto *invalid : {L"", L"0", L"3\ttext", L"00\ttext", L"1|text", L"\ttext"})
        {
            CHECK(!FanyImeTrilingualInput::ParsePayload(invalid, destination, text));
        }
        const std::wstring embeddedNull(L"1\ttext\0tail", 11);
        CHECK(!FanyImeTrilingualInput::ParsePayload(embeddedNull, destination, text));
    }

    hello.wch += 1;
    result = FanyImeProtocol::Negotiate(hello);
    CHECK(!result.accepted);
    CHECK(!FanyImeProtocol::AcceptReply(FanyImeProtocol::Reply(hello, result), 19));
    hello = FanyImeProtocol::Hello(7, 19);
    hello.point[1] |= 1u << 20; // client requires an unknown capability
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);
    hello = FanyImeProtocol::Hello(7, 19);
    hello.modifiers_down |= 1u << 20; // unknown optional capabilities are ignored
    CHECK(FanyImeProtocol::Negotiate(hello).accepted);
    hello.modifiers_down &= ~FanyImeProtocol::FocusEpochs;
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);
    hello = FanyImeProtocol::Hello(7, 19);
    hello.keycode ^= 1;
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);
    hello = FanyImeProtocol::Hello(7, 0);
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);

    FanyImeNamedpipeDataToTsf old_server{};
    old_server.msg_type = FanyImeReplyType::PipeReady;
    old_server.request_id = 19;
    CHECK(!FanyImeProtocol::AcceptReply(old_server, 19));
    // Corrupt/missing negotiated capabilities cannot authorize the new DLL.
    reply.candidate_string[2] = 0;
    CHECK(!FanyImeProtocol::AcceptReply(reply, 19));

    CHECK(FanyImeWorkerReplyType::SwitchToEn == FanyImeWorkerReplyType::SwitchToEnglish);
    CHECK(FanyImeWorkerReplyType::CommitCandidate == FanyImeWorkerReplyType::CommitCurCandidate);
    CHECK(FanyImeWorkerReplyType::CommitCandidateAndContinue == 27);
    CHECK(FanyImeWorkerReplyType::MidSentenceHelpcodeChanged == 28);
    CHECK(FanyImeWorkerReplyType::MidSentenceHelpcodeSemicolonChanged == 29);
    CHECK(FanyImeWorkerReplyType::DirectHelpcodeChanged == 30);
    CHECK(FanyImeWorkerReplyType::MidSentenceHelpcodeUppercaseChanged == 31);
    CHECK(FanyImeWorkerReplyType::VModeChanged == 32);
    CHECK(FanyImeWorkerReplyType::TrilingualCycleChanged == 33);
    CHECK(FanyImeWorkerReplyType::MaxKnown == FanyImeWorkerReplyType::TrilingualCycleChanged);
    // V 模式的形状规则，TSF（WCHAR）与 Server（char）共用。
    {
        using FanyImeVModeInput::Trigger;
        const auto v_at = [](const std::wstring &text, std::size_t caret, wchar_t ch, Trigger trigger) {
            return FanyImeVModeInput::AcceptsAt(text.data(), text.size(), caret, ch, trigger);
        };
        const auto v_end = [&](const std::wstring &text, wchar_t ch, Trigger trigger) {
            return v_at(text, text.size(), ch, trigger);
        };
        CHECK(v_end(L"V", L'1', Trigger::UppercaseOnly) && v_end(L"V", L'(', Trigger::UppercaseOnly));
        CHECK(!v_end(L"V", L'+', Trigger::UppercaseOnly) && !v_end(L"V", L'.', Trigger::AnyCase));
        CHECK(v_end(L"V1", L'+', Trigger::UppercaseOnly) && v_end(L"V1+2", L'*', Trigger::AnyCase));
        CHECK(v_end(L"V12", L'.', Trigger::AnyCase) && v_end(L"V(1", L')', Trigger::AnyCase));
        CHECK(!v_end(L"v", L'1', Trigger::UppercaseOnly) && v_end(L"v", L'1', Trigger::AnyCase));
        CHECK(!v_end(L"V", L'1', Trigger::Off) && !v_end(L"vi", L'1', Trigger::AnyCase));
        CHECK(!v_end(L"V1", L'a', Trigger::AnyCase) && !v_end(L"T1", L'1', Trigger::AnyCase));
        // 光标在中间：不能把运算符插到 V 和第一个数字之间。
        CHECK(!v_at(L"V12", 1, L'+', Trigger::AnyCase) && v_at(L"V12", 1, L'3', Trigger::AnyCase) &&
              v_at(L"V12", 2, L'+', Trigger::AnyCase) && !v_at(L"V12", 0, L'3', Trigger::AnyCase));
        CHECK(!FanyImeVModeInput::IsComposition(L"V", 1, Trigger::UppercaseOnly) &&
              !FanyImeVModeInput::IsComposition(L"v", 1, Trigger::AnyCase) &&
              FanyImeVModeInput::IsComposition(L"v1", 2, Trigger::AnyCase) &&
              !FanyImeVModeInput::IsComposition(L"v1", 2, Trigger::UppercaseOnly) &&
              FanyImeVModeInput::IsComposition("V1a", 3, Trigger::UppercaseOnly) &&
              !FanyImeVModeInput::IsComposition("vip", 3, Trigger::AnyCase));
        CHECK(FanyImeVModeInput::TriggerFromPayload(L'2') == Trigger::AnyCase &&
              FanyImeVModeInput::TriggerFromPayload(L'1') == Trigger::UppercaseOnly &&
              FanyImeVModeInput::TriggerFromPayload(L'x') == Trigger::Off &&
              FanyImeVModeInput::PayloadFromTrigger(Trigger::UppercaseOnly) == L'1');
    }
    // 直接辅助码的 / 与 ; 形状规则，TSF（WCHAR）与引擎（char）共用。
    const auto slash_at = [](const std::wstring &text, std::size_t caret) {
        return FanyImeDirectHelpcode::AcceptsSlashAt(text.data(), text.size(), caret);
    };
    CHECK(slash_at(L"uiab", 4) && slash_at(L"x;ab", 4) && !slash_at(L"uia", 3) && !slash_at(L"ui'b", 4));
    CHECK(!slash_at(L"uiab/", 4) && slash_at(L"uiabui", 4) && !slash_at(L"uiab", 5));
    CHECK(FanyImeDirectHelpcode::AcceptsSemicolonFinalAt("uiax", 4, 4) &&
          !FanyImeDirectHelpcode::AcceptsSemicolonFinalAt("uix;", 4, 4) &&
          !FanyImeDirectHelpcode::AcceptsSemicolonFinalAt("", 0, 0));
    // DirectHelpcodeChanged 的载荷：开关和 / 勾没勾。旧 DLL 认的 "0"/"1" 意思不变。
    CHECK(FanyImeDirectHelpcode::PayloadFor(false, true) == L'0' &&
          FanyImeDirectHelpcode::PayloadFor(true, true) == L'1' &&
          FanyImeDirectHelpcode::PayloadFor(true, false) == L'2');
    CHECK(FanyImeDirectHelpcode::EnabledFromPayload(L'2') && !FanyImeDirectHelpcode::SlashFromPayload(L'2') &&
          FanyImeDirectHelpcode::SlashFromPayload(L'1') && !FanyImeDirectHelpcode::EnabledFromPayload(L'0') &&
          !FanyImeDirectHelpcode::IsValidPayload(L'3'));
    // TSF（WCHAR）与引擎（char）共用同一条句中辅助码形状规则。
    const auto accepts = [](const std::wstring &text) {
        return FanyImeMidSentenceHelpcode::AcceptsMarker(text.data(), text.size());
    };
    CHECK(accepts(L"ulpb") && !accepts(L"ulp") && !accepts(L"") && !accepts(L"ulpb'"));
    CHECK(!accepts(L"ulpb`") && !accepts(L"ulpb`x") && !accepts(L"ulpb`xY") && !accepts(L"ulpb`xi"));
    CHECK(accepts(L"ulpb`xih") && !accepts(L"ulpb`xihf") && accepts(L"ulpb`xYih") && accepts(L"ulpb`xihfa"));
    CHECK(FanyImeMidSentenceHelpcode::AcceptsMarker("ni'hc", 5) &&
          !FanyImeMidSentenceHelpcode::AcceptsMarker("ni'h", 4));
    // 光标移回句中：只看光标前的部分，光标后已是这个音节的段时不收。
    const auto accepts_at = [](const std::wstring &text, std::size_t caret) {
        return FanyImeMidSentenceHelpcode::AcceptsMarkerAt(text.data(), text.size(), caret);
    };
    CHECK(accepts_at(L"ulpbih", 2) && accepts_at(L"ulpbih", 4) && accepts_at(L"ulpbih", 6));
    CHECK(!accepts_at(L"ulpbih", 0) && !accepts_at(L"ulpbih", 3) && !accepts_at(L"ulpbih", 7));
    CHECK(!accepts_at(L"ul`xpb", 2) && accepts_at(L"ul`xpb", 6) && !accepts_at(L"ul`xpb", 3));
    // 大写触发：完整音节后的大写字母自己开一段（≡ 反引号 + 这个字母），紧跟的大写字母是第二码。
    const auto upper_marker = [](const std::wstring &text) {
        return FanyImeMidSentenceHelpcode::AcceptsMarker(text.data(), text.size(), true);
    };
    CHECK(!upper_marker(L"ulX") && upper_marker(L"ulXpb") && !upper_marker(L"ulXp") && upper_marker(L"ulXYpb"));
    // 关着时大写字母只是普通的一个键：ulXpb 是 5 键（奇数），ulXpbi 是 6 键。
    CHECK(!accepts(L"ulXpb") && accepts(L"ulXpbi"));
    const auto upper_at = [](const std::wstring &text, std::size_t caret) {
        return FanyImeMidSentenceHelpcode::AcceptsMarkerAt(text.data(), text.size(), caret, true);
    };
    CHECK(!upper_at(L"ulXpb", 2) && upper_at(L"ulXpb", 5) && upper_at(L"ulpb", 2));
    const auto second_at = [](const std::wstring &text, std::size_t caret, bool upper) {
        return FanyImeMidSentenceHelpcode::AcceptsSecondCodeAt(text.data(), text.size(), caret, upper);
    };
    CHECK(second_at(L"ulX", 3, true) && !second_at(L"ulXY", 4, true) && !second_at(L"ulX", 3, false));
    CHECK(second_at(L"ul`x", 4, false) && !second_at(L"ul`xY", 5, false) && !second_at(L"ul", 2, true));
    const auto semicolon_at = [](const std::wstring &text, bool upper) {
        return FanyImeMidSentenceHelpcode::AcceptsSemicolonFinalAt(text.data(), text.size(), text.size(), upper);
    };
    // 关着时与原来的规则一致（只看最后一个 '）；开着时大写段不算键：nihkXb 里 b 后面接 ; 是 b; 音节。
    CHECK(semicolon_at(L"nihkb", false) && !semicolon_at(L"nihkXb", false) && semicolon_at(L"nihkXb", true));
    CHECK(semicolon_at(L"ni'b", false) && !semicolon_at(L"nihk", true));
    // Shift+T 指定日期时间的形状规则，TSF（WCHAR）、Server 与引擎（char）共用。
    const auto date_time_at = [](const std::wstring &text, std::size_t caret, wchar_t ch) {
        return FanyImeDateTimeInput::AcceptsAt(text.data(), text.size(), caret, ch);
    };
    const auto date_time_end = [&](const std::wstring &text, wchar_t ch) {
        return date_time_at(text, text.size(), ch);
    };
    CHECK(date_time_end(L"T", L'2') && date_time_end(L"T2024122", L'5') && date_time_end(L"T20241225", L'1'));
    CHECK(date_time_end(L"T2024122514", L':') && date_time_end(L"T2024122514:30", L':'));
    CHECK(!date_time_end(L"T2024122514:30:00", L'1') && !date_time_end(L"T20241225", L':'));
    CHECK(date_time_end(L"T2024", L'/') && date_time_end(L"T2024/1", L'2') && date_time_end(L"T2024/12", L'/'));
    CHECK(!date_time_end(L"T2024/12/25", L'1') && !date_time_end(L"T2024/12/25", L'/'));
    CHECK(date_time_end(L"T12", L'/') && date_time_end(L"T9", L':') && date_time_end(L"T9:0", L'5'));
    CHECK(!date_time_end(L"T123", L'/') && !date_time_end(L"T123", L':') && !date_time_end(L"T12/25", L':'));
    CHECK(!date_time_end(L"T", L'/') && !date_time_end(L"T", L':') && !date_time_end(L"Trq", L'1'));
    CHECK(!date_time_end(L"U12", L'3') && !date_time_end(L"T2024", L'a') && !date_time_end(L"T", L'!'));
    // 光标在中间：插入后的整串仍要能接成某种形状；光标不能停在开头的 T 前面。
    CHECK(!date_time_at(L"T2024/12", 1, L'1') && date_time_at(L"T202412", 5, L'1') && !date_time_at(L"T2", 0, L'1'));
    CHECK(FanyImeDateTimeInput::MatchComplete("20241225", 8).shape == FanyImeDateTimeInput::Shape::Date);
    CHECK(FanyImeDateTimeInput::MatchComplete("2024122514", 10).shape == FanyImeDateTimeInput::Shape::DateTime);
    CHECK(FanyImeDateTimeInput::MatchComplete("2024122514:30:05", 16).group_count == 3);
    CHECK(FanyImeDateTimeInput::MatchComplete("2024/1/5", 8).shape == FanyImeDateTimeInput::Shape::YearMonthDay);
    CHECK(FanyImeDateTimeInput::MatchComplete("2024/12", 7).shape == FanyImeDateTimeInput::Shape::YearMonth);
    CHECK(FanyImeDateTimeInput::MatchComplete("12/25", 5).shape == FanyImeDateTimeInput::Shape::MonthDay);
    CHECK(FanyImeDateTimeInput::MatchComplete("9:05", 4).shape == FanyImeDateTimeInput::Shape::Time);
    CHECK(FanyImeDateTimeInput::MatchComplete("1430", 4).shape == FanyImeDateTimeInput::Shape::None);
    CHECK(FanyImeDateTimeInput::MatchComplete("12/", 3).shape == FanyImeDateTimeInput::Shape::None);
    CHECK(FanyImeDateTimeInput::MatchComplete("9:5", 3).shape == FanyImeDateTimeInput::Shape::None);
    static_assert(FanyImeDateTimeInput::AcceptsAt("T2024", 5, 5, '/'), "the date shape must be usable at compile time");
    const std::wstring voice(1000, L'x');
    const auto frames = FanyImeVoiceCompositionPipe::EncodeSnapshot(voice, 7);
    CHECK(FanyImeVoiceCompositionPipe::AssembleFrames(frames) == voice);
    auto incomplete = frames;
    incomplete.erase(incomplete.begin());
    CHECK(FanyImeVoiceCompositionPipe::AssembleFrames(incomplete).empty());
    // A middle frame legally carries flags 0, so an unterminated packet must be rejected on the chunk, not accepted
    // because data[0] happens to be a NUL.
    std::array<wchar_t, FanyImeVoiceCompositionPipe::kPacketChars> packet{};
    packet.fill(L'x');
    packet[0] = 0; // middle frame
    packet[1] = 7; // generation
    CHECK(!FanyImeVoiceCompositionPipe::ParseFrame(packet.data()).valid);
    packet[FanyImeVoiceCompositionPipe::kPacketChars - 1] = 0;
    const auto middle = FanyImeVoiceCompositionPipe::ParseFrame(packet.data());
    CHECK(middle.valid && !middle.first && !middle.last && middle.generation == 7);
    CHECK(middle.chunk == std::wstring(FanyImeVoiceCompositionPipe::kMaxChunkChars, L'x'));
    const auto blank = FanyImeVoiceCompositionPipe::EncodeSnapshot(std::wstring(), 7);
    CHECK(blank.size() == 1);
    std::array<wchar_t, FanyImeVoiceCompositionPipe::kPacketChars> header{};
    std::copy(blank[0].begin(), blank[0].end(), header.begin());
    const auto empty = FanyImeVoiceCompositionPipe::ParseFrame(header.data());
    CHECK(empty.valid && empty.first && empty.last && empty.chunk.empty());

    // Statistics frames: fixed 28-byte header plus 24-byte events; the Go stats
    // tool mirrors these numbers in internal/frames, so both sides must change
    // together.
    CHECK(FANY_IME_STATS_MAGIC == 0x54415453);
    CHECK(FANY_IME_STATS_VERSION == 1);
    CHECK(sizeof(FanyImeStatsBatchHeader) == 28);
    CHECK(sizeof(FanyImeStatsEvent) == 24);
    CHECK(offsetof(FanyImeStatsEvent, timestamp_utc_ft) == 0);
    CHECK(offsetof(FanyImeStatsEvent, cjk) == 8);
    CHECK(sizeof(FanyImeStatsBatchHeader) + 256 * sizeof(FanyImeStatsEvent) <= FANY_IME_STATS_MAX_FRAME_BYTES);
    std::cout << "Windows IPC layout, negotiation, upgrade, voice and stats contracts passed\n";
}
