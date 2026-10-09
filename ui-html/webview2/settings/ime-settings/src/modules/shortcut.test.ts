import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyShortcutConfig, setupShortcut } from './shortcut';
import { updateConfig } from './config-sync';

vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));

class Checkbox extends EventTarget {
  checked = true;
}

let characterSet: Checkbox;
let trilingualCycle: Checkbox;
beforeEach(() => {
  vi.clearAllMocks();
  characterSet = new Checkbox();
  trilingualCycle = new Checkbox();
  trilingualCycle.checked = false;
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      if (id === 'characterSetShortcutCheckbox') return characterSet;
      if (id === 'trilingualCycleCheckbox') return trilingualCycle;
      return null;
    },
    querySelectorAll: () => [],
  });
});
afterEach(() => vi.unstubAllGlobals());

it('applies the persisted shortcut setting without writing it back', () => {
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: false });
  expect(characterSet.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: true });
  expect(characterSet.checked).toBe(true);
});

it('writes only the character-set shortcut setting when the user disables it', () => {
  setupShortcut();
  characterSet.checked = false;
  characterSet.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_character_set_ctrl_shift_f', false);
});

it('applies the persisted trilingual cycle switch without writing it back', () => {
  applyShortcutConfig({ trilingual_cycle: true });
  expect(trilingualCycle.checked).toBe(true);
  applyShortcutConfig({ trilingual_cycle: false });
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

it('writes only the trilingual cycle switch when the user toggles it', () => {
  setupShortcut();
  trilingualCycle.checked = true;
  trilingualCycle.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.trilingual_cycle', true);
});

it('accepts older configuration snapshots with no shortcut field', () => {
  applyShortcutConfig(undefined);
  applyShortcutConfig({ switch_language_shift: false });
  expect(characterSet.checked).toBe(true);
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});
