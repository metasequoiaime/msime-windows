import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { confirmDialog } from './confirm-dialog';

class TestElement extends EventTarget {
  textContent = '';
  children: TestElement[] = [];
  parent: TestElement | null = null;
  setAttribute(): void {}
  append(...children: TestElement[]): void {
    children.forEach(child => this.appendChild(child));
  }
  appendChild(child: TestElement): void {
    child.parent = this;
    this.children.push(child);
  }
  remove(): void {
    if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this);
  }
  focus(): void { doc.activeElement = this; }
}

class TestDocument extends EventTarget {
  body = new TestElement();
  activeElement: TestElement | null = null;
  createElement(): TestElement { return new TestElement(); }
}

let doc: TestDocument;
let previousFocus: TestElement;

beforeEach(() => {
  doc = new TestDocument();
  previousFocus = new TestElement();
  previousFocus.focus();
  vi.stubGlobal('document', doc);
});

afterEach(() => vi.unstubAllGlobals());

function buttons(): TestElement[] {
  return doc.body.children[0]!.children[0]!.children[2]!.children;
}

function press(key: string): void {
  const event = new Event('keydown', { cancelable: true });
  Object.defineProperty(event, 'key', { value: key });
  doc.dispatchEvent(event);
  expect(event.defaultPrevented).toBe(true);
}

it('cancels when Enter is pressed with Cancel focused', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  buttons()[0]!.focus();
  press('Enter');
  expect(await result).toBe(false);
  expect(doc.body.children).toHaveLength(0);
  expect(doc.activeElement).toBe(previousFocus);
});

it('confirms when Enter is pressed with Confirm focused', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  expect(doc.activeElement).toBe(buttons()[1]);
  press('Enter');
  expect(await result).toBe(true);
});

it('cancels on Escape even with Confirm focused', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  press('Escape');
  expect(await result).toBe(false);
});

it.each([[0, false], [1, true]] as const)('keeps button %i click behavior', async (index, expected) => {
  const result = confirmDialog('确定删除测试词条吗？');
  buttons()[index]!.dispatchEvent(new Event('click'));
  expect(await result).toBe(expected);
});
