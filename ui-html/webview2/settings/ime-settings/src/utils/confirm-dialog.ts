type ConfirmOptions = { title?: string; confirmText?: string; cancelText?: string };

// 自绘确认框，替代 window.confirm()：系统对话框不随设置页主题，在 WebView2 里还是宿主
// 模态窗口，弹出期间页面自己的键盘和焦点处理全被挂起。样式复用词条编辑弹窗的 .dict-modal。
export function confirmDialog(message: string, options: ConfirmOptions = {}): Promise<boolean> {
  const { title = '确认删除', confirmText = '删除', cancelText = '取消' } = options;
  return new Promise((resolve) => {
    const previousFocus = document.activeElement as HTMLElement | null;
    const modal = document.createElement('div');
    modal.className = 'dict-modal open';
    const dialog = document.createElement('div');
    dialog.className = 'dict-dialog dict-confirm-dialog';
    dialog.setAttribute('role', 'alertdialog');
    dialog.setAttribute('aria-modal', 'true');
    dialog.setAttribute('aria-label', title);
    const titleElement = document.createElement('div');
    titleElement.className = 'dict-dialog-title';
    titleElement.textContent = title;
    const messageElement = document.createElement('div');
    messageElement.className = 'dict-confirm-message';
    messageElement.textContent = message;
    const actions = document.createElement('div');
    actions.className = 'dict-dialog-actions';
    const cancel = document.createElement('button');
    cancel.type = 'button'; cancel.className = 'dict-button secondary'; cancel.textContent = cancelText;
    const confirm = document.createElement('button');
    confirm.type = 'button'; confirm.className = 'dict-button danger'; confirm.textContent = confirmText;
    actions.append(cancel, confirm);
    dialog.append(titleElement, messageElement, actions);
    modal.appendChild(dialog);
    // 必须挂在 body 下，理由见 hoistOverlay()
    document.body.appendChild(modal);

    const close = (result: boolean): void => {
      document.removeEventListener('keydown', onKeydown, true);
      modal.remove();
      previousFocus?.focus?.();
      resolve(result);
    };
    // 捕获阶段处理，免得 Esc 被页面上别的弹窗的全局监听先吃掉
    function onKeydown(event: KeyboardEvent): void {
      if (event.key !== 'Escape' && event.key !== 'Enter') return;
      event.preventDefault();
      event.stopPropagation();
      close(event.key === 'Enter' && document.activeElement !== cancel);
    }
    document.addEventListener('keydown', onKeydown, true);
    cancel.addEventListener('click', () => close(false));
    confirm.addEventListener('click', () => close(true));
    modal.addEventListener('mousedown', (event) => { if (event.target === modal) close(false); });
    confirm.focus();
  });
}
