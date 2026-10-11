/// <reference types="node" />
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { expect, it, vi } from 'vitest';
import {
  applyCaretStateIndicatorPosition,
  applyFloatingToolbarAutoHideConfig,
  applyFloatingToolbarShadowConfig,
  clampAutoHideDelay,
  setupFloatingToolbar
} from './floating-toolbar';
import { setupDropdownMenu, applyDropdownValue, applyToggleState, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import partial from '../partials/floating-toolbar.html?raw';

vi.mock('./shared', () => ({
  setupDropdownMenu: vi.fn(),
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setupToggleButton: vi.fn()
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./appearance', () => ({ syncCaretStateIndicatorPreview: vi.fn() }));
vi.mock('./skin', () => ({ syncAppearancePreviews: vi.fn() }));

const styles = readFileSync(fileURLToPath(new URL('../styles/modules/floating-toolbar.css', import.meta.url)), 'utf8');

it('keeps toolbar and caret controls in separate cards with separate previews', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  const caretCard = partial.match(/<div class="section caret-state-indicator-card">([\s\S]*?)<div class="section floating-toolbar-appearance">/)?.[1] ?? '';

  expect(toolbarCard).toContain('id="ftbToggleBtn"');
  expect(toolbarCard).toContain('id="ftbPreviewHost"');
  expect(toolbarCard).not.toContain('caretStateIndicator');
  expect(caretCard).toContain('class="ftb-toggle-btn" id="caretStateIndicatorToggleBtn" role="switch" aria-label="切换输入法状态时提示" aria-checked="false"');
  expect(caretCard).toContain('可与悬浮工具栏同时开启');
  expect(caretCard).not.toContain('关闭悬浮工具栏后');
  expect(caretCard).toContain('id="caretStateIndicatorPositionBtn"');
  expect(caretCard).toContain('class="ftb-toggle-btn" id="caretStateIndicatorOnFocusToggleBtn" role="switch" aria-label="切换输入框时提示当前中英文状态" aria-checked="false"');
  expect(caretCard).toContain('id="caretStatePreviewHost"');
  expect(caretCard).not.toContain('id="ftbPreviewHost"');
  expect(caretCard).toContain('role="img" aria-label="光标状态提示预览：每个文字光标左上方分别显示中、中文标点和中文模式、全角、简体"');
  expect(caretCard).toContain('class="cand-preview" aria-hidden="true"');
});

it('titles the caret card as a group whose settings sit under it', () => {
  const caretCard = partial.match(/<div class="section caret-state-indicator-card">([\s\S]*?)<div class="section floating-toolbar-appearance">/)?.[1] ?? '';
  const heading = caretCard.match(/<div class="section-header floating-toolbar-setting-row caret-state-heading">([\s\S]*?)<\/div>\s*<\/div>\s*<\/div>/)?.[1] ?? '';
  expect(heading).toContain('<div class="section-title">光标状态提示</div>');
  // The group title has no switch of its own.
  expect(heading).not.toContain('ftb-toggle-btn');
  // Only the group title keeps the plain bold section title; the rows under
  // it share the section-title size and colour with a lighter weight.
  const groupTitles = Array.from(caretCard.matchAll(/<div class="section-title">([^<]+)<\/div>/g), ([, title]) => title);
  expect(groupTitles).toEqual(['光标状态提示']);
  const rowTitles = Array.from(
    caretCard.matchAll(/<div class="section-title caret-state-option-title">([^<]+)<\/div>/g), ([, title]) => title);
  expect(rowTitles).toEqual(['切换输入法状态时提示', '切换输入框时提示', '提示位置']);
  expect(styles).toMatch(/\.caret-state-heading\s*\{[^}]*border-bottom:/);
  expect(styles).toMatch(/\.section-title\.caret-state-option-title\s*\{[^}]*font-weight:\s*400;[^}]*-webkit-text-stroke:/);
});

it('shows the four runtime samples and preserves fixed punctuation slots', () => {
  const preview = partial.match(/<div class="candidate caret-state-preview-host"[\s\S]*?<\/div>\s*<\/div>\s*<\/div>/)?.[0] ?? '';
  expect(preview).toContain('>中</div>');
  expect(preview).toContain('aria-label="，。  中"');
  expect(preview).toContain('>全</div>');
  expect(preview).toContain('>简</div>');
  expect(preview.match(/class="caret-state-badge(?: |")/g)).toHaveLength(4);
  expect(preview.match(/class="caret-state-preview-item"[^>]*>\s*<div class="caret-state-badge[^>]*>[\s\S]*?<\/div>\s*<span class="caret-state-preview-caret"><\/span>\s*<\/div>/g)).toHaveLength(4);
  expect(preview).toContain('data-position="top-left"');
  expect(styles).toMatch(/\.caret-state-badge\s*\{[^}]*width:\s*30px;[^}]*height:\s*30px;/);
  expect(styles).toMatch(/\.caret-state-badge-punctuation\s*\{[^}]*display:\s*block;[^}]*width:\s*94px;/);
  expect(styles).toMatch(/\.caret-state-badge-punctuation\s*\{\s*--badge-width:\s*94px;/);
  expect(styles).toMatch(/\.caret-state-punctuation-slot\s*\{[^}]*left:\s*-1px;[^}]*width:\s*64px;/);
  expect(styles).toMatch(/\.caret-state-mode-slot\s*\{[^}]*right:\s*-1px;[^}]*width:\s*30px;/);
  expect(styles).toMatch(/\.caret-state-preview-host\s*\{[^}]*font-size:\s*20px;/);
  expect(styles).toMatch(/\.caret-state-preview-host\s*\{[^}]*grid-template-columns:\s*repeat\(4, 132px\);/);
  expect(styles).toMatch(/@container\s*\(max-width:\s*569px\)\s*\{\s*\.caret-state-preview-host\s*\{[^}]*grid-template-columns:\s*repeat\(2, 132px\);/);
  expect(styles).toMatch(/\.caret-state-preview-item\s*\{[^}]*width:\s*132px;[^}]*height:\s*132px;[^}]*border:/);
  expect(styles).toMatch(/\.caret-state-preview-caret\s*\{[^}]*width:\s*2px;[^}]*height:\s*24px;/);
  expect(styles).toMatch(/\.caret-state-badge\s*\{[^}]*top:\s*23px;[^}]*right:\s*29px;/);
  expect(styles).toMatch(/\[data-position="top"\] \.caret-state-badge/);
  expect(styles).toMatch(/\[data-position="top-right"\] \.caret-state-badge/);
  // Every lower position shares one vertical offset; bottom centres like top
  // and bottom-right aligns like top-right (bottom-left keeps the default).
  expect(styles).toMatch(/\[data-position\^="bottom"\] \.caret-state-badge\s*\{\s*top:\s*89px;/);
  expect(styles).toMatch(/\[data-position="bottom"\] \.caret-state-badge\s*\{\s*right:\s*auto;\s*left:\s*calc\(65px/);
  expect(styles).toMatch(/\[data-position="bottom-right"\] \.caret-state-badge\s*\{\s*right:\s*auto;\s*left:\s*23px;/);
  expect(styles).not.toMatch(/\[data-position="bottom-left"\]/);
});

it('offers every side and alignment in the position selector', () => {
  const menu = partial.split('id="caretStateIndicatorPositionMenu"')[1]?.split('</button>')[0] ?? '';
  const options = Array.from(menu.matchAll(/data-value="([^"]+)">([^<]+)<\/div>/g), ([, value, label]) => [value, label]);
  expect(options).toEqual([
    ['top-left', '左上方'], ['top', '正上方'], ['top-right', '右上方'],
    ['bottom-left', '左下方'], ['bottom', '正下方'], ['bottom-right', '右下方']
  ]);
});

it('applies each selector choice immediately to all samples and also follows config snapshots', () => {
  const label = { setAttribute: vi.fn() };
  const host = { dataset: { position: 'top-left' }, closest: () => label };
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'caretStatePreviewHost' ? host : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const call = vi.mocked(setupDropdownMenu).mock.calls.find(([button]) => button === 'caretStateIndicatorPositionBtn');
    expect(call?.[4]).toBe('general.caret_state_indicator_position');
    const change = call?.[5];
    for (const [position, direction] of [
      ['top-left', '左上方'], ['top', '正上方'], ['top-right', '右上方'],
      ['bottom-left', '左下方'], ['bottom', '正下方'], ['bottom-right', '右下方']
    ]) {
      expect(change?.(position)).toBe(position);
      expect(host.dataset.position).toBe(position);
      expect(label.setAttribute).toHaveBeenLastCalledWith('aria-label', expect.stringContaining(`每个文字光标${direction}`));
      applyCaretStateIndicatorPosition(position);
      expect(applyDropdownValue).toHaveBeenLastCalledWith('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', position);
      expect(host.dataset.position).toBe(position);
    }
  } finally {
    vi.unstubAllGlobals();
  }
});

it('persists the focus announcement switch under its own config key', () => {
  const toggle = { setAttribute: vi.fn() };
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'caretStateIndicatorOnFocusToggleBtn' ? toggle : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const call = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'caretStateIndicatorOnFocusToggleBtn');
    const onToggle = call?.[1];
    expect(onToggle).toBeTypeOf('function');
    onToggle!(true);
    expect(updateConfig).toHaveBeenLastCalledWith('general.caret_state_indicator_on_focus', true);
    expect(toggle.setAttribute).toHaveBeenLastCalledWith('aria-checked', 'true');
    onToggle!(false);
    expect(updateConfig).toHaveBeenLastCalledWith('general.caret_state_indicator_on_focus', false);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('puts the auto-hide switch and its delay stepper in the toolbar card', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  expect(toolbarCard).toContain('class="ftb-toggle-btn" id="ftbAutoHideToggleBtn" role="switch" aria-label="自动隐藏工具栏" aria-checked="false"');
  expect(toolbarCard).toContain('id="ftbAutoHideDelayDecBtn"');
  expect(toolbarCard).toContain('id="ftbAutoHideDelayInput"');
  expect(toolbarCard).toContain('id="ftbAutoHideDelayIncBtn"');
  // The delay row starts greyed out, matching the switch's default of off.
  expect(toolbarCard).toMatch(/class="[^"]*ftb-auto-hide-delay-row is-disabled" id="ftbAutoHideDelayRow"/);
  expect(toolbarCard.indexOf('id="ftbAutoHideToggleBtn"')).toBeLessThan(toolbarCard.indexOf('id="ftbPreviewHost"'));
});

it('clamps the auto-hide delay to the server range', () => {
  expect(clampAutoHideDelay(0)).toBe(1);
  expect(clampAutoHideDelay(5)).toBe(5);
  expect(clampAutoHideDelay(60)).toBe(60);
  expect(clampAutoHideDelay(61)).toBe(60);
  expect(clampAutoHideDelay(2.6)).toBe(3);
  expect(clampAutoHideDelay(Number.NaN)).toBe(5);
});

it('persists the auto-hide switch and steps the delay within range', () => {
  const listeners = new Map<string, () => void>();
  const element = (id: string) => ({
    id,
    value: '',
    disabled: false,
    tabIndex: 0,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() },
    addEventListener: (type: string, listener: () => void) => { if (type === 'click') listeners.set(id, listener); }
  });
  const elements = new Map(
    ['ftbAutoHideToggleBtn', 'ftbAutoHideDelayRow', 'ftbAutoHideDelayDecBtn', 'ftbAutoHideDelayInput', 'ftbAutoHideDelayIncBtn']
      .map((id) => [id, element(id)]));
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const onToggle = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'ftbAutoHideToggleBtn')?.[1];
    onToggle!(true);
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_hide', true);
    expect(elements.get('ftbAutoHideDelayRow')!.classList.toggle).toHaveBeenLastCalledWith('is-disabled', false);

    applyFloatingToolbarAutoHideConfig(true, 59);
    expect(elements.get('ftbAutoHideDelayInput')!.value).toBe('59');
    listeners.get('ftbAutoHideDelayIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_hide_delay', 60);
    expect(elements.get('ftbAutoHideDelayIncBtn')!.disabled).toBe(true);
    vi.mocked(updateConfig).mockClear();
    listeners.get('ftbAutoHideDelayIncBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();

    applyFloatingToolbarAutoHideConfig(true, 1);
    listeners.get('ftbAutoHideDelayDecBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();
    listeners.get('ftbAutoHideDelayIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_hide_delay', 2);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('persists the shadow switch and mirrors it on the preview', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  expect(toolbarCard).toContain('class="ftb-toggle-btn active" id="ftbShadowToggleBtn" role="switch" aria-label="工具栏阴影" aria-checked="true"');
  expect(styles).toMatch(/\.ftb-preview-host\.no-shadow \.status-bar\s*\{\s*box-shadow:\s*none !important;/);

  const toggle = { setAttribute: vi.fn() };
  const preview = { classList: { toggle: vi.fn() } };
  // Withheld during setup so mounting the preview (DOMParser) is skipped.
  let previewMounted = false;
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'ftbShadowToggleBtn' ? toggle
      : id === 'ftbPreviewHost' && previewMounted ? preview : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    previewMounted = true;
    const onToggle = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'ftbShadowToggleBtn')?.[1];
    onToggle!(false);
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_shadow', false);
    expect(preview.classList.toggle).toHaveBeenLastCalledWith('no-shadow', true);

    applyFloatingToolbarShadowConfig(true);
    expect(applyToggleState).toHaveBeenLastCalledWith('ftbShadowToggleBtn', true);
    expect(preview.classList.toggle).toHaveBeenLastCalledWith('no-shadow', false);
  } finally {
    vi.unstubAllGlobals();
  }
});