import { describe, expect, it } from 'vitest';
import {
  addDays,
  calendarLevel,
  calendarStart,
  computeCalendarCellSize,
  computeCalendarWeeks,
  dateToDayKey,
  dayKeyToDate,
  detailRows,
  formatDayKey,
  formatDuration,
  formatNumber,
  formatSpeed,
  pad2,
  readableOf,
  retentionValueOf,
  RETENTION_VALUES,
  speedOf,
  totalOf
} from './stats';
import type { StatsDailyRow } from './stats';

function row(dayKey: number, cjk: number, activeMs = 0): StatsDailyRow {
  return { dayKey, cjk, latin: 0, digit: 0, punct: 0, other: 0, activeMs };
}

describe('日期与格式化', () => {
  it('pad2 补零', () => {
    expect(pad2(0)).toBe('00');
    expect(pad2(7)).toBe('07');
    expect(pad2(12)).toBe('12');
  });

  it('formatDayKey 输出 YYYY-MM-DD，0 显示占位符', () => {
    expect(formatDayKey(20250105)).toBe('2025-01-05');
    expect(formatDayKey(19991231)).toBe('1999-12-31');
    expect(formatDayKey(0)).toBe('-');
  });

  it('dayKey 与 Date 往返无损', () => {
    expect(dayKeyToDate(20250105).getTime()).toBe(new Date(2025, 0, 5).getTime());
    expect(dateToDayKey(new Date(2025, 0, 5))).toBe(20250105);
    expect(dateToDayKey(dayKeyToDate(20241231))).toBe(20241231);
  });

  it('addDays 跨月跨界', () => {
    expect(dateToDayKey(addDays(new Date(2025, 0, 31), 1))).toBe(20250201);
    expect(dateToDayKey(addDays(new Date(2025, 2, 1), -1))).toBe(20250228);
  });

  it('totalOf 汇总五类字符', () => {
    const full: StatsDailyRow = { dayKey: 20250101, cjk: 1, latin: 2, digit: 3, punct: 4, other: 5, activeMs: 0 };
    expect(totalOf(full)).toBe(15);
    expect(totalOf(row(20250101, 7))).toBe(7);
  });

  it('readableOf 只数中文与英文', () => {
    const full: StatsDailyRow = { dayKey: 20250101, cjk: 1, latin: 2, digit: 3, punct: 4, other: 5, activeMs: 0 };
    expect(readableOf(full)).toBe(3);
    expect(readableOf(row(20250101, 7))).toBe(7);
  });

  it('speedOf 只按中文+英文计算，标点/数字不影响速度', () => {
    const quiet: StatsDailyRow = { dayKey: 20250101, cjk: 30, latin: 30, digit: 0, punct: 0, other: 0, activeMs: 60_000 };
    const noisy: StatsDailyRow = {
      dayKey: 20250101,
      cjk: 30,
      latin: 30,
      digit: 500,
      punct: 500,
      other: 500,
      activeMs: 60_000
    };
    expect(speedOf(quiet)).toBe(60);
    expect(speedOf(noisy)).toBe(60);
    expect(speedOf({ ...quiet, cjk: 60, latin: 60 })).toBe(120);
    expect(speedOf({ ...quiet, activeMs: 0 })).toBe(0);
  });

  // 分母下限：不足一分钟按一分钟算，与 server 的 kMinSpeedActiveMs 同口径。
  it('speedOf 活跃时间不足一分钟时按一分钟算', () => {
    // 真实记录：81 个可读字符 / 1.9 秒活跃，按真实时间外推是 2503 字/分。
    expect(speedOf({ dayKey: 20250101, cjk: 19, latin: 62, digit: 10, punct: 0, other: 0, activeMs: 1942 })).toBe(81);
    expect(speedOf({ dayKey: 20250101, cjk: 50, latin: 0, digit: 0, punct: 0, other: 0, activeMs: 30_000 })).toBe(50);
    expect(speedOf({ dayKey: 20250101, cjk: 50, latin: 0, digit: 0, punct: 0, other: 0, activeMs: 1 })).toBe(50);
    // 超过下限后回到真实速率。
    expect(speedOf({ dayKey: 20250101, cjk: 120, latin: 0, digit: 0, punct: 0, other: 0, activeMs: 120_000 })).toBe(60);
    // 完全没有活跃时间是缺样本，不是样本少，仍为 0。
    expect(speedOf({ dayKey: 20250101, cjk: 50, latin: 0, digit: 0, punct: 0, other: 0, activeMs: 0 })).toBe(0);
    expect(speedOf({ dayKey: 20250101, cjk: 0, latin: 0, digit: 0, punct: 0, other: 0, activeMs: 1942 })).toBe(0);
  });

  it('formatNumber 千分位', () => {
    expect(formatNumber(0)).toBe('0');
    expect(formatNumber(1234)).toBe('1,234');
    expect(formatNumber(12.6)).toBe('13');
  });

  it('formatDuration 分档', () => {
    expect(formatDuration(0)).toBe('0 分钟');
    expect(formatDuration(30_000)).toBe('不足 1 分钟');
    expect(formatDuration(5 * 60_000)).toBe('5.0 分钟');
    expect(formatDuration(45 * 60_000)).toBe('45 分钟');
    expect(formatDuration(90 * 60_000)).toBe('1 小时 30 分');
  });

  it('formatSpeed 取整并带单位', () => {
    expect(formatSpeed(0)).toBe('0 字/分');
    expect(formatSpeed(61.6)).toBe('62 字/分');
  });
});

describe('热力图分档', () => {
  it('无记录或历史最高为 0 时是 0 档', () => {
    expect(calendarLevel(0, 100)).toBe(0);
    expect(calendarLevel(10, 0)).toBe(0);
  });

  it('按占最高日比例分五档，边界取低档', () => {
    expect(calendarLevel(1, 100)).toBe(1);
    expect(calendarLevel(25, 100)).toBe(1);
    expect(calendarLevel(26, 100)).toBe(2);
    expect(calendarLevel(50, 100)).toBe(2);
    expect(calendarLevel(51, 100)).toBe(3);
    expect(calendarLevel(75, 100)).toBe(3);
    expect(calendarLevel(76, 100)).toBe(4);
    expect(calendarLevel(100, 100)).toBe(4);
    expect(calendarLevel(200, 100)).toBe(4);
  });
});

describe('热力图窗口', () => {
  it('宽度不足时保底 53 周（约 12 个月）', () => {
    expect(computeCalendarWeeks(0)).toBe(53);
    expect(computeCalendarWeeks(400)).toBe(53);
    expect(computeCalendarWeeks(53 * 15)).toBe(53);
  });

  it('更宽的容器容纳更多周，且有上限', () => {
    expect(computeCalendarWeeks(54 * 15)).toBe(54);
    expect(computeCalendarWeeks(100 * 15)).toBe(100);
    expect(computeCalendarWeeks(100_000)).toBe(200);
  });

  it('窄容器自动缩小格子以容纳 53 周', () => {
    expect(computeCalendarCellSize(612, 53)).toBe(9);
    expect(computeCalendarCellSize(795, 53)).toBe(13);
    expect(computeCalendarCellSize(1600, 106)).toBe(13);
    expect(computeCalendarCellSize(0, 53)).toBe(9);
    expect(computeCalendarCellSize(300, 53)).toBe(9);
  });

  it('起点向前对齐到周一且覆盖约 53 周', () => {
    const today = new Date(2025, 0, 15); // 周三
    const start = calendarStart(today, 53);
    expect(start.getDay()).toBe(1);
    const coveredDays = Math.round((today.getTime() - start.getTime()) / 86_400_000);
    expect(coveredDays).toBeGreaterThanOrEqual(53 * 7 - 7);
    expect(coveredDays).toBeLessThanOrEqual(53 * 7 + 5);
  });

  it('起点不晚于覆盖窗口起点，并对齐到周一', () => {
    const today = new Date(2025, 0, 15); // 周三
    const start = calendarStart(today, 1); // today-6 天是周四，回退到周一
    expect(start.getDay()).toBe(1);
    expect(dateToDayKey(start)).toBe(20250106);
  });

  it('渲染列数最多比 weeks 多一列，格子必须按 weeks+1 计算', () => {
    const base = new Date(2025, 0, 12); // 周日
    for (let offset = 0; offset < 7; offset++) {
      const today = addDays(base, offset);
      const start = calendarStart(today, 53);
      const columns = Math.floor(Math.round((today.getTime() - start.getTime()) / 86_400_000) / 7) + 1;
      expect(columns).toBeGreaterThanOrEqual(53);
      expect(columns).toBeLessThanOrEqual(54);
    }
  });
});

describe('按日明细', () => {
  const daily = Array.from({ length: 40 }, (_, index) => row(20250101 + index, index));

  it('只取最近 30 天并按日期倒序', () => {
    const rows = detailRows(daily, 30);
    expect(rows).toHaveLength(30);
    expect(rows[0].dayKey).toBe(daily[daily.length - 1].dayKey);
    expect(rows[rows.length - 1].dayKey).toBe(daily[daily.length - 30].dayKey);
  });

  it('记录不足 30 天时全部返回', () => {
    const rows = detailRows(daily.slice(0, 3), 30);
    expect(rows.map((item) => item.dayKey)).toEqual([daily[2].dayKey, daily[1].dayKey, daily[0].dayKey]);
  });
});

describe('保留策略回填', () => {
  it('契约枚举内的值原样返回', () => {
    for (const value of RETENTION_VALUES) {
      expect(retentionValueOf(value)).toBe(value);
    }
  });

  it('未知值返回 null，不覆盖下拉', () => {
    expect(retentionValueOf('2d')).toBeNull();
    expect(retentionValueOf('')).toBeNull();
    expect(retentionValueOf('FOREVER')).toBeNull();
    expect(retentionValueOf('30')).toBeNull();
  });
});
