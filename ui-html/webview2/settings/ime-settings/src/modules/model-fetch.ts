import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

export type ModelListService = 'ai.assistant';

export interface ModelFetchOptions {
  buttonId: string;
  statusId: string;
  menuId: string;
  service: () => ModelListService;
  readConfig: () => Record<string, string>;
}

let nextRequestId = 0;

/**
 * 「获取模型」按钮：点一下按当前服务商和凭据拉取模型列表，把结果灌进同字段的下拉菜单。
 * 模型本身仍是可自由输入的文本框，菜单只是候选来源，清空菜单不影响手动填写。
 *
 * 返回的 reset 清空菜单和状态，并丢弃尚未返回的请求：切换服务商后旧请求的结果不再适用。
 */
export function setupModelFetch(options: ModelFetchOptions): () => void {
  const button = document.getElementById(options.buttonId) as HTMLButtonElement | null;
  const status = document.getElementById(options.statusId);
  const menu = document.getElementById(options.menuId);
  if (!button || !menu) return () => {};

  let pendingRequestId = '';
  const idleLabel = button.textContent?.trim() || '获取模型';
  const settle = (): void => {
    pendingRequestId = '';
    button.disabled = false;
    button.textContent = idleLabel;
  };

  onHostMessage('apiModelListResult', payload => {
    if (!pendingRequestId || payload.requestId !== pendingRequestId) return;
    settle();

    if (!payload.ok) {
      setStatus(status, payload.message, 'error');
      return;
    }

    const models = (payload.models ?? []).filter(model => model.trim().length > 0);
    renderModels(menu, models);
    if (models.length === 0) {
      setStatus(status, payload.message || '服务商没有返回可用模型', 'error');
      return;
    }

    setStatus(status, `获取到 ${models.length} 个模型`, 'success');
    document.querySelectorAll('.dropdown-menu.open').forEach(openMenu => {
      if (openMenu !== menu) openMenu.classList.remove('open');
    });
    menu.classList.add('open');
  });

  button.addEventListener('click', () => {
    if (!window.chrome?.webview || pendingRequestId) return;
    pendingRequestId = `${Date.now()}-${++nextRequestId}`;
    button.disabled = true;
    button.textContent = '获取中…';
    setStatus(status, '正在向服务商查询模型列表…', 'pending');
    window.chrome.webview.postMessage(serializeHostMessage({
      type: 'apiModelList',
      data: { requestId: pendingRequestId, service: options.service(), config: options.readConfig() }
    }));
  });

  return () => {
    settle();
    menu.classList.remove('open');
    menu.replaceChildren();
    if (status) {
      status.textContent = '';
      delete status.dataset.kind;
    }
  };
}

function renderModels(menu: HTMLElement, models: string[]): void {
  menu.replaceChildren(...models.map(model => {
    const item = document.createElement('div');
    item.className = 'dropdown-item';
    item.dataset.value = model;
    item.textContent = model;
    item.title = model;
    return item;
  }));
}

function setStatus(status: HTMLElement | null, message: string, kind: string): void {
  if (!status) return;
  status.textContent = message;
  status.dataset.kind = kind;
}
