import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage } from '../../../../shared/messages';
import type { ServerMessage, SettingsMessage } from '../../../../shared/messages';
import { applyToggleState, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';

type StatsRequest = Extract<SettingsMessage, { type: 'statsRequest' }>['data'];
type StatsResponse = Extract<ServerMessage, { type: 'statsResponse' }>;
type StatsOverview = NonNullable<StatsResponse['overview']>;
export type StatsDailyRow = StatsOverview['daily'][number];

export type RetentionValue = 'forever' | '30d' | '90d' | '180d' | '365d';

export const RETENTION_VALUES: readonly RetentionValue[] = ['forever', '30d', '90d', '180d', '365d'];
const STATISTICS_ENABLED_PATH = 'statistics.enabled';
const STATISTICS_RETENTION_PATH = 'statistics.retention';

/** 热力图列宽必须与 stats.css 里的默认值保持一致，否则自适应列数会算错。 */
const CALENDAR_CELL_PX = 13;
const CALENDAR_GAP_PX = 2;
const CALENDAR_COLUMN_PX = CALENDAR_CELL_PX + CALENDAR_GAP_PX;
/** 窄容器放不下 53 周时格子最多缩到 9px，保证默认窗口下不出现横向滚动。 */
const CALENDAR_MIN_CELL_PX = 9;
/** 热力图至少覆盖近 12 个月。 */
const CALENDAR_MIN_WEEKS = 53;
/** 超宽窗口也不渲染数年空网格。 */
const CALENDAR_MAX_WEEKS = 200;
/** 按日明细只展示最近 30 天（overview.daily 是全量历史）。 */
const DETAIL_DAYS = 30;
const STATUS_TIMEOUT_MS = 4000;
/** 统计页可见时的轮询间隔：一次查询只是 Server 端的一次 SQLite 只读快照。 */
const OVERVIEW_POLL_MS = 5000;
/** 焦点、可见性、轮询可能同时触发，这个间隔内只真正发一次查询。 */
const OVERVIEW_MIN_INTERVAL_MS = 1000;
/** 在途概览查询超过这个时间没回来，就当它丢了，允许再发。 */
const OVERVIEW_STALE_MS = 15000;

let statisticsEnabled = false;
let lastOverview: StatsOverview | null = null;
let lastOverviewSignature = '';
let calendarWeeks = CALENDAR_MIN_WEEKS;
let calendarCellPx = CALENDAR_CELL_PX;
let calendarObserver: ResizeObserver | null = null;
let requestCounter = 0;
let statusTimer: number | null = null;
let lastOverviewRequestAt = 0;
let overviewPollTimer: number | null = null;
const pendingRequests = new Map<string, StatsRequest['action']>();

// ------------------------------------------------------------------ 纯函数（单测覆盖）

export function pad2(value: number): string {
  return value < 10 ? `0${value}` : String(value);
}

export function formatDayKey(dayKey: number): string {
  if (!dayKey) {
    return '-';
  }
  const year = Math.floor(dayKey / 10000);
  const month = Math.floor(dayKey / 100) % 100;
  const day = dayKey % 100;
  return `${year}-${pad2(month)}-${pad2(day)}`;
}

export function dayKeyToDate(dayKey: number): Date {
  const year = Math.floor(dayKey / 10000);
  const month = Math.floor(dayKey / 100) % 100;
  const day = dayKey % 100;
  return new Date(year, month - 1, day);
}

export function dateToDayKey(date: Date): number {
  return date.getFullYear() * 10000 + (date.getMonth() + 1) * 100 + date.getDate();
}

export function addDays(date: Date, days: number): Date {
  const next = new Date(date);
  next.setDate(next.getDate() + days);
  return next;
}

export function totalOf(row: StatsDailyRow): number {
  return row.cjk + row.latin + row.digit + row.punct + row.other;
}

/**
 * 速度分子只数可读字符（中文 + 英文）；数字、标点和其他不计入，
 * 与 server/src/statistics/stats_types.h 的 DailyRow::SpeedChars 同口径。
 */
export function readableOf(row: StatsDailyRow): number {
  return row.cjk + row.latin;
}

/** 速度分母下限，与 server/src/statistics/stats_types.h 的 kMinSpeedActiveMs 同值。 */
const SPEED_MIN_ACTIVE_MS = 60_000;

/**
 * 按日速度：可读字符数 ÷ 活跃分钟。
 *
 * 活跃时间只累加相邻两次上屏之间的间隔，首键之前与末键之后的时间不在其中，零散
 * 输入的日子分母可能只有几秒。按它外推会得到没人打过的速率，所以不足一分钟时按
 * 一分钟算；完全没有活跃时间仍然返回 0——那是缺样本，不是样本少。
 */
export function speedOf(row: StatsDailyRow): number {
  const readable = readableOf(row);
  if (readable <= 0 || row.activeMs <= 0) {
    return 0;
  }
  const denominatorMs = Math.max(row.activeMs, SPEED_MIN_ACTIVE_MS);
  return readable / (denominatorMs / 60000);
}

export function formatNumber(value: number): string {
  return Math.round(value).toLocaleString('zh-CN');
}

export function formatDuration(ms: number): string {
  if (!ms || ms <= 0) {
    return '0 分钟';
  }
  const minutes = ms / 60000;
  if (minutes < 1) {
    return '不足 1 分钟';
  }
  if (minutes < 60) {
    return `${minutes < 10 ? minutes.toFixed(1) : Math.round(minutes)} 分钟`;
  }
  const hours = Math.floor(minutes / 60);
  const rest = Math.round(minutes % 60);
  return `${hours} 小时 ${rest} 分`;
}

export function formatSpeed(charsPerMinute: number): string {
  // 单位按可读字符（中+英）计，标点/数字不计入速度。
  return `${Math.round(charsPerMinute)} 字/分`;
}

/** 五级色阶：0 = 无记录，1-4 按当日字数占历史最高日的比例分档。 */
export function calendarLevel(chars: number, maxChars: number): number {
  if (chars <= 0 || maxChars <= 0) {
    return 0;
  }
  const ratio = chars / maxChars;
  if (ratio <= 0.25) {
    return 1;
  }
  if (ratio <= 0.5) {
    return 2;
  }
  if (ratio <= 0.75) {
    return 3;
  }
  return 4;
}

/** 按容器宽度算出热力图能容纳的周数（保底 12 个月，超宽窗口有上限）。 */
export function computeCalendarWeeks(containerWidth: number): number {
  const weeks = Math.floor((containerWidth + CALENDAR_GAP_PX) / CALENDAR_COLUMN_PX);
  return Math.max(CALENDAR_MIN_WEEKS, Math.min(CALENDAR_MAX_WEEKS, weeks));
}

/** 容器放不下 weeks 列时按比例缩小格子；宽裕时用满 13px，不再放大。 */
export function computeCalendarCellSize(containerWidth: number, weeks: number): number {
  const available = containerWidth - CALENDAR_GAP_PX * (weeks - 1);
  const cell = Math.floor(available / weeks);
  return Math.max(CALENDAR_MIN_CELL_PX, Math.min(CALENDAR_CELL_PX, cell));
}

/** 热力图起点：覆盖 weeks 周并向前对齐到周一，保证每列恰好一周。 */
export function calendarStart(today: Date, weeks: number): Date {
  const start = addDays(today, -(weeks * 7 - 1));
  return addDays(start, -((start.getDay() + 6) % 7));
}

/** 明细列表：取最近 days 天并按日期倒序（新的在上）。 */
export function detailRows(daily: StatsDailyRow[], days: number): StatsDailyRow[] {
  return daily.slice(-days).reverse();
}

/** 配置回填的守卫：只接受契约枚举内的值，其余一律丢弃。 */
export function retentionValueOf(value: string): RetentionValue | null {
  return (RETENTION_VALUES as readonly string[]).includes(value) ? (value as RetentionValue) : null;
}

// ------------------------------------------------------------------ DOM 工具

function byId<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function setHidden(id: string, hidden: boolean): void {
  const element = byId(id);
  if (element) {
    element.hidden = hidden;
  }
}

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  text?: string
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined) {
    node.textContent = text;
  }
  return node;
}

// ------------------------------------------------------------------ 渲染

function renderEmptyState(): void {
  const hasData = lastOverview?.hasData === true;
  setHidden('statsNoDataEmpty', !(statisticsEnabled && !hasData));
  setHidden('statsDisabledEmpty', statisticsEnabled);
  const disabledText = byId('statsDisabledText');
  if (disabledText) {
    disabledText.textContent = hasData
      ? '统计当前已关闭，历史数据仍保留在下方；重新开启后继续记录。'
      : '开启后这里会显示输入字数、速度与时段分布。统计只保存在本机，不记录输入内容，也不联网。';
  }
}

function renderOverview(overview: StatsOverview): void {
  // 轮询每隔几秒就回来一份概览，没打字时内容完全一样。各 render 都是整棵子树
  // replaceChildren，会抹掉热力图的横向滚动和正在看的 tooltip，所以数据没变就不重绘。
  const signature = JSON.stringify(overview);
  const unchanged = signature === lastOverviewSignature && lastOverview !== null;
  lastOverviewSignature = signature;
  lastOverview = overview;
  if (unchanged) {
    return;
  }
  renderEmptyState();
  const hasData = overview.hasData;
  setHidden('statsContent', !hasData);
  if (!hasData) {
    return;
  }
  // 重建热力图会把滚动位置打回 0，列数没变时原样放回去。
  const calendarBox = byId('statsCalendar');
  const calendarScroll = calendarBox?.scrollLeft ?? 0;
  renderCards(overview);
  renderSpeed(overview);
  renderCalendar(overview);
  renderHourly(overview);
  renderCategories(overview);
  renderDetails(overview);
  if (calendarBox) {
    calendarBox.scrollLeft = calendarScroll;
  }
}

function statCard(title: string, value: string, unit: string, note: string): HTMLElement {
  const node = el('div', 'stats-card');
  const valueRow = el('div', 'stats-card-value');
  valueRow.append(el('strong', undefined, value), el('span', 'stats-card-unit', unit));
  node.append(el('span', 'stats-card-title', title), valueRow, el('span', 'stats-card-note', note));
  return node;
}

function renderCards(overview: StatsOverview): void {
  const container = byId('statsCards');
  if (!container) {
    return;
  }
  container.replaceChildren(
    statCard('今日输入', formatNumber(overview.todayChars), '字', `活跃 ${formatDuration(overview.todayActiveMs)}`),
    statCard('累计输入', formatNumber(overview.totalChars), '字', `共记录 ${overview.days} 天`),
    statCard('日均输入', formatNumber(overview.averagePerDay), '字', `自 ${formatDayKey(overview.firstDayKey)}`),
    statCard('连续输入', formatNumber(overview.currentStreak), '天', `历史最长 ${overview.longestStreak} 天`)
  );
}

function renderSpeed(overview: StatsOverview): void {
  const grid = byId('statsSpeedGrid');
  if (grid) {
    const entries: Array<[string, number, string]> = [
      ['今日速度', overview.todaySpeed, formatDuration(overview.todayActiveMs)],
      ['平均速度', overview.averageSpeed, formatDuration(overview.totalActiveMs)],
      ['最快速度', overview.fastestSpeed, overview.fastestDayKey ? formatDayKey(overview.fastestDayKey) : '暂无']
    ];
    const fragment = document.createDocumentFragment();
    for (const [label, speed, note] of entries) {
      const item = el('div', 'stats-speed-item');
      item.append(
        el('span', 'stats-speed-label', label),
        el('strong', 'stats-speed-value', formatSpeed(speed)),
        el('span', 'stats-speed-note', note)
      );
      fragment.append(item);
    }
    grid.replaceChildren(fragment);
  }
  const best = byId('statsBestDay');
  if (best) {
    best.textContent = overview.bestDayKey
      ? `最高日：${formatDayKey(overview.bestDayKey)} · ${formatNumber(overview.bestDayChars)} 字`
      : '最高日：暂无记录';
  }
}

function renderCalendar(overview: StatsOverview): void {
  const months = byId('statsCalendarMonths');
  const grid = byId('statsCalendarGrid');
  if (!months || !grid) {
    return;
  }

  const daily = new Map<number, StatsDailyRow>();
  let maxChars = 1;
  for (const row of overview.daily ?? []) {
    daily.set(row.dayKey, row);
    maxChars = Math.max(maxChars, totalOf(row));
  }

  // 固定窗口：不按首条记录收缩，区间外没有数据时格子自然显示为空。
  const today = dayKeyToDate(overview.todayDayKey);
  const monthFragment = document.createDocumentFragment();
  const gridFragment = document.createDocumentFragment();
  let weekStart = calendarStart(today, calendarWeeks);
  while (weekStart.getTime() <= today.getTime()) {
    // 只在列内出现某月 1 日时标注月份，保证标签与该月的列对齐。
    const label = el('span', 'stats-month-label');
    for (let i = 0; i < 7; i++) {
      const date = addDays(weekStart, i);
      if (date.getDate() === 1) {
        label.textContent = `${date.getMonth() + 1}月`;
        break;
      }
    }
    monthFragment.append(label);

    for (let i = 0; i < 7; i++) {
      const date = addDays(weekStart, i);
      const dayKey = dateToDayKey(date);
      const row = daily.get(dayKey);
      const chars = row ? totalOf(row) : 0;
      const cell = el('i', `stats-cell lv${calendarLevel(chars, maxChars)}`);
      if (date.getTime() > today.getTime()) {
        cell.classList.add('out');
      }
      cell.title = `${formatDayKey(dayKey)}：${chars > 0 ? `${formatNumber(chars)} 字` : '无记录'}`;
      gridFragment.append(cell);
    }
    weekStart = addDays(weekStart, 7);
  }

  months.replaceChildren(monthFragment);
  grid.replaceChildren(gridFragment);
}

function renderHourly(overview: StatsOverview): void {
  const container = byId('statsHourly');
  if (!container) {
    return;
  }
  const values = overview.todayHourly ?? [];
  const max = Math.max(1, ...values);
  const fragment = document.createDocumentFragment();
  for (let hour = 0; hour < 24; hour++) {
    const value = values[hour] ?? 0;
    const column = el('div', 'stats-hour-col');
    const track = el('div', 'stats-hour-track');
    const bar = el('div', value > 0 ? 'stats-hour-bar' : 'stats-hour-bar is-empty');
    bar.style.height = `${Math.max(2, Math.round((value / max) * 100))}%`;
    bar.title = `${pad2(hour)} 时：${formatNumber(value)} 字`;
    track.append(bar);
    column.append(track, el('span', 'stats-hour-label', hour % 3 === 0 ? String(hour) : ''));
    fragment.append(column);
  }
  container.replaceChildren(fragment);
}

function renderCategories(overview: StatsOverview): void {
  const container = byId('statsCategories');
  if (!container) {
    return;
  }
  const categories = overview.categories;
  const total = categories.cjk + categories.latin + categories.digit + categories.punct + categories.other;
  const rows: Array<[string, number]> = [
    ['中文', categories.cjk],
    ['英文', categories.latin],
    ['数字', categories.digit],
    ['标点', categories.punct],
    ['其他', categories.other]
  ];
  const fragment = document.createDocumentFragment();
  rows.forEach(([name, value], index) => {
    const row = el('div', 'stats-cat-row');
    const track = el('div', 'stats-cat-track');
    const bar = el('div', `stats-cat-bar k${index}`);
    bar.style.width = total > 0 ? `${(value / total) * 100}%` : '0%';
    track.append(bar);
    const percent = total > 0 ? Math.round((value / total) * 100) : 0;
    row.append(el('span', 'stats-cat-name', name), track, el('span', 'stats-cat-stat', `${formatNumber(value)} 字 · ${percent}%`));
    fragment.append(row);
  });
  container.replaceChildren(fragment);
}

function renderDetails(overview: StatsOverview): void {
  const container = byId('statsDetails');
  if (!container) {
    return;
  }
  const table = el('table', 'stats-detail-table');
  const head = el('thead');
  const headRow = el('tr');
  for (const title of ['日期', '字数', '中文', '英文', '数字', '标点', '其他', '活跃', '速度']) {
    headRow.append(el('th', undefined, title));
  }
  head.append(headRow);

  const body = el('tbody');
  for (const row of detailRows(overview.daily ?? [], DETAIL_DAYS)) {
    const tr = el('tr');
    tr.append(el('td', 'mono', formatDayKey(row.dayKey)));
    tr.append(el('td', 'mono strong', formatNumber(totalOf(row))));
    for (const value of [row.cjk, row.latin, row.digit, row.punct, row.other]) {
      tr.append(el('td', 'mono dim', formatNumber(value)));
    }
    tr.append(el('td', undefined, formatDuration(row.activeMs)));
    tr.append(el('td', undefined, formatSpeed(speedOf(row))));
    body.append(tr);
  }
  table.append(head, body);
  container.replaceChildren(table);
}

// ---------------------------------------------------------------- 请求与反馈

function showStatus(message: string, ok: boolean): void {
  const status = byId('statsStatus');
  if (!status) {
    return;
  }
  status.textContent = message;
  status.classList.toggle('is-error', !ok);
  status.hidden = false;
  if (statusTimer !== null) {
    window.clearTimeout(statusTimer);
    statusTimer = null;
  }
  if (ok) {
    statusTimer = window.setTimeout(() => {
      status.hidden = true;
      statusTimer = null;
    }, STATUS_TIMEOUT_MS);
  }
}

function setClearStatus(message: string, isError: boolean): void {
  const status = byId('statsClearStatus');
  if (!status) {
    return;
  }
  status.textContent = message;
  status.classList.toggle('is-error', isError);
}

function post(action: StatsRequest['action']): void {
  const requestId = `stats-${++requestCounter}`;
  if (action === 'overview') {
    // 所有概览查询（含 setup 的首次请求、清空后的回填）都记时间，
    // refreshOverview 的节流才不会在刚查完之后又补一次。
    lastOverviewRequestAt = Date.now();
    // 在途的旧概览请求丢了响应就再也不会销账，这里顺手清掉，免得 map 越积越长；
    // 迟到的响应会因为查不到 action 被 handleResponse 忽略。
    for (const [id, pending] of pendingRequests) {
      if (pending === 'overview') {
        pendingRequests.delete(id);
      }
    }
  }
  pendingRequests.set(requestId, action);
  window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'statsRequest', data: { requestId, action } }));
}

function handleResponse(response: StatsResponse, action: StatsRequest['action']): void {
  if (!response.ok) {
    if (action === 'clearAll') {
      setClearStatus(response.message || '清空统计数据失败', true);
    } else {
      showStatus(response.message || '统计操作失败', false);
    }
    return;
  }
  if (response.overview) {
    renderOverview(response.overview);
  }
  if (action === 'openDirectory') {
    showStatus(response.message || '已打开数据目录', true);
  } else if (action === 'clearAll') {
    const removed = response.removedDays ?? 0;
    setClearStatus(response.message || (removed > 0 ? `已删除 ${removed} 天的记录` : '没有需要清理的数据'), false);
  }
}

/** 「清空统计数据」：破坏性操作，确认后才发请求；响应里的空态概览直接刷新页面。 */
function onClearAll(): void {
  if (!window.confirm('确定要清空全部统计数据吗？今天与历史的记录都会被删除，删除后无法恢复。')) {
    return;
  }
  setClearStatus('正在清空统计数据…', false);
  post('clearAll');
}

/** 「自动清理」下拉：选中即保存到 config.toml，Server 下次写库时按新策略执行。 */
function onRetentionChange(): void {
  const select = byId<HTMLSelectElement>('statsRetention');
  if (!select) {
    return;
  }
  const retention = retentionValueOf(select.value);
  if (!retention) {
    return;
  }
  updateConfig(STATISTICS_RETENTION_PATH, retention);
  if (retention === 'forever') {
    setClearStatus('已保存：永久保留，不会自动删除任何记录。', false);
    return;
  }
  const label = select.selectedOptions[0]?.textContent ?? retention;
  setClearStatus(`已保存自动清理策略：${label}；Server 跨天后首次写入统计时执行清理。`, false);
}

function setupCalendarResize(): void {
  const box = byId('statsCalendar');
  const root = byId('stats-settings');
  if (!box || !root || typeof ResizeObserver === 'undefined') {
    return;
  }
  const applyMetrics = () => {
    const width = box.clientWidth;
    const nextWeeks = computeCalendarWeeks(width);
    // 起点向前对齐到周一后，实际渲染列数在「今天不是周日」时会比 weeks 多一列；
    // 按 weeks + 1 计算格子尺寸才能保证热图不出现横向滚动条。
    const nextCell = computeCalendarCellSize(width, nextWeeks + 1);
    if (nextWeeks === calendarWeeks && nextCell === calendarCellPx) {
      return;
    }
    const previousWeeks = calendarWeeks;
    calendarWeeks = nextWeeks;
    calendarCellPx = nextCell;
    root.style.setProperty('--stats-cal-cell', `${nextCell}px`);
    if (!lastOverview) {
      return;
    }
    // 新列在左侧追加，按宽度差平移滚动位置，保持用户正在看的日期不跳。
    const previousScroll = box.scrollLeft;
    renderCalendar(lastOverview);
    box.scrollLeft = Math.max(0, previousScroll + (nextWeeks - previousWeeks) * CALENDAR_COLUMN_PX);
  };
  calendarObserver?.disconnect();
  calendarObserver = new ResizeObserver(applyMetrics);
  calendarObserver.observe(box);
  applyMetrics();
}

/** 统计页当前是不是真的在用户眼前：窗口没被隐藏，且侧栏停在统计模块上。 */
function isStatsPageVisible(): boolean {
  if (document.visibilityState !== 'visible') {
    return false;
  }
  const panel = byId('stats');
  // offsetParent 为空说明自己或祖先 display:none，覆盖 showOnlyCurrentModule
  // 之外的隐藏路径；空面板（partial 还没插入）也会被挡掉。
  return !!panel && panel.offsetParent !== null;
}

/** 同一时刻只留一个在途概览查询，Server 慢时不至于堆成一串。 */
function hasPendingOverview(): boolean {
  // 响应只在 statsResponse 里销账：万一丢了一条，不能让刷新从此永久停摆，
  // 超过这个时间的在途请求一律当作没有。
  if (Date.now() - lastOverviewRequestAt > OVERVIEW_STALE_MS) {
    return false;
  }
  for (const action of pendingRequests.values()) {
    if (action === 'overview') {
      return true;
    }
  }
  return false;
}

function refreshOverview(): void {
  if (!isStatsPageVisible() || hasPendingOverview()) {
    return;
  }
  if (Date.now() - lastOverviewRequestAt < OVERVIEW_MIN_INTERVAL_MS) {
    return;
  }
  post('overview');
}

/**
 * 统计不是推送模型（数据由 Server 进程写库，设置窗口是另一个进程，自己开库查），
 * 所以「什么时候补一次查询」全靠这里的几个触发源：
 *   - 侧栏切走再切回：模块不会重跑 setup，只靠 IntersectionObserver 的显隐翻转；
 *   - 设置窗口关掉再打开：只是隐藏 + controller.put_IsVisible(false)，走 visibilitychange；
 *   - 窗口一直开着、用户切到别的程序打字再点回来：上面两个都不会触发，只有 focus；
 *   - 人就盯着统计页、在旁边的窗口里打字：连 focus 都没有，靠可见时的轮询兜底。
 * 都汇到 refreshOverview，由它做节流和在途去重，避免几个触发源叠在一起连发。
 */
function setupOverviewRefresh(): void {
  const root = byId('stats-settings');
  if (root && typeof IntersectionObserver !== 'undefined') {
    let visible = false;
    new IntersectionObserver((entries) => {
      const nowVisible = entries.some((entry) => entry.isIntersecting);
      // 首次显示紧跟 setupStats 的初始请求，被 refreshOverview 的节流挡掉。
      if (nowVisible && !visible) {
        refreshOverview();
      }
      visible = nowVisible;
    }).observe(root);
  }
  document.addEventListener('visibilitychange', () => {
    refreshOverview();
  });
  window.addEventListener('focus', () => {
    refreshOverview();
  });
  if (overviewPollTimer !== null) {
    window.clearInterval(overviewPollTimer);
  }
  // 常驻定时器：不可见时 refreshOverview 直接返回，窗口隐藏后浏览器还会自己降频，
  // 比按显隐反复建销定时器少一份状态。
  overviewPollTimer = window.setInterval(refreshOverview, OVERVIEW_POLL_MS);
}

/** 配置快照回填：statistics.enabled 的 boolean 守卫在 config-sync 里，这里只应用。 */
export function applyStatisticsEnabled(enabled: boolean): void {
  statisticsEnabled = enabled;
  applyToggleState('statisticsEnabledToggleBtn', enabled);
  renderEmptyState();
}

/** 配置快照回填：只接受契约枚举内的保留策略，非法值不改动下拉。 */
export function applyStatisticsRetention(value: string): void {
  const retention = retentionValueOf(value);
  const select = byId<HTMLSelectElement>('statsRetention');
  if (select && retention) {
    select.value = retention;
  }
}

export function setupStats(): void {
  setupToggleButton('statisticsEnabledToggleBtn', (active) => {
    statisticsEnabled = active;
    updateConfig(STATISTICS_ENABLED_PATH, active);
    renderEmptyState();
  });
  byId('statsEnableButton')?.addEventListener('click', () => {
    applyStatisticsEnabled(true);
    updateConfig(STATISTICS_ENABLED_PATH, true);
  });
  byId('statsRetention')?.addEventListener('change', onRetentionChange);
  byId('statsClearButton')?.addEventListener('click', onClearAll);
  byId('statsOpenDirectoryButton')?.addEventListener('click', () => post('openDirectory'));

  setupCalendarResize();
  setupOverviewRefresh();

  onHostMessage('statsResponse', (payload) => {
    const action = pendingRequests.get(payload.requestId);
    if (!action) {
      return;
    }
    pendingRequests.delete(payload.requestId);
    handleResponse(payload, action);
  });

  post('overview');
}
