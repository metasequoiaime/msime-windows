import { afterEach, beforeEach, expect, it, vi } from 'vitest';

// setupInput 只装配控件；用 Map 捕获每个开关的 onChanged 回调后直接调用，验证上报的配置键。
const toggles = vi.hoisted(() => new Map<string, (active: boolean) => void>());

vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setFuzzyRuleOptionsDisabled: vi.fn(),
  setSmartPunctuationOptionsDisabled: vi.fn(),
  setupDropdownMenu: vi.fn(),
  setupNeuralModelControls: vi.fn(),
  setupToggleButton: (id: string, onChanged: (active: boolean) => void) => { toggles.set(id, onChanged); }
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./appearance', () => ({ updateCandidatePreviewHelpcode: vi.fn() }));
vi.mock('./credential-test', () => ({ setupCredentialTest: vi.fn() }));

import { updateConfig } from './config-sync';
import { setSmartPunctuationOptionsDisabled } from './shared';
import { applyTranslationConfig, setupInput } from './input';
import { isUsableAiAssistantState, setAiAssistantState } from './ai-assistant-state';

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
let aiSummary: { value: string };

beforeEach(() => {
  toggles.clear();
  vi.clearAllMocks();
  expand = new StubElement();
  details = new StubElement();
  aiSummary = { value: '' };
  vi.stubGlobal('document', {
    querySelectorAll: () => [],
    querySelector: () => null,
    getElementById: (id: string) => {
      if (id === 'smartPunctuationExpand') return expand;
      if (id === 'smartPunctuationDetails') return details;
      if (id === 'aiTranslationSummary') return aiSummary;
      return null;
    },
    addEventListener: vi.fn()
  });
  vi.stubGlobal('window', { chrome: { webview: { postMessage: vi.fn() } } });
  setupInput();
});

afterEach(() => vi.unstubAllGlobals());

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

it('shows the AI assistant provider when candidate translation reuses it', () => {
  setAiAssistantState({
    provider: 'deepseek', endpoint: 'https://api.deepseek.com/chat/completions', model: 'deepseek-v4-flash', token: 'sk-test'
  });
  applyTranslationConfig({ provider: 'ai' });
  expect(aiSummary.value).toBe('deepseek · deepseek-v4-flash');
});

it('follows AI assistant edits made after the snapshot', () => {
  setupInput();
  setAiAssistantState({ provider: 'deepseek', endpoint: 'https://api.deepseek.com/chat/completions', model: 'm', token: '' });
  applyTranslationConfig({ provider: 'ai' });
  expect(aiSummary.value).toBe('');
  setAiAssistantState({ provider: 'deepseek', endpoint: 'https://api.deepseek.com/chat/completions', model: 'm', token: 'sk-real' });
  expect(aiSummary.value).toBe('deepseek · m');
});

it('treats template placeholder tokens as not configured', () => {
  for (const token of ['FAKESECRET_e1f2g3h4', '<YOUR_AI_TOKEN_DEEPSEEK>']) {
    expect(isUsableAiAssistantState({ provider: 'deepseek', endpoint: 'https://x/chat', model: 'm', token })).toBe(false);
  }
  expect(isUsableAiAssistantState({ provider: 'custom', endpoint: 'ftp://x', model: 'm', token: 'sk' })).toBe(false);
});
