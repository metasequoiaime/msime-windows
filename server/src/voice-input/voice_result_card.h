#pragma once

#include "voice_overlay_layout.h"
#include "voice_result_card_model.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Native result card that keeps the final voice transcription on screen after recording when there
// is no editable input target. It owns its own topmost layered popup; the WaveOverlay keeps serving
// the live/compact recording flow and is managed separately.
//
// Threading: init/shutdown run on the UI thread only. reset/show/close/set_anchor may be called from
// any thread — they update the desired model/anchor under a mutex and post a bare message; the UI
// thread always reads the latest complete values and never receives text or pointers in messages.
// Every ShowWindow, layout, D2D call and resource release happens on the UI thread.
//
// Placement: set_anchor() stores the monitor sampled when the recording started. As long as the
// anchor is valid the card never asks for the foreground monitor again, so focus moving to another
// display while ASR runs cannot move the card. A fixture that never calls set_anchor falls back to
// capturing the foreground monitor on each show.
class VoiceResultCard
{
  public:
    VoiceResultCard();
    ~VoiceResultCard();

    VoiceResultCard(const VoiceResultCard &) = delete;
    VoiceResultCard &operator=(const VoiceResultCard &) = delete;

    bool init(HINSTANCE instance);
    void shutdown();

    // Thread-safe. Pass the anchor captured once at recording start; an invalid anchor clears the
    // explicit anchor and restores the foreground-monitor fallback.
    void set_anchor(const VoiceOverlayAnchor &anchor);

    // New generation: hide and clear the current card. Older-generation calls are ignored.
    void reset(std::uint64_t session);
    // Show the final result for the current generation. Ignored when the session is stale, the
    // card was dismissed, or the text is empty.
    void show(std::uint64_t session, const std::wstring &text);
    // Dismiss the current generation unconditionally (programmatic cancel of the current recording
    // era). The UI's close button/right-click instead uses the atomic shown-verifying dismiss, so a
    // stale click can never close a newer card.
    void close();

  private:
    enum class Region
    {
        None,
        Close,
        Copy
    };
    enum class CopyFeedback
    {
        None,
        Copied,
        Failed
    };

    // 1 is the entrance-animation timer; each pending auto-close takes a fresh ID above it.
    static constexpr UINT_PTR kCopyAutoCloseTimerIdBase = 2;
    struct ButtonRect
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool ensure_render_target();
    void release_render_target();
    bool ensure_text_layout(const std::wstring &text, float width);
    void release_text_layout();
    void draw();
    void draw_header_check(float center_x, float center_y);
    void draw_close_icon(float center_x, float center_y);
    void draw_copy_icon(float center_x, float center_y, CopyFeedback feedback);

    VoiceOverlayAnchor resolve_anchor() const;
    void refresh_anchor_for_display_change();
    void update_dpi_scale(const VoiceOverlayAnchor &anchor);
    void update_card_geometry(const VoiceOverlayAnchor &anchor, float measured_body_height);
    void update_window_bounds();
    void update_close_button_rect();
    void update_copy_button_rect();
    void apply_desired_model();
    void dismiss_shown_card();
    void on_copy_button();
    void schedule_copy_auto_close(std::uint64_t session, const std::wstring &text);
    void cancel_copy_auto_close();
    void on_copy_auto_close_timer();
    void reset_press_state();
    void reset_hover_state();
    bool hit_test(float x_dip, float y_dip, Region &region) const;
    const wchar_t *copy_label() const;
    bool animations_enabled() const;
    void start_entrance_animation();
    void stop_entrance_animation(bool snap_to_target);
    void tick_entrance_animation();
    void hide_card();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    // Safe publication target for background callers; cleared before DestroyWindow.
    std::atomic<HWND> post_hwnd_{nullptr};
    UINT dpi_ = 96;
    float scale_x_ = 1.0f;
    float scale_y_ = 1.0f;

    struct ID2D1Factory *factory_ = nullptr;
    struct ID2D1HwndRenderTarget *render_target_ = nullptr;
    struct ID2D1SolidColorBrush *bg_brush_ = nullptr;
    struct ID2D1SolidColorBrush *border_brush_ = nullptr;
    struct ID2D1SolidColorBrush *text_brush_ = nullptr;
    struct ID2D1SolidColorBrush *secondary_text_brush_ = nullptr;
    struct ID2D1SolidColorBrush *header_text_brush_ = nullptr;
    struct ID2D1SolidColorBrush *hover_fill_brush_ = nullptr;
    struct ID2D1SolidColorBrush *accent_brush_ = nullptr;
    struct ID2D1SolidColorBrush *primary_fill_brush_ = nullptr;
    struct ID2D1SolidColorBrush *primary_hover_brush_ = nullptr;
    struct ID2D1SolidColorBrush *on_primary_brush_ = nullptr;
    struct IDWriteFactory *write_factory_ = nullptr;
    struct IDWriteTextFormat *title_text_format_ = nullptr;
    struct IDWriteTextFormat *body_text_format_ = nullptr;
    struct IDWriteTextFormat *button_text_format_ = nullptr;
    struct IDWriteTextLayout *text_layout_ = nullptr;
    std::wstring layout_source_text_;
    float layout_source_width_ = 0.0f;

    VoiceResultCardModel model_;
    mutable std::mutex anchor_mutex_;
    VoiceOverlayAnchor anchor_;

    // Current card geometry in DIP, derived from the captured monitor work area so the preferred
    // proportions are clamped to what the display can actually host.
    float card_width_dip_ = 390.0f;
    float card_height_dip_ = 196.14f;
    float body_height_dip_ = 48.14f;
    RECT target_window_rect_{};

    // UI-thread view of the latest applied model.
    std::uint64_t displayed_session_ = 0;
    std::wstring displayed_text_;
    bool card_visible_ = false;
    CopyFeedback feedback_ = CopyFeedback::None;
    // Pending auto-close after a successful copy. The session/text pair identifies the exact card
    // whose text was copied; close_if_current revalidates it under the model lock on the UI thread.
    // A fresh timer ID per schedule makes WM_TIMER messages already queued for a killed timer
    // harmless, so a repeated copy cannot be cut short by the previous dwell.
    std::uint64_t copy_close_session_ = 0;
    std::wstring copy_close_text_;
    UINT_PTR copy_close_timer_id_ = 0;
    UINT_PTR next_copy_close_timer_id_ = kCopyAutoCloseTimerIdBase;
    ButtonRect close_button_rect_;
    ButtonRect copy_button_rect_;
    float title_label_width_dip_ = 60.0f;
    float copy_label_width_dip_ = 0.0f;
    Region pressed_region_ = Region::None;
    Region hover_region_ = Region::None;
    bool region_pressed_ = false;
    bool tracking_mouse_ = false;
    float scroll_offset_ = 0.0f;

    // Entrance animation: 180 ms ease-out, layered alpha plus an 8 DIP upward settle. No animation
    // runs on text updates or while SPI_GETCLIENTAREAANIMATION is off.
    bool animation_active_ = false;
    ULONGLONG animation_start_ms_ = 0;
    RECT animation_from_rect_{};
};
