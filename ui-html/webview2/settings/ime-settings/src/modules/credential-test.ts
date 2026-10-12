import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

export type CredentialTestService =
  | 'translation.tencent'
  | 'translation.niutrans'
  | 'translation.custom'
  | 'translation.ai'
  | 'voice.asr'
  | 'voice.polish'
  | 'ai.assistant';

type TestConfig = Record<string, string>;
let nextRequestId = 0;

export function setupCredentialTest(
  buttonId: string,
  statusId: string,
  service: () => CredentialTestService,
  readConfig: () => TestConfig
): void {
  const button = document.getElementById(buttonId) as HTMLButtonElement | null;
  const status = document.getElementById(statusId);
  if (!button || !status) return;

  let pending: { requestId: string; service: CredentialTestService; config: TestConfig } | null = null;
  const idleLabel = button.textContent?.trim() || '测试配置';
  onHostMessage('apiCredentialTestResult', payload => {
    if (!pending || payload.requestId !== pending.requestId) return;
    const currentConfig = readConfig();
    const configMatches = pending.service === service()
      && Object.keys(pending.config).length === Object.keys(currentConfig).length
      && Object.entries(pending.config).every(([key, value]) => currentConfig[key] === value);
    pending = null;
    button.disabled = false;
    button.textContent = idleLabel;
    if (!configMatches) {
      status.textContent = '配置已变更，请重新测试';
      delete status.dataset.kind;
      return;
    }
    status.textContent = payload.message;
    status.dataset.kind = payload.ok ? 'success' : 'error';
  });

  button.addEventListener('click', () => {
    if (!window.chrome?.webview || pending) return;
    pending = {
      requestId: `${Date.now()}-${++nextRequestId}`,
      service: service(),
      config: { ...readConfig() }
    };
    button.disabled = true;
    button.textContent = '测试中…';
    status.textContent = '正在连接服务，请稍候…';
    status.dataset.kind = 'pending';
    window.chrome.webview.postMessage(serializeHostMessage({
      type: 'apiCredentialTest',
      data: pending
    }));
  });
}
