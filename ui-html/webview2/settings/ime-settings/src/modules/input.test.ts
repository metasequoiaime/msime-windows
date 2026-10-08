import { afterEach, beforeEach, expect, it, vi } from 'vitest';

// setupInput 只装配控件；用 Map 捕获每个开关的 onChanged 回调后直接调用，验证上报的配置键。
const toggles = vi.hoisted(() => new Map<string, (active: boolean) => void>());

vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setFuzzyRuleOptionsDisabled: vi.fn(),
  setSmartPunctuationOptionsDisabled: vi.fn(),
  setupDropdownMenu: vi.fn(),
  setupToggleButton: (id: string, onChanged: (active: boolean) => void) => { toggles.set(id, onChanged); }
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./appearance', () => ({ updateCandidatePreviewHelpcode: vi.fn() }));
vi.mock('./credential-test', () => ({ setupCredentialTest: vi.fn() }));

import { updateConfig } from './config-sync';
import { setSmartPunctuationOptionsDisabled } from './shared';
import { applyInputConfig, setupInput } from './input';

class StubElement extends EventTarget {
  hidden = false;
  checked = false;
  value = '';
  classes = new Set<string>();
  attributes = new Map<string, string>();
  classList = {
    toggle: (name: string, force?: boolean) => {
      const next = force ?? !this.classes.has(name);
      if (next) this.classes.add(name);
      else this.classes.delete(name);
    }
  };
  getAttribute(name: string): string | null { return this.attributes.get(name) ?? null; }
  setAttribute(name: string, value: string): void { this.attributes.set(name, value); }
}

let expand: StubElement;
let details: StubElement;
let modeRadios: StubElement[];
let chineseSettings: StubElement;
let japaneseSettings: StubElement;

beforeEach(() => {
  toggles.clear();
  vi.clearAllMocks();
  expand = new StubElement();
  details = new StubElement();
  modeRadios = ['chinese', 'japanese', 'trilingual'].map((value) => {
    const radio = new StubElement();
    radio.value = value;
    return radio;
  });
  chineseSettings = new StubElement();
  japaneseSettings = new StubElement();
  vi.stubGlobal('document', {
    querySelectorAll: (selector: string) => {
      if (selector === 'input[name="input-mode"]') return modeRadios;
      if (selector === '.chinese-scheme-settings') return [chineseSettings];
      if (selector === '.japanese-scheme-settings') return [japaneseSettings];
      return [];
    },
    querySelector: (selector: string) => {
      const value = selector.match(/^input\[name="input-mode"\]\[value="([^"]+)"\]$/)?.[1];
      return modeRadios.find((radio) => radio.value === value) ?? null;
    },
    getElementById: (id: string) => {
      if (id === 'smartPunctuationExpand') return expand;
      if (id === 'smartPunctuationDetails') return details;
      return null;
    },
    addEventListener: vi.fn()
  });
  vi.stubGlobal('window', { chrome: { webview: { postMessage: vi.fn() } } });
  setupInput();
});

afterEach(() => vi.unstubAllGlobals());

it.each(['chinese', 'japanese'])('restores trilingual selection while %s is active', (activeMode) => {
  applyInputConfig(activeMode, 'quanpin', 'simplified', 'xiaohe', 'wubi86', false, 'wildcard', false, false, 'chinese', 'app', 'romaji', true);
  expect(modeRadios[2].checked).toBe(true);
  expect(chineseSettings.hidden).toBe(false);
  expect(japaneseSettings.hidden).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  expect(window.chrome?.webview?.postMessage).not.toHaveBeenCalled();
});

it.each(['chinese', 'japanese'])('keeps the fixed %s mode when cycling is disabled', (mode) => {
  applyInputConfig(mode, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, false);
  expect(modeRadios.find((radio) => radio.value === mode)?.checked).toBe(true);
  expect(chineseSettings.hidden).toBe(mode === 'japanese');
  expect(japaneseSettings.hidden).toBe(mode === 'chinese');
});

it('enables cycling with one settings update and exposes both language schemes', () => {
  const radio = modeRadios[2];
  radio.checked = true;
  radio.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('input.trilingual_cycle', true);
  expect(chineseSettings.hidden).toBe(false);
  expect(japaneseSettings.hidden).toBe(false);
});

it.each(['chinese', 'japanese'])('leaves cycling through the existing fixed %s mode update', (mode) => {
  applyInputConfig('chinese', undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, true);
  const radio = modeRadios.find((item) => item.value === mode)!;
  radio.checked = true;
  radio.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('input.mode', mode);
  expect(chineseSettings.hidden).toBe(mode === 'japanese');
  expect(japaneseSettings.hidden).toBe(mode === 'chinese');
});

it('reports every smart punctuation sub-switch to its config path', () => {
  const options: [string, string][] = [
    ['smartPunctuationSpaceConvertToggleBtn', 'input.smart_punctuation_space_convert'],
    ['smartPunctuationRepeatToChineseToggleBtn', 'input.smart_punctuation_repeat_to_chinese'],
    ['smartPunctuationDirectDigitToggleBtn', 'input.smart_punctuation_direct_digit'],
    ['smartPunctuationDirectLetterToggleBtn', 'input.smart_punctuation_direct_letter']
  ];
  for (const [id, path] of options) {
    toggles.get(id)?.(true);
    expect(updateConfig).toHaveBeenCalledWith(path, true);
  }
});

it('disables the sub-switches together with the master switch', () => {
  toggles.get('smartPunctuationToggleBtn')?.(false);
  expect(updateConfig).toHaveBeenCalledWith('input.smart_punctuation', false);
  expect(setSmartPunctuationOptionsDisabled).toHaveBeenCalledWith(true);
  toggles.get('smartPunctuationToggleBtn')?.(true);
  expect(setSmartPunctuationOptionsDisabled).toHaveBeenLastCalledWith(false);
});

it('toggles the details container from the section header', () => {
  expand.dispatchEvent(new Event('click'));
  expect(expand.getAttribute('aria-expanded')).toBe('true');
  expect(details.classes.has('open')).toBe(true);
  expand.dispatchEvent(new Event('click'));
  expect(expand.getAttribute('aria-expanded')).toBe('false');
  expect(details.classes.has('open')).toBe(false);
});
