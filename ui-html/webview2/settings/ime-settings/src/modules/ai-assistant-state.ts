// AI 辅助当前服务商配置的页面内副本。候选词翻译选「AI 辅助」时复用这份配置，
// AI 辅助页改了服务商、地址、Token 或模型要立刻反映到翻译区的提示和摘要上，
// 而配置写入不会回推新的 configSnapshot，所以由 AI 辅助页主动更新这里。

export type AiAssistantState = {
  provider: string;
  endpoint: string;
  model: string;
  token: string;
};

let state: AiAssistantState = { provider: '', endpoint: '', model: '', token: '' };
const listeners = new Set<() => void>();

export function getAiAssistantState(): AiAssistantState {
  return state;
}

export function setAiAssistantState(next: AiAssistantState): void {
  state = { ...next };
  listeners.forEach((listener) => listener());
}

export function onAiAssistantStateChange(listener: () => void): void {
  listeners.add(listener);
}

// 与服务端 AiTranslation::IsUsableConfig / CloudTranslation::IsUsableSecret 同一规则：
// 出厂模板里的 <YOUR_...> 和 FAKESECRET_ 占位符不算已填写。
export function isUsableAiAssistantState(value: AiAssistantState): boolean {
  const token = value.token.trim();
  const placeholder = (token.startsWith('<') && token.endsWith('>')) || token.startsWith('FAKESECRET_');
  return Boolean(token) && !placeholder && Boolean(value.model.trim()) && /^https?:\/\//.test(value.endpoint.trim());
}
