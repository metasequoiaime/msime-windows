import { updateConfig } from './config-sync';

export function applyShortcutConfig(config: {
  switch_language_shift?: boolean;
  switch_language_ctrl?: boolean;
  switch_language_ctrl_alt_space?: boolean;
  toggle_character_set_ctrl_shift_f?: boolean;
  trilingual_cycle?: boolean;
} | undefined): void {
  if (!config) {
    return;
  }
  const shift = document.getElementById('switchLanguageShiftCheckbox') as HTMLInputElement | null;
  const ctrl = document.getElementById('switchLanguageCtrlCheckbox') as HTMLInputElement | null;
  const ctrlAltSpace = document.getElementById('switchLanguageCtrlAltSpaceCheckbox') as HTMLInputElement | null;
  const characterSet = document.getElementById('characterSetShortcutCheckbox') as HTMLInputElement | null;
  const trilingualCycle = document.getElementById('trilingualCycleCheckbox') as HTMLInputElement | null;
  if (characterSet && typeof config.toggle_character_set_ctrl_shift_f === 'boolean') {
    characterSet.checked = config.toggle_character_set_ctrl_shift_f;
  }
  if (shift && typeof config.switch_language_shift === 'boolean') {
    shift.checked = config.switch_language_shift;
  }
  if (ctrl && typeof config.switch_language_ctrl === 'boolean') {
    ctrl.checked = config.switch_language_ctrl;
  }
  if (ctrlAltSpace && typeof config.switch_language_ctrl_alt_space === 'boolean') {
    ctrlAltSpace.checked = config.switch_language_ctrl_alt_space;
  }
  if (trilingualCycle && typeof config.trilingual_cycle === 'boolean') {
    trilingualCycle.checked = config.trilingual_cycle;
  }
}

export function setupShortcut(): void {
  const characterSet = document.getElementById('characterSetShortcutCheckbox') as HTMLInputElement | null;
  characterSet?.addEventListener('change', () => {
    updateConfig('keybindings.toggle_character_set_ctrl_shift_f', characterSet.checked);
  });
  const trilingualCycle = document.getElementById('trilingualCycleCheckbox') as HTMLInputElement | null;
  trilingualCycle?.addEventListener('change', () => {
    updateConfig('keybindings.trilingual_cycle', trilingualCycle.checked);
  });
  const mapping: Record<string, string> = {
    shift: 'keybindings.switch_language_shift',
    ctrl: 'keybindings.switch_language_ctrl',
    'ctrl-alt-space': 'keybindings.switch_language_ctrl_alt_space'
  };
  document.querySelectorAll<HTMLInputElement>('input[name="switch-language"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      const path = mapping[checkbox.value];
      if (path) {
        updateConfig(path, checkbox.checked);
      }
    });
  });
}
