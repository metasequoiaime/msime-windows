#pragma once

#include "voice_overlay_layout.h"
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

class WaveOverlay
{
  public:
    enum class CompactStatus
    {
        None,
        Recognizing,
        Processing,
    };

    enum class Action
    {
        Cancel,
        Confirm,
    };

    WaveOverlay();
    ~WaveOverlay();

    WaveOverlay(const WaveOverlay &) = delete;
    WaveOverlay &operator=(const WaveOverlay &) = delete;

    // Create and destroy on the UI thread.
    bool init(HINSTANCE instance, std::function<void(Action)> action_handler);
    void shutdown();

    // Publish visibility without waiting for the UI thread. Queued messages apply the latest state.
    void show();
    void hide();

    // Thread-safe. Pass the anchor captured once at recording start; an invalid anchor restores
    // the foreground-monitor fallback used by standalone fixtures.
    void set_anchor(const VoiceOverlayAnchor &anchor);

    void set_listening(bool listening);
    void set_input_level(float level);
    void set_transcript(const std::wstring &text);
    // When false, the overlay stays compact and never paints live ASR text.
    void set_show_transcript(bool show);
    void set_compact_status(CompactStatus status);
    void set_actions_visible(bool visible);

  private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void apply_visibility();
    bool ensure_render_target();
    bool ensure_text_layout(const std::wstring &transcript, float width, float height);
    void release_render_target();
    void release_text_layout();
    void update_wave_levels();
    void draw();
    VoiceOverlayAnchor resolve_anchor() const;
    void refresh_anchor_for_display_change();
    void update_dpi_scale(const VoiceOverlayAnchor &anchor);
    void update_window_bounds();
    bool hit_test_action(float x, float y, Action &action) const;

  private:
    static constexpr int kLevelCount = 12;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    // Safe publication target for background callers; cleared before DestroyWindow.
    std::atomic<HWND> post_hwnd_{nullptr};
    std::atomic<bool> visibility_requested_{false};
    UINT dpi_ = 96;
    float scale_x_ = 1.0f;
    float scale_y_ = 1.0f;

    std::atomic<bool> listening_{false};
    std::atomic<float> input_level_{0.0f};
    std::atomic<bool> show_transcript_{true};
    std::atomic<CompactStatus> compact_status_{CompactStatus::None};
    std::atomic<bool> actions_visible_{false};
    Action pressed_action_ = Action::Confirm;
    bool action_pressed_ = false;
    std::function<void(Action)> action_handler_;
    std::mutex transcript_mutex_;
    std::wstring transcript_;
    mutable std::mutex anchor_mutex_;
    VoiceOverlayAnchor anchor_;
    float levels_[kLevelCount] = {};
    float amplitudes_[kLevelCount] = {};
    float phases_[kLevelCount] = {};
    float freqs_[kLevelCount] = {};
    bool light_theme_ = false;

    struct ID2D1Factory *factory_ = nullptr;
    struct ID2D1HwndRenderTarget *render_target_ = nullptr;
    struct ID2D1SolidColorBrush *bar_brush_ = nullptr;
    struct ID2D1SolidColorBrush *bg_brush_ = nullptr;
    struct ID2D1SolidColorBrush *border_brush_ = nullptr;
    struct ID2D1SolidColorBrush *action_bg_brush_ = nullptr;
    struct IDWriteFactory *write_factory_ = nullptr;
    struct IDWriteTextFormat *text_format_ = nullptr;
    struct IDWriteTextFormat *processing_text_format_ = nullptr;
    struct IDWriteTextLayout *text_layout_ = nullptr;
    std::wstring layout_source_transcript_;
};
