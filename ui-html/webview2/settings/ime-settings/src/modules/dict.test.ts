import { afterEach, beforeEach, expect, it, vi } from 'vitest';

vi.mock('../utils/dictionary-file', () => ({ readDictionaryFile: vi.fn() }));
vi.mock('../utils/dictionary-pagination', () => ({ createDictionaryPager: vi.fn(), DICTIONARY_PAGE_SIZE: 50 }));
vi.mock('../utils/host-messages', () => ({ onHostMessage: vi.fn() }));
vi.mock('../utils/overlay-host', () => ({ hoistOverlay: vi.fn() }));

import { readDictionaryFile } from '../utils/dictionary-file';
import { onHostMessage } from '../utils/host-messages';

class TestElement extends EventTarget {
  value = '';
  textContent = '';
  innerHTML = '';
  placeholder = '';
  dataset: Record<string, string> = {};
  style: Record<string, string> = {};
  files = [{} as File];
  classes = new Set<string>();
  classList = {
    add: (name: string) => { this.classes.add(name); },
    remove: (name: string) => { this.classes.delete(name); },
    contains: (name: string) => this.classes.has(name),
  };
  setAttribute(): void {}
}

let elements: Map<string, TestElement>;
let tabs: TestElement[];
let postMessage: ReturnType<typeof vi.fn>;

beforeEach(async () => {
  vi.resetModules();
  vi.clearAllMocks();
  elements = new Map();
  for (const id of ['dictSearch', 'dictHint', 'dictTableHeader', 'dictRows', 'dictImportFile', 'dictImportHansFile', 'dictModal']) {
    elements.set(id, new TestElement());
  }
  tabs = ['quanpin', 'wubi', 'english'].map(dictionary => {
    const tab = new TestElement();
    tab.dataset.dictionary = dictionary;
    return tab;
  });
  tabs[0]!.classList.add('active');
  postMessage = vi.fn();
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    querySelectorAll: (selector: string) => selector === '.dict-tab' ? tabs : [],
    querySelector: (selector: string) => selector === '.dict-tab.active'
      ? tabs.find(tab => tab.classList.contains('active')) : null,
    addEventListener: vi.fn(),
  });
  vi.stubGlobal('window', {
    chrome: { webview: { postMessage } },
    requestAnimationFrame: vi.fn(),
    addEventListener: vi.fn(),
  });
  vi.stubGlobal('MsimeProtocol', {
    version: 1,
    validate: () => true,
    serializeClientMessage: (message: unknown) => JSON.stringify(message),
  });
  const { setupDictionary } = await import('./dict');
  setupDictionary();
});

afterEach(() => vi.unstubAllGlobals());

it.each([
  ['dictImportFile', 'import', '测试\tceshi'],
  ['dictImportHansFile', 'importHans', '测试'],
])('keeps the selected dictionary while %s is being read', async (inputId, action, content) => {
  let finishRead!: (content: string) => void;
  vi.mocked(readDictionaryFile).mockReturnValueOnce(new Promise(resolve => { finishRead = resolve; }));
  elements.get(inputId)!.dispatchEvent(new Event('change'));
  expect(postMessage).not.toHaveBeenCalled();
  tabs[1]!.dispatchEvent(new Event('click'));
  finishRead(content);
  await Promise.resolve();

  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  expect(request.data).toMatchObject({ action, dictionary: 'quanpin', content });

  // An import completed on another tab must not close the current tab's editor.
  elements.get('dictModal')!.classList.add('open');
  const response = vi.mocked(onHostMessage).mock.calls.find(([type]) => type === 'dictionaryResponse')![1];
  response({ type: 'dictionaryResponse', requestId: request.data.requestId, ok: true, message: '导入成功', rows: [] });
  expect(elements.get('dictModal')!.classList.contains('open')).toBe(true);
});

it('imports into the active dictionary when the tab does not change', async () => {
  tabs[1]!.dispatchEvent(new Event('click'));
  vi.mocked(readDictionaryFile).mockResolvedValueOnce('测试\tiya');
  elements.get('dictImportFile')!.dispatchEvent(new Event('change'));
  await Promise.resolve();
  expect(JSON.parse(postMessage.mock.calls[0]![0]).data).toMatchObject({
    action: 'import', dictionary: 'wubi', content: '测试\tiya',
  });
});
