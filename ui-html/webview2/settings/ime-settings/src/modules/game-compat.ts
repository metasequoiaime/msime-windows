import { serializeHostMessage, type ServerMessage, type SettingsMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

// 帮助页的「游戏兼容」：把水杉在系统里登记的名称换成 CS2 / Dota 2 白名单里的名字。
// 权威在系统注册表（需要管理员权限才能改），页面不存副本：每次帮助页显示、窗口重新获得
// 焦点都向宿主重新查询。

type GameCompatAction = Extract<SettingsMessage, { type: 'gameCompatRequest' }>['data']['action'];
type GameCompatResponse = Extract<ServerMessage, { type: 'gameCompatResponse' }>;

export const GAME_COMPAT_FALLBACK_NAME = '拼音输入法';
export const GAME_COMPAT_NAME_MAX_LENGTH = 64;

let requestCounter = 0;
const pendingRequests = new Map<string, GameCompatAction>();
let defaultName = GAME_COMPAT_FALLBACK_NAME;

// 开启时空名称落到建议值；关闭时不带名称，宿主据此恢复真名。
export function resolveGameCompatName(enabled: boolean, raw: string, fallback: string): string {
  if (!enabled) return '';
  const trimmed = raw.trim();
  return (trimmed || fallback).slice(0, GAME_COMPAT_NAME_MAX_LENGTH);
}

export function describeGameCompatStatus(
  response: Pick<GameCompatResponse, 'enabled' | 'name' | 'registeredName'>,
): string {
  const registered = response.registeredName ? `当前在系统中登记的名称：${response.registeredName}` : '';
  if (!response.enabled) return registered ? `未启用。${registered}` : '未启用';
  if (response.name && response.registeredName && response.name !== response.registeredName) {
    return `已启用「${response.name}」，但系统中登记的仍是「${response.registeredName}」，请点「应用」重新登记`;
  }
  return registered ? `已启用。${registered}` : '已启用';
}

function element<T extends HTMLElement>(id: string, type: { new (): T }): T | null {
  const node = document.getElementById(id);
  return node instanceof type ? node : null;
}

function setStatus(message: string, kind: 'idle' | 'success' | 'error'): void {
  const status = document.getElementById('gameCompatStatus');
  if (!status) return;
  status.textContent = message;
  status.dataset.kind = kind;
}

function syncNameInput(): void {
  const toggle = element('gameCompatEnabled', HTMLInputElement);
  const input = element('gameCompatName', HTMLInputElement);
  if (input) input.disabled = !toggle?.checked;
}

function setBusy(busy: boolean): void {
  const button = element('gameCompatApplyButton', HTMLButtonElement);
  if (!button) return;
  button.disabled = busy;
  button.textContent = busy ? '正在应用…' : '应用';
}

function post(action: GameCompatAction, extra: { enabled?: boolean; name?: string } = {}): void {
  const webview = window.chrome?.webview;
  if (!webview) return;
  const requestId = `game-compat-${++requestCounter}`;
  pendingRequests.set(requestId, action);
  webview.postMessage(serializeHostMessage({ type: 'gameCompatRequest', data: { requestId, action, ...extra } }));
}

function applyResponseState(response: GameCompatResponse): void {
  if (response.defaultName) defaultName = response.defaultName;
  const toggle = element('gameCompatEnabled', HTMLInputElement);
  const input = element('gameCompatName', HTMLInputElement);
  if (input) input.placeholder = defaultName;
  if (toggle && typeof response.enabled === 'boolean') toggle.checked = response.enabled;
  // 正在编辑时不回填，免得吞掉用户还没提交的字。
  if (input && document.activeElement !== input && typeof response.name === 'string') input.value = response.name;
  syncNameInput();
}

function handleResponse(response: GameCompatResponse, action: GameCompatAction): void {
  applyResponseState(response);
  if (action === 'apply') {
    setBusy(false);
    if (!response.ok) {
      setStatus(response.message || '应用失败，请重试', 'error');
      return;
    }
    setStatus(response.message || describeGameCompatStatus(response), 'success');
    return;
  }
  setBusy(false);
  if (!response.ok) {
    setStatus(response.message || '无法读取游戏兼容状态', 'error');
    return;
  }
  setStatus(describeGameCompatStatus(response), response.enabled ? 'success' : 'idle');
}

function isHelpPageVisible(): boolean {
  const root = document.getElementById('help-settings');
  return !!root && root.getClientRects().length > 0 && document.visibilityState === 'visible';
}

function refreshStatus(): void {
  if (!isHelpPageVisible()) return;
  // 应用请求在途时不插查询：它的回复本身就带最新状态。
  for (const action of pendingRequests.values()) {
    if (action === 'apply') return;
  }
  post('status');
}

export function setupGameCompat(): void {
  onHostMessage('gameCompatResponse', (payload) => {
    const action = pendingRequests.get(payload.requestId);
    if (!action) return;
    pendingRequests.delete(payload.requestId);
    handleResponse(payload, action);
  });

  element('gameCompatEnabled', HTMLInputElement)?.addEventListener('change', syncNameInput);
  element('gameCompatApplyButton', HTMLButtonElement)?.addEventListener('click', () => {
    const enabled = element('gameCompatEnabled', HTMLInputElement)?.checked ?? false;
    const raw = element('gameCompatName', HTMLInputElement)?.value ?? '';
    const name = resolveGameCompatName(enabled, raw, defaultName);
    setBusy(true);
    setStatus('正在应用，请在弹出的 UAC 窗口中确认…', 'idle');
    post('apply', enabled ? { enabled, name } : { enabled });
  });

  const root = document.getElementById('help-settings');
  if (root && typeof IntersectionObserver !== 'undefined') {
    new IntersectionObserver((entries) => {
      if (entries.some((entry) => entry.isIntersecting)) refreshStatus();
    }).observe(root);
  }
  window.addEventListener('focus', refreshStatus);
  document.addEventListener('visibilitychange', refreshStatus);

  setBusy(true);
  post('status');
}
