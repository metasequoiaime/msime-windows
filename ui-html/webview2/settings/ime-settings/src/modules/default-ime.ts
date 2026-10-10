import { serializeHostMessage, type ServerMessage, type SettingsMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

// 帮助页的「默认输入法」。默认与否的权威在系统（当前用户的输入法设置），页面不存副本：
// 每次帮助页显示、窗口重新获得焦点（用户可能刚从系统设置回来）都向宿主重新查询。

type DefaultImeAction = Extract<SettingsMessage, { type: 'defaultImeRequest' }>['data']['action'];
type DefaultImeResponse = Extract<ServerMessage, { type: 'defaultImeResponse' }>;

let requestCounter = 0;
const pendingRequests = new Map<string, DefaultImeAction>();

function setStatus(message: string, kind: 'idle' | 'success' | 'error'): void {
  const status = document.getElementById('defaultImeStatus');
  if (!status) return;
  status.textContent = message;
  status.dataset.kind = kind;
}

function setButtonBusy(busy: boolean): void {
  const button = document.getElementById('defaultImeSetButton');
  if (!(button instanceof HTMLButtonElement)) return;
  button.disabled = busy;
  button.textContent = busy ? '正在设置…' : '设为默认';
}

function post(action: DefaultImeAction): void {
  const webview = window.chrome?.webview;
  if (!webview) return;
  const requestId = `default-ime-${++requestCounter}`;
  pendingRequests.set(requestId, action);
  webview.postMessage(serializeHostMessage({ type: 'defaultImeRequest', data: { requestId, action } }));
}

export function describeDefaultImeStatus(response: Pick<DefaultImeResponse, 'enabled' | 'isDefault'>): string {
  if (response.isDefault) return '水杉输入法已是默认输入法';
  if (response.enabled === false) return '水杉输入法不在当前用户的键盘列表中，设为默认时会自动添加';
  return '当前默认输入法不是水杉输入法';
}

function handleResponse(response: DefaultImeResponse, action: DefaultImeAction): void {
  const button = document.getElementById('defaultImeSetButton');
  if (action === 'setDefault') setButtonBusy(false);
  if (button instanceof HTMLButtonElement) button.disabled = response.isDefault === true;

  if (action === 'setDefault' && !response.ok) {
    setStatus(response.message || '设置失败，请点击「系统设置」手动选择', 'error');
    return;
  }
  if (action === 'setDefault') {
    setStatus(response.message || describeDefaultImeStatus(response), 'success');
    return;
  }
  setStatus(describeDefaultImeStatus(response), response.isDefault ? 'success' : 'idle');
}

function isHelpPageVisible(): boolean {
  const root = document.getElementById('help-settings');
  return !!root && root.getClientRects().length > 0 && document.visibilityState === 'visible';
}

function refreshStatus(): void {
  if (!isHelpPageVisible()) return;
  // 设置请求在途时不插查询：它的回复本身就带最新状态。
  for (const action of pendingRequests.values()) {
    if (action === 'setDefault') return;
  }
  post('status');
}

export function setupDefaultIme(): void {
  onHostMessage('defaultImeResponse', (payload) => {
    const action = pendingRequests.get(payload.requestId);
    if (!action) return;
    pendingRequests.delete(payload.requestId);
    handleResponse(payload, action);
  });

  document.getElementById('defaultImeSetButton')?.addEventListener('click', () => {
    setButtonBusy(true);
    setStatus('正在设置…', 'idle');
    post('setDefault');
  });
  document.getElementById('defaultImeSystemButton')?.addEventListener('click', () => {
    post('openSystemSettings');
  });

  const root = document.getElementById('help-settings');
  if (root && typeof IntersectionObserver !== 'undefined') {
    new IntersectionObserver((entries) => {
      if (entries.some((entry) => entry.isIntersecting)) refreshStatus();
    }).observe(root);
  }
  window.addEventListener('focus', refreshStatus);
  document.addEventListener('visibilitychange', refreshStatus);

  post('status');
}
