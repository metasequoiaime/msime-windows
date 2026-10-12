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

  let pendingRequestId = '';
  const idleLabel = button.textContent?.trim() || '测试配置';
  onHostMessage('apiCredentialTestResult', payload => {
    if (payload.requestId !== pendingRequestId) return;
    pendingRequestId = '';
    button.disabled = false;
    button.textContent = idleLabel;
    status.textContent = payload.message;
    status.dataset.kind = payload.ok ? 'success' : 'error';
  });

  button.addEventListener('click', () => {
    if (!window.chrome?.webview || pendingRequestId) return;
    pendingRequestId = `${Date.now()}-${++nextRequestId}`;
    button.disabled = true;
    button.textContent = '测试中…';
    status.textContent = '正在连接服务，请稍候…';
    status.dataset.kind = 'pending';
    window.chrome.webview.postMessage(serializeHostMessage({
      type: 'apiCredentialTest',
      data: { requestId: pendingRequestId, service: service(), config: readConfig() }
    }));
  });
}
