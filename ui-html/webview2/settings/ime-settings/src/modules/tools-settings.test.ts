import { afterEach, beforeEach, expect, it, vi } from 'vitest';

// setupToolsSettings 只装配控件；用 Map 捕获每个开关的 onChanged 回调后直接调用，验证上报的配置键。
const toggles = vi.hoisted(() => new Map<string, (active: boolean) => void>());

vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setLanguageBarOptionsDisabled: vi.fn(),
  setMixedCandidateOptionsDisabled: vi.fn(),
  setupDropdownMenu: vi.fn(),
  setupToggleButton: (id: string, onChanged: (active: boolean) => void) => { toggles.set(id, onChanged); }
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('../utils/dictionary-pagination', () => ({ createDictionaryPager: vi.fn(), DICTIONARY_PAGE_SIZE: 50 }));
vi.mock('../utils/host-messages', () => ({ onHostMessage: vi.fn() }));
vi.mock('../utils/confirm-dialog', () => ({ confirmDialog: vi.fn() }));
vi.mock('../utils/overlay-host', () => ({ hoistOverlay: vi.fn() }));

import { updateConfig } from './config-sync';
import { setLanguageBarOptionsDisabled, setMixedCandidateOptionsDisabled, setupDropdownMenu } from './shared';
import { setupToolsSettings } from './tools-settings';

class StubElement extends EventTarget {
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
let zhEnOptions: StubElement;
let langBarExpand: StubElement;
let langBarDetails: StubElement;

beforeEach(() => {
  toggles.clear();
  vi.clearAllMocks();
  expand = new StubElement();
  details = new StubElement();
  zhEnOptions = new StubElement();
  langBarExpand = new StubElement();
  langBarDetails = new StubElement();
  vi.stubGlobal('document', {
    querySelectorAll: () => [],
    querySelector: () => null,
    getElementById: (id: string) => {
      if (id === 'mixedCandidatesExpand') return expand;
      if (id === 'mixedCandidatesDetails') return details;
      if (id === 'zhEnMixedInputOptions') return zhEnOptions;
      if (id === 'languageBarExpand') return langBarExpand;
      if (id === 'languageBarDetails') return langBarDetails;
      return null;
    },
    addEventListener: vi.fn()
  });
  vi.stubGlobal('window', { chrome: { webview: { postMessage: vi.fn() } }, addEventListener: vi.fn() });
  setupToolsSettings();
});

afterEach(() => vi.unstubAllGlobals());

it('reports every mixed candidate switch to its config path', () => {
  const options: [string, string][] = [
    ['mixedCandidatesToggleBtn', 'utility.mixed_candidates'],
    ['zhEnToggleBtn', 'general.cn_en_mixed_input'],
    ['emojiMixedInputToggleBtn', 'general.emoji_mixed_input'],
    ['kaomojiMixedInputToggleBtn', 'general.kaomoji_mixed_input'],
    ['quickPhraseCandidatesToggleBtn', 'utility.quick_phrase_candidates'],
    ['quickPhraseFrequencyToggleBtn', 'utility.quick_phrase_frequency'],
    ['dateTimeCandidatesToggleBtn', 'utility.date_time_candidates'],
    ['dateTimeMenuToggleBtn', 'utility.date_time_menu']
  ];
  for (const [id, path] of options) {
    toggles.get(id)?.(true);
    expect(updateConfig).toHaveBeenCalledWith(path, true);
  }
  expect(setupDropdownMenu).toHaveBeenCalledWith(
    'zhEnTriggerLengthBtn',
    'zhEnTriggerLengthMenu',
    '',
    true,
    'general.cn_en_mixed_input_min_chars',
    Number
  );
});

it('disables the sub-switches together with the master switch', () => {
  toggles.get('mixedCandidatesToggleBtn')?.(false);
  expect(setMixedCandidateOptionsDisabled).toHaveBeenCalledWith(true);
  toggles.get('mixedCandidatesToggleBtn')?.(true);
  expect(setMixedCandidateOptionsDisabled).toHaveBeenLastCalledWith(false);
});

it('greys out the English trigger length with the English switch', () => {
  toggles.get('zhEnToggleBtn')?.(false);
  expect(zhEnOptions.classes.has('is-disabled')).toBe(true);
  toggles.get('zhEnToggleBtn')?.(true);
  expect(zhEnOptions.classes.has('is-disabled')).toBe(false);
});

it('toggles the details container from the section header', () => {
  expand.dispatchEvent(new Event('click'));
  expect(expand.getAttribute('aria-expanded')).toBe('true');
  expect(details.classes.has('open')).toBe(true);
  expand.dispatchEvent(new Event('click'));
  expect(expand.getAttribute('aria-expanded')).toBe('false');
  expect(details.classes.has('open')).toBe(false);
});

it('reports every language bar switch to its config path', () => {
  const options: [string, string][] = [
    ['languageBarToggleBtn', 'language_bar.enabled'],
    ['languageBarImeModeToggleBtn', 'language_bar.show_ime_mode'],
    ['languageBarDoubleSingleByteToggleBtn', 'language_bar.show_double_single_byte'],
    ['languageBarPunctuationToggleBtn', 'language_bar.show_punctuation']
  ];
  for (const [id, path] of options) {
    toggles.get(id)?.(true);
    expect(updateConfig).toHaveBeenCalledWith(path, true);
  }
});

it('greys out the language bar sub-switches together with the master switch', () => {
  toggles.get('languageBarToggleBtn')?.(false);
  expect(setLanguageBarOptionsDisabled).toHaveBeenCalledWith(true);
  toggles.get('languageBarToggleBtn')?.(true);
  expect(setLanguageBarOptionsDisabled).toHaveBeenLastCalledWith(false);
});

it('toggles the language bar details container from the section header', () => {
  langBarExpand.dispatchEvent(new Event('click'));
  expect(langBarExpand.getAttribute('aria-expanded')).toBe('true');
  expect(langBarDetails.classes.has('open')).toBe(true);
  langBarExpand.dispatchEvent(new Event('click'));
  expect(langBarExpand.getAttribute('aria-expanded')).toBe('false');
  expect(langBarDetails.classes.has('open')).toBe(false);
});
