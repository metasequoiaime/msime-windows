# UI ownership

| Surface | Renderer | Owner |
|---|---|---|
| Candidate, floating toolbar, tray menu | msimeui native Direct2D/DirectWrite | `src/window/` presenters in the Server process |
| Settings | msimeui native Direct2D/DirectWrite | `src/settings/native/` in the separate `MetasequoiaImeSettings.exe` |
| Keyboard, handwriting, emoji and voice panels | Their existing native application/presenter | Shared actions/configuration stay in Server; panels do not own input sessions |

Direct2D is the only renderer. The WebView2 small windows, the WebView2 settings host and
the `ui-html/` pages have been removed, together with the `appearance.ui_backend` and
`appearance.settings_ui_backend` configuration keys; existing values are dropped by the
template merge on upgrade.

The candidate worker resolves output text, auxiliary codes, source badges and
translations once into `CandidateViewItem`, and the native renderer consumes these
values as plain text. Clicks enter the existing `EnqueueCandidateUiAction` queue with the
focus owner checks and action policy. The ten-entry mouse range is distinct from the
1–9 keyboard shortcuts. New business behavior belongs in shared actions/session/view
models, not in a presenter.

## Settings window

`MetasequoiaImeSettings.exe` draws its window with msimeui in `src/settings/native/`.
The window speaks the message contract in `engine/contracts/webview/` (the name is
historical): it receives `configSnapshot` and the response messages as JSON and posts
`configUpdate` and the request messages through `HandleClientMessage` in
`settings_app.cpp`, so config paths are mapped in one place. Its image assets live in
`server/assets/settings/` and are installed to `<data dir>\assets\settings\`.

The pages were ported one to one from the former HTML/CSS settings page: each page
builder in `native_page_*.cpp` names the partial and module it came from, and the
controls in `native_settings_controls.*` name the CSS rule they mirror. Those sources
remain in git history under `ui-html/webview2/settings/ime-settings/src/`.

Sciter is not an active product backend. The msimeui library owns generic controls and
rendering, never Server configuration, pipe sessions, dictionary access, or global
input state.

## v2 skins

The v2 nine-slice panel, free widgets and canvas insets only apply to `schema_version = 2` skins with `base = "canvas"` (see `skin/candidate_skin_catalog.h`); a v2 skin on another base loads as v1, and the settings page lists the canvas-only keys it ignored. `[animation]` and decoration `motion` are valid v2 keys on any base in the cross-platform schema; Windows does not play them yet and draws the skin still, without a notice. The fields are documented in `examples/skin-examples/schema/README.md`. `canvas` is the full-texture base: transparent panel, no stroke, square corners, no shadow by default. It is only usable as a base and is not part of `BuiltInIds()`.

- Geometry lives in `window/candidate_skin_layers.*` (pure arithmetic, no rendering) and is
  shared by the candidate window and the settings preview: `ResolveNineSlice` follows CSS
  `border-image` (slices in source-image pixels, widths in DIP); `ResolveContentPadding`
  measures `content_insets_dip` from the panel's outer edge and never below the border width;
  `SplitDecorations` stably sorts decorations and splits them at `kDecorationFrontZIndex` (1)
  into a layer below and a layer above the card.
- In `CandidatePresenter`'s scene the card and two `CandidateSkinOverlay` layers share one
  cell: back decorations, card, front decorations. Overlay layers measure 0 and do not hit
  test. The panel is drawn by `msimeui::Card::SetBackgroundNineSlice`; with a panel image the
  card's fill, stroke, radius and shadow are cleared. `line_gap_dip` becomes the preedit row's
  bottom margin, so list/pager spacing is unaffected. `outer_insets_dip` becomes extra window
  space around the card. `anchor` and `offset_*_dip`: positive values move right and down.
- Animation: images are fetched through `DeviceResources::GetImageFrame`. `Present` samples
  the animation clock with `BeginFrame`, renders, then reads `Scene::GetNextFrameDeadline()`
  and, only while the candidate window is shown, arms one one-shot coalescable timer
  (`kCandidateAnimationTimerId`) that presents again. Hiding, a deferred show and a skin change
  stop it; the clock restarts when the window goes from hidden to shown. With
  `SPI_GETCLIENTAREAANIMATION` off, images are drawn still (first frame), as for v1 skins.
