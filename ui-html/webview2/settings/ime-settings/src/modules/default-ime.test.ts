import { describe, expect, it } from 'vitest';
import { describeDefaultImeStatus } from './default-ime';

describe('default input method status text', () => {
  it('reports default, missing and non-default states', () => {
    expect(describeDefaultImeStatus({ enabled: true, isDefault: true })).toBe('水杉输入法已是默认输入法');
    expect(describeDefaultImeStatus({ enabled: false, isDefault: false })).toContain('不在当前用户的键盘列表中');
    expect(describeDefaultImeStatus({ enabled: true, isDefault: false })).toBe('当前默认输入法不是水杉输入法');
  });
});
