import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import type { CredentialTestService } from './credential-test';

class TestElement extends EventTarget {
  textContent = '';
  disabled = false;
  dataset: Record<string, string> = {};
}

let button: TestElement;
let status: TestElement;
let postMessage: ReturnType<typeof vi.fn>;
let hostListener: ((event: Event & { data?: unknown }) => void) | undefined;

beforeEach(() => {
  vi.resetModules();
  hostListener = undefined;
  button = new TestElement();
  button.textContent = '测试配置';
  status = new TestElement();
  postMessage = vi.fn();
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'testButton' ? button : status
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

it('posts current field values and renders the matching result', async () => {
  const { setupCredentialTest } = await import('./credential-test');
  setupCredentialTest(
    'testButton',
    'testStatus',
    () => 'ai.assistant',
    () => ({ token: 'current-token', model: 'current-model' })
  );
  button.dispatchEvent(new Event('click'));
  expect(button.disabled).toBe(true);
  expect(button.textContent).toBe('测试中…');
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  expect(request).toMatchObject({
    type: 'apiCredentialTest',
    data: { service: 'ai.assistant', config: { token: 'current-token', model: 'current-model' } }
  });

  hostListener?.({ data: {
    type: 'apiCredentialTestResult', requestId: request.data.requestId,
    ok: true, message: '连接成功'
  } } as Event & { data?: unknown });
  expect(button.disabled).toBe(false);
  expect(button.textContent).toBe('测试配置');
  expect(status.textContent).toBe('连接成功');
  expect(status.dataset.kind).toBe('success');
});

it.each(['token', 'endpoint', 'model', 'provider'])('does not apply a result after %s changes', async (field) => {
  const { setupCredentialTest } = await import('./credential-test');
  const config: Record<string, string> = {
    token: 'test-token', endpoint: 'https://example.invalid', model: 'test-model', provider: 'custom'
  };
  setupCredentialTest('testButton', 'testStatus', () => 'ai.assistant', () => config);
  button.dispatchEvent(new Event('click'));
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  config[field] = 'changed-value';
  hostListener?.({ data: {
    type: 'apiCredentialTestResult', requestId: request.data.requestId,
    ok: true, message: '连接成功'
  } } as Event & { data?: unknown });

  expect(status.textContent).toBe('配置已变更，请重新测试');
  expect(status.dataset.kind).toBeUndefined();
  expect(button.disabled).toBe(false);
  expect(button.textContent).toBe('测试配置');

  button.dispatchEvent(new Event('click'));
  const retry = JSON.parse(postMessage.mock.calls[1]![0]);
  expect(retry.data.config[field]).toBe('changed-value');
  hostListener?.({ data: {
    type: 'apiCredentialTestResult', requestId: retry.data.requestId,
    ok: true, message: '新配置连接成功'
  } } as Event & { data?: unknown });
  expect(status.textContent).toBe('新配置连接成功');
  expect(status.dataset.kind).toBe('success');
});

it('does not apply a result after the service changes', async () => {
  const { setupCredentialTest } = await import('./credential-test');
  let service: CredentialTestService = 'translation.ai';
  setupCredentialTest('testButton', 'testStatus', () => service, () => ({ token: 'test-token' }));
  button.dispatchEvent(new Event('click'));
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  service = 'translation.custom';
  hostListener?.({ data: {
    type: 'apiCredentialTestResult', requestId: request.data.requestId,
    ok: false, message: '连接失败'
  } } as Event & { data?: unknown });
  expect(status.textContent).toBe('配置已变更，请重新测试');
  expect(status.dataset.kind).toBeUndefined();
  expect(button.disabled).toBe(false);
});

it('accepts unchanged values regardless of config key order', async () => {
  const { setupCredentialTest } = await import('./credential-test');
  let config = { token: 'test-token', model: 'test-model' };
  setupCredentialTest('testButton', 'testStatus', () => 'ai.assistant', () => config);
  button.dispatchEvent(new Event('click'));
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  config = { model: 'test-model', token: 'test-token' };
  hostListener?.({ data: {
    type: 'apiCredentialTestResult', requestId: request.data.requestId,
    ok: true, message: '连接成功'
  } } as Event & { data?: unknown });
  expect(status.textContent).toBe('连接成功');
  expect(status.dataset.kind).toBe('success');
});
