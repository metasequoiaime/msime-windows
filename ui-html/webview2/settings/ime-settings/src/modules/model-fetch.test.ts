import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { setupModelFetch } from './model-fetch';

class FakeElement extends EventTarget {
  textContent = '';
  disabled = false;
  dataset: Record<string, string> = {};
  children: FakeElement[] = [];
  className = '';
  title = '';
  classList = {
    values: new Set<string>(),
    add: (name: string) => { this.classList.values.add(name); },
    remove: (name: string) => { this.classList.values.delete(name); },
    contains: (name: string) => this.classList.values.has(name)
  };
  replaceChildren(...nodes: FakeElement[]): void {
    this.children = nodes;
  }
  get childElementCount(): number {
    return this.children.length;
  }
}

let button: FakeElement;
let status: FakeElement;
let menu: FakeElement;
let postMessage: ReturnType<typeof vi.fn>;
let hostListener: ((event: Event & { data?: unknown }) => void) | undefined;

beforeEach(() => {
  button = new FakeElement();
  button.textContent = '获取模型';
  status = new FakeElement();
  menu = new FakeElement();
  postMessage = vi.fn();
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      if (id === 'modelButton') return button;
      if (id === 'modelStatus') return status;
      if (id === 'modelMenu') return menu;
      return null;
    },
    createElement: () => new FakeElement(),
    querySelectorAll: () => []
  });
  vi.stubGlobal('window', {
    chrome: { webview: {
      postMessage,
      addEventListener: (_type: string, listener: (event: Event & { data?: unknown }) => void) => {
        hostListener = listener;
      }
    } }
  });
  vi.stubGlobal('MsimeProtocol', {
    version: 1,
    validate: () => true,
    serializeClientMessage: (message: unknown) => JSON.stringify(message)
  });
});

afterEach(() => vi.unstubAllGlobals());

function setup(): () => void {
  return setupModelFetch({
    buttonId: 'modelButton',
    statusId: 'modelStatus',
    menuId: 'modelMenu',
    service: () => 'ai.assistant',
    readConfig: () => ({ provider: 'groq', token: 'token', endpoint: 'https://api.groq.com/openai/v1/chat/completions' })
  });
}

it('posts the current config and renders the returned models into the menu', () => {
  setup();
  button.dispatchEvent(new Event('click'));
  expect(button.disabled).toBe(true);
  expect(button.textContent).toBe('获取中…');
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  expect(request).toMatchObject({
    type: 'apiModelList',
    data: { service: 'ai.assistant', config: { provider: 'groq' } }
  });

  hostListener?.({ data: {
    type: 'apiModelListResult', requestId: request.data.requestId,
    ok: true, message: '获取成功', models: ['llama-3.3-70b-versatile', 'qwen/qwen3.8-27b']
  } } as Event & { data?: unknown });

  expect(button.disabled).toBe(false);
  expect(button.textContent).toBe('获取模型');
  expect(menu.children.map(item => item.dataset.value)).toEqual(['llama-3.3-70b-versatile', 'qwen/qwen3.8-27b']);
  expect(status.textContent).toBe('获取到 2 个模型');
  expect(status.dataset.kind).toBe('success');
  expect(menu.classList.contains('open')).toBe(true);
});

it('keeps the menu untouched and reports the error when the query fails', () => {
  setup();
  button.dispatchEvent(new Event('click'));
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  hostListener?.({ data: {
    type: 'apiModelListResult', requestId: request.data.requestId,
    ok: false, message: '获取模型列表失败：HTTP 401', models: []
  } } as Event & { data?: unknown });

  expect(menu.children).toEqual([]);
  expect(status.textContent).toBe('获取模型列表失败：HTTP 401');
  expect(status.dataset.kind).toBe('error');
});

it('ignores results that do not match the pending request', () => {
  setup();
  button.dispatchEvent(new Event('click'));
  hostListener?.({ data: {
    type: 'apiModelListResult', requestId: 'stale', ok: true, message: '获取成功', models: ['x']
  } } as Event & { data?: unknown });

  expect(button.disabled).toBe(true);
  expect(menu.children).toEqual([]);
});

it('drops an in-flight result after reset, such as on a provider switch', () => {
  const reset = setup();
  button.dispatchEvent(new Event('click'));
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  reset();
  expect(button.disabled).toBe(false);
  expect(button.textContent).toBe('获取模型');

  hostListener?.({ data: {
    type: 'apiModelListResult', requestId: request.data.requestId,
    ok: true, message: '获取成功', models: ['llama-3.3-70b-versatile']
  } } as Event & { data?: unknown });

  expect(menu.children).toEqual([]);
  expect(menu.classList.contains('open')).toBe(false);
  expect(status.textContent).toBe('');
});
