import { afterEach, beforeEach, expect, it, vi } from 'vitest';

vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setupDropdownMenu: vi.fn(),
  setupToggleButton: vi.fn()
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./credential-test', () => ({ setupCredentialTest: vi.fn() }));
vi.mock('./model-fetch', () => ({ setupModelFetch: vi.fn(() => vi.fn()) }));

import { applyAiConfig, setupAiSettings } from './ai-settings';
import { updateConfig } from './config-sync';

class StubElement {
  value = '';
  placeholder = '';
  listeners = new Map<string, (event: unknown) => void>();
  addEventListener(type: string, listener: (event: unknown) => void): void {
    this.listeners.set(type, listener);
  }
  select(value: string): void {
    this.listeners.get('click')?.({ target: { closest: () => ({ dataset: { value } }) } });
  }
}

let elements: Map<string, StubElement>;

beforeEach(() => {
  vi.clearAllMocks();
  elements = new Map(['aiToken', 'aiEndpoint', 'aiModel', 'aiProviderMenu'].map(id => [id, new StubElement()]));
  vi.stubGlobal('document', { getElementById: (id: string) => elements.get(id) ?? null });
  setupAiSettings();
});

afterEach(() => vi.unstubAllGlobals());

it('switches custom endpoints, models and tokens together and restores them on return', () => {
  applyAiConfig({
    provider: 'deepseek', token: 'test-deepseek',
    endpoint: 'https://deepseek.example.test/chat/completions', model: 'custom-deepseek',
    tokens: { deepseek: 'test-deepseek', openai: 'test-openai' },
    endpoints: { deepseek: 'https://deepseek.example.test/chat/completions', openai: 'https://openai.example.test/v1/chat/completions' },
    models: { deepseek: 'custom-deepseek', openai: 'custom-openai' }
  });

  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiToken')!.value).toBe('test-openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://openai.example.test/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('custom-openai');
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.provider', 'openai');

  elements.get('aiProviderMenu')!.select('deepseek');
  expect(elements.get('aiToken')!.value).toBe('test-deepseek');
  expect(elements.get('aiEndpoint')!.value).toBe('https://deepseek.example.test/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('custom-deepseek');
});

it('keeps legacy custom values with their provider and uses defaults for an unconfigured provider', () => {
  applyAiConfig({
    provider: 'deepseek', endpoint: 'https://legacy.example.test/chat/completions', model: 'legacy-model'
  });
  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://api.openai.com/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('gpt-4o-mini');
  elements.get('aiProviderMenu')!.select('deepseek');
  expect(elements.get('aiEndpoint')!.value).toBe('https://legacy.example.test/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('legacy-model');
});

it('treats empty endpoints and models as provider defaults', () => {
  applyAiConfig({
    provider: 'deepseek', endpoint: '', model: '',
    endpoints: { deepseek: '', openai: '' }, models: { deepseek: '', openai: '' }
  });
  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://api.openai.com/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('gpt-4o-mini');

  const endpoint = elements.get('aiEndpoint')!;
  endpoint.value = '';
  endpoint.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.endpoint', '');
  expect(endpoint.value).toBe('https://api.openai.com/v1/chat/completions');

  endpoint.value = '';
  elements.get('aiProviderMenu')!.select('deepseek');
  elements.get('aiProviderMenu')!.select('openai');
  expect(endpoint.value).toBe('https://api.openai.com/v1/chat/completions');
});

it('reloads provider-specific values from a new config snapshot', () => {
  applyAiConfig({
    provider: 'openai', endpoint: 'https://openai.example.test/v1/chat/completions', model: 'custom-openai',
    endpoints: { openai: 'https://openai.example.test/v1/chat/completions', groq: 'https://groq.example.test/v1/chat/completions' },
    models: { openai: 'custom-openai', groq: 'custom-groq' }
  });
  elements.get('aiProviderMenu')!.select('groq');
  expect(elements.get('aiEndpoint')!.value).toBe('https://groq.example.test/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('custom-groq');
});

it('keeps the Custom base url, api key and model independent from built-in providers', () => {
  applyAiConfig({
    provider: 'deepseek',
    token: 'deepseek-key',
    endpoint: 'https://api.deepseek.com/chat/completions',
    model: 'deepseek-v4-flash'
  });

  elements.get('aiProviderMenu')!.select('custom');
  // Custom 槽位没有内置默认值，切过去应为空等待填写。
  expect(elements.get('aiEndpoint')!.value).toBe('');
  expect(elements.get('aiModel')!.value).toBe('');
  expect(elements.get('aiToken')!.value).toBe('');

  elements.get('aiEndpoint')!.value = 'https://my-llm.example.test/v1/chat/completions';
  elements.get('aiToken')!.value = 'custom-key';
  elements.get('aiModel')!.value = 'my-model';

  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://api.openai.com/v1/chat/completions');
  expect(elements.get('aiToken')!.value).toBe('');

  elements.get('aiProviderMenu')!.select('custom');
  expect(elements.get('aiEndpoint')!.value).toBe('https://my-llm.example.test/v1/chat/completions');
  expect(elements.get('aiToken')!.value).toBe('custom-key');
  expect(elements.get('aiModel')!.value).toBe('my-model');
});
