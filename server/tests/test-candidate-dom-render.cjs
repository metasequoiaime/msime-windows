const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(
  path.join(__dirname, '../src/webview2/windows_webview2_candidate.cpp'),
  'utf8',
);
const match = source.match(/constexpr wchar_t kRenderCandidateItemsScript\[\] = LR"MSIME_JS\(([\s\S]*?)\)MSIME_JS";/);
assert.ok(match, 'candidate DOM renderer must be present');

function element(tagName) {
  return {
    tagName,
    textContent: '',
    className: '',
    style: {},
    children: [],
    set innerHTML(_) {
      throw new Error('candidate text must not use innerHTML');
    },
    replaceChildren(...children) {
      this.children = children;
    },
    append(child) {
      this.children.push(child);
    },
  };
}

test('candidate DOM renderer keeps text, order, style, badge, and translation', () => {
  const slots = Array.from({ length: 3 }, () => element('div'));
  const wrappers = slots.map((slot) => ({
    querySelector(selector) {
      assert.equal(selector, '.cand-content');
      return slot;
    },
  }));
  const root = {
    querySelectorAll(selector) {
      assert.equal(selector, '.row-wrapper');
      return wrappers;
    },
  };
  const items = [
    { text: 'a,b\uF000', annotation: '<&', badge: '[固定]', translation: '译,\uF000<&', fixedPosition: true },
    { text: '${value}`', annotation: '&<', badge: '②', translation: '译${value}`', fixedPosition: false },
    { text: '末尾', annotation: '', badge: '', translation: '', fixedPosition: false },
  ];
  const context = { document: { createElement: element } };
  vm.runInNewContext(match[1], context);
  context.RenderCandidateItems(root, items);

  assert.equal(slots.length, items.length);
  assert.deepEqual(slots.map((slot) => slot.children.length), [2, 2, 1]);
  assert.deepEqual(
    slots.map((slot) => slot.children[0].textContent),
    items.map((item) => item.text + item.annotation + item.badge),
  );
  assert.deepEqual(
    slots.slice(0, 2).map((slot) => slot.children[1].textContent),
    items.slice(0, 2).map((item) => item.translation),
  );
  assert.deepEqual(slots.slice(0, 2).map((slot) => slot.children[1].className), [
    'cand-translation',
    'cand-translation',
  ]);
  assert.equal(slots[0].children[0].style.color, '#379AD3');
  assert.equal(slots[1].children[0].style.color, undefined);
  assert.equal(slots[2].children[0].style.color, undefined);
});
