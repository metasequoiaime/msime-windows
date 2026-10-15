import { describe, expect, it } from 'vitest';
import { describeGameCompatStatus, resolveGameCompatName } from './game-compat';

describe('game compat name', () => {
  it('falls back to the suggested name and clears it when disabled', () => {
    expect(resolveGameCompatName(true, '  ', '拼音输入法')).toBe('拼音输入法');
    expect(resolveGameCompatName(true, ' 中文 - QQ拼音输入法 ', '拼音输入法')).toBe('中文 - QQ拼音输入法');
    expect(resolveGameCompatName(false, '拼音输入法', '拼音输入法')).toBe('');
    expect(resolveGameCompatName(true, 'x'.repeat(80), '拼音输入法')).toHaveLength(64);
  });
});

describe('game compat status text', () => {
  it('reports disabled, enabled and out-of-sync states', () => {
    expect(describeGameCompatStatus({ enabled: false })).toBe('未启用');
    expect(describeGameCompatStatus({ enabled: false, registeredName: 'Metasequoia 水杉输入法' })).toBe(
      '未启用。当前在系统中登记的名称：Metasequoia 水杉输入法',
    );
    expect(describeGameCompatStatus({ enabled: true, name: '拼音输入法', registeredName: '拼音输入法' })).toBe(
      '已启用。当前在系统中登记的名称：拼音输入法',
    );
    expect(
      describeGameCompatStatus({ enabled: true, name: '拼音输入法', registeredName: 'Metasequoia 水杉输入法' }),
    ).toContain('请点「应用」重新登记');
  });
});
