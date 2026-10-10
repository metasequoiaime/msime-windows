import { applyDropdownValue, applyToggleState, setupDropdownMenu, setupToggleButton } from './shared';
import { syncCaretStateIndicatorPreview } from './appearance';
import { updateConfig } from './config-sync';
import { syncAppearancePreviews } from './skin';
import { applyToolbarIconGlyphFallbacks } from './toolbar-icon-glyphs';
import ftbHTML from '../../../../ftb/default.html?raw';

type FloatingToolbarItem = 'fullwidth' | 'punctuation' | 'character_set' | 'emoji' | 'screen_keyboard' | 'settings';
type FloatingToolbarItemsConfig = Partial<Record<FloatingToolbarItem, boolean>>;

const toolbarItems: FloatingToolbarItem[] = [
  'fullwidth',
  'punctuation',
  'character_set',
  'emoji',
  'screen_keyboard',
  'settings'
];

const toolbarItemState: Record<FloatingToolbarItem, boolean> = {
  fullwidth: true,
  punctuation: true,
  character_set: true,
  emoji: true,
  screen_keyboard: false,
  settings: true
};

let toolbarScale = 1;
let toolbarFontSize = 24;

// Mirrors kFloatingToolbarAutoHideDelay{Min,Max,Default} in the server config.
export const AUTO_HIDE_DELAY_MIN = 1;
export const AUTO_HIDE_DELAY_MAX = 60;
const AUTO_HIDE_DELAY_DEFAULT = 5;
let autoHideDelay = AUTO_HIDE_DELAY_DEFAULT;

export function setupFloatingToolbar(): void {
  mountFloatingToolbarPreview();
  syncCaretStateIndicatorPreview();
  syncAppearancePreviews();

  setupToggleButton('ftbToggleBtn', (active) => {
    updateConfig('general.floating_toolbar', active);
    document.getElementById('ftbToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupToggleButton('ftbAutoHideToggleBtn', (active) => {
    updateConfig('general.floating_toolbar_auto_hide', active);
    document.getElementById('ftbAutoHideToggleBtn')?.setAttribute('aria-checked', String(active));
    setAutoHideDelayDisabled(!active);
  });
  setupAutoHideDelayStepper();

  setupToggleButton('ftbShadowToggleBtn', (active) => {
    updateConfig('general.floating_toolbar_shadow', active);
    document.getElementById('ftbShadowToggleBtn')?.setAttribute('aria-checked', String(active));
    setPreviewShadow(active);
  });

  setupToggleButton('caretStateIndicatorToggleBtn', (active) => {
    updateConfig('general.caret_state_indicator', active);
    document.getElementById('caretStateIndicatorToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupToggleButton('caretStateIndicatorOnFocusToggleBtn', (active) => {
    updateConfig('general.caret_state_indicator_on_focus', active);
    document.getElementById('caretStateIndicatorOnFocusToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupDropdownMenu('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', '', true,
    'general.caret_state_indicator_position', (value) => {
      updateCaretPreviewPosition(value);
      return value;
    });

  setupDropdownMenu('ftbScaleBtn', 'ftbScaleMenu', '', true, 'general.floating_toolbar_scale', (value) => {
    const parsed = Number(value);
    toolbarScale = Number.isFinite(parsed) && parsed > 0 ? parsed : 1;
    applyPreviewAppearance();
    return toolbarScale;
  });

  setupDropdownMenu('ftbFontSizeBtn', 'ftbFontSizeMenu', '', true, 'general.floating_toolbar_font_size', (value) => {
    const parsed = Number(value);
    toolbarFontSize = Number.isFinite(parsed) ? parsed : 24;
    applyPreviewAppearance();
    return toolbarFontSize;
  });

  document.querySelectorAll<HTMLInputElement>('.floating-toolbar-component-list input[data-toolbar-item]')
    .forEach((checkbox) => {
      checkbox.addEventListener('change', () => {
        const item = checkbox.dataset.toolbarItem as FloatingToolbarItem;
        toolbarItemState[item] = checkbox.checked;
        updatePreviewItems();
        updateConfig(`general.floating_toolbar_${item}`, checkbox.checked);
      });
    });
}

export function applyFloatingToolbarItemsConfig(config: FloatingToolbarItemsConfig | undefined): void {
  if (!config) return;
  toolbarItems.forEach((item) => {
    if (typeof config[item] !== 'boolean') return;
    toolbarItemState[item] = config[item]!;
    const checkbox = document.querySelector<HTMLInputElement>(
      `.floating-toolbar-component-list input[data-toolbar-item="${item}"]`
    );
    if (checkbox) checkbox.checked = toolbarItemState[item];
  });
  updatePreviewItems();
}

export function applyCaretStateIndicatorPosition(position?: string): void {
  const value = position || 'top-left';
  applyDropdownValue('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', value);
  updateCaretPreviewPosition(value);
}

function updateCaretPreviewPosition(position: string): void {
  const host = document.getElementById('caretStatePreviewHost');
  if (!host) return;
  host.dataset.position = position;
  const direction = ({
    'top-left': '左上方', top: '正上方', 'top-right': '右上方',
    'bottom-left': '左下方', bottom: '正下方', 'bottom-right': '右下方'
  } as Record<string, string>)[position];
  host.closest('.caret-state-preview')?.setAttribute('aria-label',
    `光标状态提示预览：每个文字光标${direction || '左上方'}分别显示中、中文标点和中文模式、全角、简体`);
}

export function applyFloatingToolbarAppearanceConfig(scale?: number, fontSize?: number): void {
  if (typeof scale === 'number' && scale > 0) {
    toolbarScale = scale;
    applyDropdownValue('ftbScaleBtn', 'ftbScaleMenu', normalizeScaleKey(scale));
  }
  if (typeof fontSize === 'number' && fontSize > 0) {
    toolbarFontSize = fontSize;
    applyDropdownValue('ftbFontSizeBtn', 'ftbFontSizeMenu', String(fontSize));
  }
  applyPreviewAppearance();
}

export function clampAutoHideDelay(value: number): number {
  if (!Number.isFinite(value)) return AUTO_HIDE_DELAY_DEFAULT;
  return Math.min(AUTO_HIDE_DELAY_MAX, Math.max(AUTO_HIDE_DELAY_MIN, Math.round(value)));
}

export function applyFloatingToolbarAutoHideConfig(enabled?: boolean, delay?: number): void {
  if (typeof enabled === 'boolean') {
    applyToggleState('ftbAutoHideToggleBtn', enabled);
    setAutoHideDelayDisabled(!enabled);
  }
  if (typeof delay === 'number') {
    autoHideDelay = clampAutoHideDelay(delay);
    renderAutoHideDelay();
  }
}

export function applyFloatingToolbarShadowConfig(enabled?: boolean): void {
  if (typeof enabled !== 'boolean') return;
  applyToggleState('ftbShadowToggleBtn', enabled);
  setPreviewShadow(enabled);
}

function setPreviewShadow(enabled: boolean): void {
  document.getElementById('ftbPreviewHost')?.classList.toggle('no-shadow', !enabled);
}

function setAutoHideDelayDisabled(disabled: boolean): void {
  document.getElementById('ftbAutoHideDelayRow')?.classList.toggle('is-disabled', disabled);
  ['ftbAutoHideDelayDecBtn', 'ftbAutoHideDelayInput', 'ftbAutoHideDelayIncBtn'].forEach((id) => {
    const control = document.getElementById(id);
    if (control) control.tabIndex = disabled ? -1 : 0;
  });
  renderAutoHideDelay();
}

function renderAutoHideDelay(): void {
  const input = document.getElementById('ftbAutoHideDelayInput') as HTMLInputElement | null;
  if (input) input.value = String(autoHideDelay);
  const decrement = document.getElementById('ftbAutoHideDelayDecBtn') as HTMLButtonElement | null;
  const increment = document.getElementById('ftbAutoHideDelayIncBtn') as HTMLButtonElement | null;
  if (decrement) decrement.disabled = autoHideDelay <= AUTO_HIDE_DELAY_MIN;
  if (increment) increment.disabled = autoHideDelay >= AUTO_HIDE_DELAY_MAX;
}

function commitAutoHideDelay(value: number): void {
  const next = clampAutoHideDelay(value);
  const changed = next !== autoHideDelay;
  autoHideDelay = next;
  renderAutoHideDelay();
  if (changed) updateConfig('general.floating_toolbar_auto_hide_delay', next);
}

function setupAutoHideDelayStepper(): void {
  document.getElementById('ftbAutoHideDelayDecBtn')?.addEventListener('click', () => {
    commitAutoHideDelay(autoHideDelay - 1);
  });
  document.getElementById('ftbAutoHideDelayIncBtn')?.addEventListener('click', () => {
    commitAutoHideDelay(autoHideDelay + 1);
  });
  const input = document.getElementById('ftbAutoHideDelayInput') as HTMLInputElement | null;
  if (!input) return;
  // Typed values commit on Enter or blur; anything unparsable snaps back.
  const commitTyped = () => {
    const text = input.value.trim();
    commitAutoHideDelay(/^\d+$/.test(text) ? Number(text) : autoHideDelay);
  };
  input.addEventListener('change', commitTyped);
  input.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      commitTyped();
    } else if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      event.preventDefault();
      commitAutoHideDelay(autoHideDelay + (event.key === 'ArrowUp' ? 1 : -1));
    }
  });
}

function normalizeScaleKey(scale: number): string {
  if (Math.abs(scale - 0.75) < 0.001) return '0.75';
  if (Math.abs(scale - 1.25) < 0.001) return '1.25';
  if (Math.abs(scale - 1.5) < 0.001) return '1.5';
  return '1';
}

function mountFloatingToolbarPreview(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;

  const source = new DOMParser().parseFromString(ftbHTML, 'text/html');
  const statusBar = source.querySelector<HTMLElement>('.status-bar');
  if (!statusBar) return;

  statusBar.querySelectorAll('#en, #fullwidth, #puncEn').forEach((element) => element.remove());
  statusBar.querySelectorAll<HTMLElement>('[id]').forEach((element) => element.removeAttribute('id'));
  applyToolbarIconGlyphFallbacks(statusBar);
  host.replaceChildren(statusBar);
  updatePreviewItems();
  applyPreviewAppearance();
}

function updatePreviewItems(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;
  toolbarItems.forEach((item) => {
    const element = host.querySelector<HTMLElement>(`[data-toolbar-item="${item}"]`);
    if (element) element.style.display = toolbarItemState[item] ? 'flex' : 'none';
  });
}

function applyPreviewAppearance(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;
  host.style.setProperty('--ftb-scale', String(toolbarScale));
  host.style.setProperty('--ftb-icon-size', `${toolbarFontSize}px`);
}
