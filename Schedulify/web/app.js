/* Schedulify dashboard — presentation only.
 *
 * READ THIS BEFORE EDITING
 * ------------------------
 * This file contains no scheduling logic and computes no metric. It:
 *   1. collects the workload from the form,
 *   2. POSTs it to the C engine (/api/simulate, /api/compare),
 *   3. animates the timeline the engine sends back.
 *
 * Every number rendered here — completion, turnaround, waiting, response,
 * utilisation, throughput, context switches, preemptions and the averages —
 * arrives already computed in the JSON payload. The only arithmetic below is
 * layout (time -> percentage of chart width) and reading the C timeline to
 * ask "what is on the CPU at t?", which is presentation, not scheduling.
 *
 * If you are tempted to work out a schedule here: don't. Add it to
 * src/sched_*.c and let metrics.c derive the numbers from the timeline.
 *
 * The same goes for the predictive analysis: the arrival rate, the Poisson
 * probabilities, the predicted processes, the stability checks and the
 * original-vs-forecast comparison all come from src/predict.c and
 * src/webapp.c. This file only lays them out.
 */
'use strict';

/* ───────────────────────────── helpers ───────────────────────────── */

const $  = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
};

/* The stylesheet staggers entrance animations off --i; it clamps the
 * value itself, so this only has to hand over the index. */
const stagger = (node, i) => { node.style.setProperty('--i', String(i)); return node; };

const fmt = (v, dp = 2) =>
  (typeof v === 'number' && isFinite(v)) ? v.toFixed(dp) : '—';

/* The animation clock is fractional, so remaining-time readouts would show
 * float noise (5.534000000000001). Round to 2dp and drop a trailing .00. */
const num = (v) => {
  if (typeof v !== 'number' || !isFinite(v)) return '—';
  const r = Math.round(v * 100) / 100;
  return Number.isInteger(r) ? String(r) : r.toFixed(2);
};

/* ───────────────────────────── state ───────────────────────────── */

const state = {
  algorithms: [],
  examples: [],
  limits: { minArrival: 0, maxArrival: 100000, minBurst: 1, maxBurst: 10000,
            minPriority: 0, maxPriority: 99, minQuantum: 1, maxQuantum: 1000,
            maxProcesses: 512 },
  algo: 'srtf',
  quantum: 2,
  rows: [],            // { id, arrival, burst, priority }
  result: null,        // the schedule on screen, verbatim from the engine
  resultKind: 'plain', // 'plain' | 'original' | 'augmented'
  forecast: null,      // { data, basis } -- /api/forecast reply + workload it came from
  forecastSim: null,   // /api/forecast/simulate reply
  clock: 0,
  playing: false,
  speed: 4,
  lastFrame: 0,
  rafId: 0
};

let nextRowSeq = 1;

/* ───────────────────────────── API ───────────────────────────── */

async function api(path, body) {
  const opts = body === undefined
    ? { method: 'GET' }
    : { method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body) };

  const res = await fetch(path, opts);
  let data = null;
  try { data = await res.json(); }
  catch (_) { throw new Error(`the engine returned a non-JSON reply (HTTP ${res.status})`); }
  if (!res.ok) throw new Error(data && data.error ? data.error : `HTTP ${res.status}`);
  return data;
}

/* ───────────────────────────── process editor ───────────────────── */

function addRow(proc) {
  const p = proc || { id: `P${nextRowSeq}`, arrival: 0, burst: 4, priority: 0 };
  state.rows.push({ key: nextRowSeq++, ...p });
  renderRows();
}

function renderRows() {
  const host = $('#proc-rows');
  host.textContent = '';
  const L = state.limits;

  state.rows.forEach((row, i) => {
    const r = el('div', 'proc-row');

    const mk = (type, key, min, max) => {
      const inp = el('input');
      inp.type = type;
      inp.value = row[key];
      if (min !== undefined) inp.min = min;
      if (max !== undefined) inp.max = max;
      inp.setAttribute('aria-label', `${key} of process ${row.id}`);
      inp.addEventListener('input', () => {
        row[key] = type === 'number' ? inp.valueAsNumber : inp.value;
        const bad = type === 'number'
          ? !Number.isInteger(row[key]) || row[key] < min || row[key] > max
          : String(row[key]).trim() === '';
        inp.classList.toggle('bad', bad);
        if (key === 'id') $('#proc-count').textContent = String(state.rows.length);
        syncForecastStale();
      });
      return inp;
    };

    r.append(mk('text', 'id'));
    r.append(mk('number', 'arrival', L.minArrival, L.maxArrival));
    r.append(mk('number', 'burst', L.minBurst, L.maxBurst));
    r.append(mk('number', 'priority', L.minPriority, L.maxPriority));

    const del = el('button', 'row-del', '×');
    del.type = 'button';
    del.title = `Remove ${row.id}`;
    del.setAttribute('aria-label', `Remove process ${row.id}`);
    del.disabled = state.rows.length <= 1;
    del.addEventListener('click', () => {
      state.rows.splice(i, 1);
      renderRows();
    });
    r.append(del);

    host.append(r);
  });

  $('#proc-count').textContent = String(state.rows.length);
  syncForecastStale();
}

function setRows(procs) {
  state.rows = procs.map((p) => ({ key: nextRowSeq++, id: p.id,
                                   arrival: p.arrival, burst: p.burst,
                                   priority: p.priority || 0 }));
  renderRows();
}

/* ───────────────────────────── algorithm picker ─────────────────── */

function renderAlgorithms() {
  const host = $('#algo-list');
  host.textContent = '';

  state.algorithms.forEach((a, i) => {
    const b = el('button', 'algo');
    stagger(b, i);
    b.type = 'button';
    b.setAttribute('role', 'radio');
    b.setAttribute('aria-checked', String(a.slug === state.algo));
    b.dataset.slug = a.slug;
    b.append(el('span', 'algo-key', a.key));
    const col = el('span');
    col.append(el('span', 'algo-name', a.name));
    col.append(document.createElement('br'));
    col.append(el('span', 'algo-kind', a.kind));
    b.append(col);
    b.addEventListener('click', () => selectAlgorithm(a.slug));
    host.append(b);
  });
  syncAlgorithmUi();
}

function selectAlgorithm(slug) {
  state.algo = slug;
  $$('.algo').forEach((b) =>
    b.setAttribute('aria-checked', String(b.dataset.slug === slug)));
  syncAlgorithmUi();
}

function currentAlgorithm() {
  return state.algorithms.find((a) => a.slug === state.algo) || null;
}

function syncAlgorithmUi() {
  const a = currentAlgorithm();
  $('#algo-rule').textContent = a ? a.rule : '';
  $('#quantum-block').hidden = !(a && a.needsQuantum);
}

/* ───────────────────────────── run ───────────────────────────── */

function workloadPayload() {
  return {
    algorithm: state.algo,
    quantum: state.quantum,
    processes: state.rows.map((r) => ({
      id: String(r.id).trim(),
      arrival: Number(r.arrival),
      burst: Number(r.burst),
      priority: Number(r.priority)
    }))
  };
}

function showError(msg) {
  const box = $('#error');
  if (!msg) { box.hidden = true; box.textContent = ''; return; }
  box.hidden = false;
  box.textContent = msg;
}

/* Puts one engine schedule into the main timeline and plays it. kind says
 * which workload it is, so the chart can label a forecast-augmented run. */
function showSchedule(data, kind) {
  state.result = data;
  state.resultKind = kind;
  $('#empty-state').hidden = true;
  $('#result-view').hidden = false;
  buildChart();
  stopPlayback();
  setClock(data.window.start);
  startPlayback();
  syncViewToggle();
}

async function run() {
  showError(null);
  $('#run').disabled = true;
  try {
    const data = await api('/api/simulate', workloadPayload());
    $('#compare-view').hidden = true;
    showSchedule(data, 'plain');
  } catch (e) {
    showError(e.message);
  } finally {
    $('#run').disabled = false;
  }
}

async function compareAll() {
  showError(null);
  $('#compare').disabled = true;
  try {
    const payload = workloadPayload();
    delete payload.algorithm;                 // compare runs all six
    const data = await api('/api/compare', payload);
    renderComparison(data);
    $('#empty-state').hidden = true;
    $('#compare-view').hidden = false;
    $('#compare-view').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (e) {
    showError(e.message);
  } finally {
    $('#compare').disabled = false;
  }
}

/* ───────────────────────────── chart building ───────────────────── */

/* Time -> percentage across the chart. */
function pct(t) {
  const w = state.result.window;
  const span = Math.max(1, w.end - w.start);
  return ((t - w.start) / span) * 100;
}

function buildChart() {
  const r = state.result;
  const a = r.algorithm;

  const nPred = r.processes.filter((p) => p.predicted).length;
  const which = state.resultKind === 'augmented' ? ' · with forecast'
              : state.resultKind === 'original' ? ' · original workload' : '';
  $('#gantt-title').textContent = `${a.key} — ${a.name}${which}`;
  $('#gantt-sub').textContent =
    (a.needsQuantum ? `${a.kind}, quantum ${a.quantum}` : a.kind)
    + ` · ${r.processes.length} processes`
    + (nPred ? ` (${nPred} predicted, dashed outline)` : '')
    + ` · t=${r.window.start} to t=${r.window.end}`;

  $('#clock-end').textContent = `/ ${r.window.end}`;

  /* One grid rule per unit of time, inherited by the Gantt and the
   * swimlanes alike. Layout arithmetic over the window the engine
   * reported -- it decides nothing about the schedule. */
  const span = r.window.end - r.window.start;
  $('.chart').style.setProperty('--unit', span > 0 ? `${100 / span}%` : '100%');

  buildGantt();
  buildLanes();
  buildRuler();
  buildTiles();
  buildTable();
  buildTrace();
}

function segTooltip(s) {
  const dur = s.end - s.start;
  return s.idle
    ? `<b>CPU idle</b><br>t=${s.start} to t=${s.end} (${dur} unit${dur === 1 ? '' : 's'})<br>nothing was ready to run`
    : `<b>${s.id}</b> on the CPU${isPredicted(s.id) ? ' (predicted process)' : ''}`
      + `<br>t=${s.start} to t=${s.end} (${dur} unit${dur === 1 ? '' : 's'})`;
}

/* Whether the schedule on screen marks this process as forecast-generated
 * (a flag the engine sets; nothing is inferred from the id). */
function isPredicted(id) {
  const r = state.result;
  const p = r && r.processes.find((x) => x.id === id);
  return Boolean(p && p.predicted);
}

function attachTooltip(node, html) {
  const tip = $('#tooltip');
  node.addEventListener('mouseenter', () => {
    tip.innerHTML = html;
    tip.hidden = false;
  });
  node.addEventListener('mousemove', (ev) => {
    const pad = 14;
    let x = ev.clientX + pad;
    let y = ev.clientY + pad;
    const b = tip.getBoundingClientRect();
    if (x + b.width > window.innerWidth - 8) x = ev.clientX - b.width - pad;
    if (y + b.height > window.innerHeight - 8) y = ev.clientY - b.height - pad;
    tip.style.left = `${x}px`;
    tip.style.top = `${y}px`;
  });
  node.addEventListener('mouseleave', () => { tip.hidden = true; });
}

function buildGantt() {
  const host = $('#gantt');
  host.textContent = '';
  const r = state.result;

  r.segments.forEach((s, i) => {
    const node = stagger(el('div', 'seg' + (s.idle ? ' idle' : '')
                                   + (isPredicted(s.id) ? ' predicted' : '')), i);
    node.style.left = `${pct(s.start)}%`;
    node.style.width = `${pct(s.end) - pct(s.start)}%`;
    node.dataset.seg = String(i);

    node.append(el('div', 'seg-track'));
    node.append(el('div', 'seg-fill'));
    /* The direct label is what carries process identity — never colour. */
    node.append(el('div', 'seg-label', s.idle ? 'IDLE' : s.id));

    attachTooltip(node, segTooltip(s));
    host.append(node);
  });

  /* preemption markers: a process leaving the CPU with work left.
   * Derived from the C timeline + the C-computed burst, not recomputed. */
  const executed = {};
  r.segments.forEach((s, i) => {
    if (s.idle) return;
    executed[s.id] = (executed[s.id] || 0) + (s.end - s.start);
    const proc = r.processes.find((p) => p.id === s.id);
    const next = r.segments.slice(i + 1).find((x) => !x.idle);
    if (proc && next && next.id !== s.id && executed[s.id] < proc.burst) {
      const m = stagger(el('div', 'marker'), i);
      m.style.left = `${pct(s.end)}%`;
      m.title = `${s.id} preempted at t=${s.end} with ${proc.burst - executed[s.id]} unit(s) left`;
      host.append(m);
    }
  });

  const ph = el('div', 'playhead');
  ph.id = 'playhead';
  host.append(ph);
}

function buildLanes() {
  const host = $('#lanes');
  host.textContent = '';
  const r = state.result;

  r.processes.forEach((p, i) => {
    const lane = stagger(el('div', 'lane' + (p.predicted ? ' predicted' : '')), i);
    const name = el('div', 'lane-name');
    name.append(el('span', null, p.id));
    name.append(document.createElement('br'));
    /* on its own line: the name column is narrow, and a tag beside the id
     * would push the id itself into the ellipsis */
    if (p.predicted) {
      name.append(el('span', 'tag tag-lane', 'forecast'));
      name.append(document.createElement('br'));
    }
    name.append(el('span', 'lane-meta', `a${p.arrival} · b${p.burst} · p${p.priority}`));
    lane.append(name);

    const track = el('div', 'lane-track');
    track.dataset.pid = p.id;

    const arr = el('div', 'lane-arrival');
    arr.style.left = `${pct(p.arrival)}%`;
    arr.title = `${p.id} arrives at t=${p.arrival}`;
    track.append(arr);

    r.segments.filter((s) => !s.idle && s.id === p.id).forEach((s) => {
      const seg = el('div', 'lane-seg');
      seg.style.left = `${pct(s.start)}%`;
      seg.style.width = `${pct(s.end) - pct(s.start)}%`;
      seg.dataset.start = String(s.start);
      seg.dataset.end = String(s.end);
      attachTooltip(seg, segTooltip(s));
      track.append(seg);
    });

    const lph = el('div', 'lane-playhead');
    track.append(lph);

    lane.append(track);
    host.append(lane);
  });
}

function buildRuler() {
  const host = $('#ruler');
  host.textContent = '';
  const r = state.result;

  const times = [...new Set(
    r.segments.flatMap((s) => [s.start, s.end])
  )].sort((a, b) => a - b);

  /* Drop labels that would collide, keeping the first and last. */
  const widthPx = host.clientWidth || 800;
  const minGap = 26;
  let lastX = -Infinity;

  times.forEach((t, i) => {
    const x = (pct(t) / 100) * widthPx;
    const isEdge = i === 0 || i === times.length - 1;
    if (!isEdge && x - lastX < minGap) return;
    lastX = x;
    const tick = stagger(el('div', 'tick', String(t)), i);
    tick.style.left = `${pct(t)}%`;
    host.append(tick);
  });
}

function buildTiles() {
  const host = $('#tiles');
  host.textContent = '';
  const m = state.result.metrics;

  const tiles = [
    ['Avg turnaround', fmt(m.avgTurnaround), '', 'completion − arrival'],
    ['Avg waiting',    fmt(m.avgWaiting),    '', 'turnaround − burst'],
    ['Avg response',   fmt(m.avgResponse),   '', 'first CPU − arrival'],
    ['CPU utilisation', fmt(m.cpuUtilization), '%', `${m.busyTime} busy / ${m.totalTime} elapsed`],
    ['Throughput',     fmt(m.throughput, 4), '/unit', `${m.processes} processes`],
    ['Context switches', String(m.contextSwitches), '', 'hand-overs between processes'],
    ['Preemptions',    String(m.preemptions), '', 'left the CPU with work left'],
    ['CPU idle',       String(m.idleTime), 'units', 'nothing was ready'],
    ['Makespan',       String(m.totalTime), 'units', `t=${state.result.window.start} → ${state.result.window.end}`]
  ];
  renderTiles(host, tiles);
}

/* tiles: [label, value, unit, note] -- shared by the metric and forecast panels */
function renderTiles(host, tiles) {
  host.textContent = '';
  tiles.forEach(([label, value, unit, note], i) => {
    const t = stagger(el('div', 'tile'), i);
    t.append(el('div', 'tile-label', label));
    const v = el('div', 'tile-value', value);
    if (unit) v.append(el('span', 'tile-unit', unit));
    t.append(v);
    t.append(el('div', 'tile-note', note));
    host.append(t);
  });
}

function buildTable() {
  const tb = $('#proc-table tbody');
  const tf = $('#proc-table tfoot');
  tb.textContent = '';
  tf.textContent = '';
  const r = state.result;

  r.processes.forEach((p, i) => {
    const tr = stagger(el('tr'), i);
    tr.dataset.pid = p.id;
    if (p.predicted) tr.classList.add('row-predicted');
    [p.id, p.arrival, p.burst, p.priority, p.start, p.completion,
     p.turnaround, p.waiting, p.response].forEach((v, c) => {
      const td = el('td', null, String(v));
      if (c === 0 && p.predicted) td.append(el('span', 'tag', 'forecast'));
      tr.append(td);
    });
    tb.append(tr);
  });

  const m = r.metrics;
  const tr = el('tr');
  ['Average', '—', '—', '—', '—', fmt(m.avgCompletion), fmt(m.avgTurnaround),
   fmt(m.avgWaiting), fmt(m.avgResponse)].forEach((v) => {
    tr.append(el('td', null, String(v)));
  });
  tf.append(tr);
}

function buildTrace() {
  const host = $('#trace');
  host.textContent = '';
  state.result.trace.forEach((ev, i) => {
    const li = stagger(el('li'), i);
    li.dataset.time = String(ev.time);
    li.dataset.i = String(i);
    li.append(el('span', 't', `t=${ev.time}`));
    li.append(el('span', null, ev.text));
    host.append(li);
  });
}

/* ───────────────────────────── animation ───────────────────────── */

/* Which segment covers t (reading the C timeline). */
function segmentAt(t) {
  const r = state.result;
  for (const s of r.segments) {
    if (t >= s.start && t < s.end) return s;
  }
  /* exactly at the end: report the final segment */
  return r.segments.length ? r.segments[r.segments.length - 1] : null;
}

/* CPU time each process has received strictly before t. */
function executedBefore(t) {
  const out = {};
  state.result.processes.forEach((p) => { out[p.id] = 0; });
  for (const s of state.result.segments) {
    if (s.idle) continue;
    if (s.start >= t) break;
    out[s.id] += Math.min(s.end, t) - s.start;
  }
  return out;
}

function setClock(t) {
  const w = state.result.window;
  state.clock = Math.max(w.start, Math.min(w.end, t));
  paintClock();
}

function paintClock() {
  const r = state.result;
  const t = state.clock;
  const shown = Math.round(t * 100) / 100;

  $('#clock-out').textContent = Number.isInteger(shown) ? String(shown) : shown.toFixed(2);
  $('#live-t').textContent = String(Math.floor(t));
  $('#scrub').value = String(pct(t));

  const ph = document.getElementById('playhead');
  if (ph) ph.style.left = `${pct(t)}%`;

  /* reveal each Gantt cell up to the clock */
  $$('#gantt .seg').forEach((node) => {
    const s = r.segments[Number(node.dataset.seg)];
    const dur = s.end - s.start;
    const vis = Math.max(0, Math.min(dur, t - s.start));
    const fill = node.querySelector('.seg-fill');
    if (fill) fill.style.width = `${(vis / dur) * 100}%`;
    node.classList.toggle('running', !s.idle && t >= s.start && t < s.end);
  });

  /* swimlanes: pending until reached, filled once executed */
  $$('#lanes .lane-seg').forEach((seg) => {
    const start = Number(seg.dataset.start);
    const end = Number(seg.dataset.end);
    seg.classList.toggle('pending', t <= start);
    seg.classList.toggle('running', t >= start && t < end);
  });
  $$('#lanes .lane-playhead').forEach((p) => { p.style.left = `${pct(t)}%`; });

  const cur = segmentAt(t);
  $$('#lanes .lane-track').forEach((tr) => {
    tr.classList.toggle('is-running',
      Boolean(cur) && !cur.idle && tr.dataset.pid === cur.id && t < r.window.end);
  });

  paintLive(t, cur);
  paintTrace(t);
}

function paintLive(t, cur) {
  const r = state.result;
  const exec = executedBefore(t);
  const atEnd = t >= r.window.end;

  const cpu = $('#cpu-now');
  if (atEnd) {
    cpu.className = 'cpu-now idle';
    cpu.textContent = 'all processes complete';
  } else if (!cur || cur.idle) {
    cpu.className = 'cpu-now idle';
    cpu.textContent = 'idle — nothing is ready to run';
  } else {
    const p = r.processes.find((x) => x.id === cur.id);
    const left = p ? p.burst - exec[cur.id] : 0;
    cpu.className = 'cpu-now';
    cpu.textContent = `${cur.id} running — ${num(left)} unit${left === 1 ? '' : 's'} left`;
  }

  const waiting = r.processes.filter((p) =>
    p.arrival <= t && exec[p.id] < p.burst && !(cur && !cur.idle && cur.id === p.id));
  renderChips($('#ready-queue'), waiting.map((p) => ({
    cls: 'chip',
    id: p.id,
    rem: ` ${num(p.burst - exec[p.id])} left`
  })), atEnd ? '—' : 'nobody waiting');

  const finished = r.processes.filter((p) => exec[p.id] >= p.burst);
  renderChips($('#done-list'), finished.map((p) => ({
    cls: 'chip done',
    id: `${p.id} @ ${p.completion}`
  })), 'none yet');

  $$('#proc-table tbody tr').forEach((tr) => {
    const pid = tr.dataset.pid;
    const p = r.processes.find((x) => x.id === pid);
    tr.classList.toggle('row-running',
      Boolean(cur) && !cur.idle && cur.id === pid && !atEnd);
    tr.classList.toggle('row-done', Boolean(p) && exec[pid] >= p.burst);
  });
}

/* Chips are rebuilt only when the set they show actually changes.
 *
 * paintLive runs once per animation frame, and a chip's entrance animation
 * restarts whenever its element is replaced -- so rebuilding every frame
 * would leave every chip frozen on the first keyframe. Comparing a
 * signature first means a chip animates in exactly once, when the process
 * it stands for really does enter the queue or finish. */
function renderChips(host, items, emptyText) {
  const sig = items.map((c) => `${c.cls}|${c.id}|${c.rem || ''}`).join('\u0001');
  if (host.dataset.sig === sig) return;
  host.dataset.sig = sig;

  host.textContent = '';
  if (!items.length) {
    host.append(el('span', 'chips-empty', emptyText));
    return;
  }
  items.forEach((c, i) => {
    const node = stagger(el('span', c.cls), i);
    node.append(el('span', null, c.id));
    if (c.rem) node.append(el('span', 'chip-rem', c.rem));
    host.append(node);
  });
}

function paintTrace(t) {
  const items = $$('#trace li');
  let currentIdx = -1;
  items.forEach((li, i) => {
    const seen = Number(li.dataset.time) <= t;
    li.classList.toggle('seen', seen);
    li.classList.remove('current');
    if (seen) currentIdx = i;
  });
  if (currentIdx >= 0) {
    const li = items[currentIdx];
    li.classList.add('current');
    const box = $('#trace');
    const top = li.offsetTop - box.offsetTop;
    if (top < box.scrollTop || top > box.scrollTop + box.clientHeight - 40) {
      box.scrollTop = top - box.clientHeight / 2;
    }
  }
}

function frame(ts) {
  if (!state.playing) return;
  if (!state.lastFrame) state.lastFrame = ts;
  const dt = (ts - state.lastFrame) / 1000;
  state.lastFrame = ts;

  setClock(state.clock + dt * state.speed);

  if (state.clock >= state.result.window.end) {
    stopPlayback();
    return;
  }
  state.rafId = requestAnimationFrame(frame);
}

function startPlayback() {
  if (!state.result) return;
  if (state.clock >= state.result.window.end) state.clock = state.result.window.start;
  state.playing = true;
  state.lastFrame = 0;
  document.body.classList.add('is-playing');
  $('#play-icon').textContent = '⏸';
  $('#play-text').textContent = 'Pause';
  $('#play').setAttribute('aria-label', 'Pause');
  state.rafId = requestAnimationFrame(frame);
}

function stopPlayback() {
  state.playing = false;
  if (state.rafId) cancelAnimationFrame(state.rafId);
  state.rafId = 0;
  document.body.classList.remove('is-playing');
  $('#play-icon').textContent = '▶';
  $('#play-text').textContent = 'Play';
  $('#play').setAttribute('aria-label', 'Play');
}

function togglePlay() {
  if (state.playing) stopPlayback();
  else startPlayback();
}

/* Jump to the next / previous event boundary. */
function stepTo(dir) {
  if (!state.result) return;
  stopPlayback();
  const times = [...new Set(state.result.segments.flatMap((s) => [s.start, s.end]))]
    .sort((a, b) => a - b);
  const t = state.clock;
  if (dir > 0) {
    const next = times.find((x) => x > t + 1e-9);
    setClock(next === undefined ? state.result.window.end : next);
  } else {
    const prev = [...times].reverse().find((x) => x < t - 1e-9);
    setClock(prev === undefined ? state.result.window.start : prev);
  }
}

/* ───────────────────────────── comparison ───────────────────────── */

function renderComparison(data) {
  const tb = $('#cmp-table tbody');
  tb.textContent = '';

  const ok = data.results.filter((r) => !r.error);

  /* Best value per column — comparing numbers the engine already computed. */
  const best = (pick, dir) => {
    const vals = ok.map((r) => pick(r.metrics));
    if (!vals.length) return null;
    return dir < 0 ? Math.min(...vals) : Math.max(...vals);
  };
  const cols = [
    { pick: (m) => m.avgTurnaround,  dir: -1, dp: 2 },
    { pick: (m) => m.avgWaiting,     dir: -1, dp: 2 },
    { pick: (m) => m.avgResponse,    dir: -1, dp: 2 },
    { pick: (m) => m.cpuUtilization, dir: +1, dp: 2 },
    { pick: (m) => m.throughput,     dir: +1, dp: 4 },
    { pick: (m) => m.contextSwitches, dir: -1, dp: 0 },
    { pick: (m) => m.preemptions,    dir: -1, dp: 0 },
    { pick: (m) => m.totalTime,      dir: -1, dp: 0 }
  ];
  const bests = cols.map((c) => best(c.pick, c.dir));

  data.results.forEach((r) => {
    const tr = el('tr');
    const label = r.algorithm.needsQuantum
      ? `${r.algorithm.key} (q=${r.algorithm.quantum})`
      : r.algorithm.key;
    tr.append(el('td', null, label));

    if (r.error) {
      const td = el('td', null, r.error);
      td.colSpan = cols.length;
      tr.append(td);
      tb.append(tr);
      return;
    }

    cols.forEach((c, i) => {
      const v = c.pick(r.metrics);
      const td = el('td', null, c.dp ? fmt(v, c.dp) : String(v));
      if (bests[i] !== null && Math.abs(v - bests[i]) < 1e-9) {
        td.classList.add('best');
        td.append(el('span', 'star', '★'));
      }
      tr.append(td);
    });
    tb.append(tr);
  });

  /* A compact timeline strip per algorithm, all on one shared scale. */
  const charts = $('#cmp-charts');
  charts.textContent = '';
  const end = Math.max(...ok.map((r) => r.window.end), 1);
  const start = Math.min(...ok.map((r) => r.window.start), 0);
  const span = Math.max(1, end - start);
  /* every strip shares one scale, so one grid spacing serves them all */
  charts.style.setProperty('--unit', `${100 / span}%`);

  data.results.forEach((r, i) => {
    if (r.error) return;
    const row = stagger(el('div', 'cmp-row'), i);
    const label = r.algorithm.needsQuantum
      ? `${r.algorithm.key} q=${r.algorithm.quantum}`
      : r.algorithm.key;
    row.append(el('div', 'cmp-label', label));

    const strip = el('div', 'cmp-strip');
    r.segments.forEach((s) => {
      const seg = el('div', 'cmp-seg' + (s.idle ? ' idle' : ''));
      seg.style.left = `${((s.start - start) / span) * 100}%`;
      seg.style.width = `${((s.end - s.start) / span) * 100}%`;
      if (!s.idle) seg.append(el('span', null, s.id));
      attachTooltip(seg, segTooltip(s));
      strip.append(seg);
    });
    row.append(strip);
    charts.append(row);
  });
}

/* ───────────────────────────── predictive analysis ──────────────── */

/* Every statistic below arrives computed from /api/forecast; these only
 * format it. */
const esc = (s) => String(s).replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const prob = (p) => {
  if (typeof p !== 'number' || !isFinite(p)) return '—';
  if (p === 0 || p === 1) return `${p * 100}%`;
  if (p < 0.001) return '< 0.1%';
  if (p > 0.999) return '> 99.9%';
  return `${(p * 100).toFixed(1)}%`;
};

const pval = (p) => (typeof p !== 'number' || !isFinite(p)) ? '—'
  : p < 0.001 ? '< 0.001' : p.toFixed(3);

const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;

function showForecastError(msg) {
  const box = $('#fc-error');
  box.hidden = !msg;
  box.textContent = msg || '';
}

/* The forecast belongs to the workload it was generated from. */
function workloadSignature() {
  return JSON.stringify(workloadPayload().processes);
}

function syncForecastStale() {
  const f = state.forecast;
  const stale = Boolean(f) && workloadSignature() !== f.basis;
  $('#fc-stale').hidden = !stale;
  $('#fc-simulate').disabled = !f || stale;
}

function syncViewToggle() {
  $('#fc-show-original').setAttribute('aria-pressed', String(state.resultKind === 'original'));
  $('#fc-show-augmented').setAttribute('aria-pressed', String(state.resultKind === 'augmented'));
}

/* Blank fields are left out so the engine applies its documented defaults;
 * anything typed is sent as-is and validated in C. */
function forecastPayload() {
  const body = { processes: workloadPayload().processes };
  const take = (sel, key) => {
    const raw = $(sel).value.trim();
    if (raw !== '') body[key] = Number(raw);
  };
  take('#fc-horizon', 'horizon');
  if ($('#fc-window-mode').value === 'explicit') {
    take('#fc-obs-start', 'observationStart');
    take('#fc-obs-end', 'observationEnd');
    if (!('observationEnd' in body)) {
      throw new Error('Enter where the observation window ends, or choose "Infer from arrival gaps".');
    }
  }
  take('#fc-cap', 'maxGenerated');
  take('#fc-seed', 'seed');
  return body;
}

async function generateForecast() {
  showForecastError(null);
  $('#fc-generate').disabled = true;
  try {
    const payload = forecastPayload();
    const data = await api('/api/forecast', payload);
    const oldSim = state.forecastSim;

    state.forecast = { data, basis: JSON.stringify(payload.processes) };
    state.forecastSim = null;
    /* a schedule built from the previous forecast no longer matches it */
    if (oldSim && state.resultKind !== 'plain') showSchedule(oldSim.original, 'plain');

    renderForecast();
    $('#forecast-view').hidden = false;
    $('#fc-compare').hidden = true;
    $('#fc-reset').disabled = false;
    syncForecastStale();
    $('#forecast-view').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (e) {
    showForecastError(e.message);
  } finally {
    $('#fc-generate').disabled = false;
  }
}

async function simulateForecast() {
  const f = state.forecast;
  showForecastError(null);
  if (!f) return;
  if (workloadSignature() !== f.basis) {
    showForecastError('The workload has changed since this forecast was generated. Generate it again first.');
    return;
  }
  $('#fc-simulate').disabled = true;
  try {
    const base = workloadPayload();
    const data = await api('/api/forecast/simulate', {
      algorithm: base.algorithm,
      quantum: base.quantum,
      processes: base.processes,
      predicted: f.data.predicted.map(({ id, arrival, burst, priority }) =>
        ({ id, arrival, burst, priority }))
    });
    state.forecastSim = data;
    renderForecastComparison();
    $('#fc-compare').hidden = false;
    $('#compare-view').hidden = true;
    showSchedule(data.augmented, 'augmented');
    $('#result-view').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (e) {
    showForecastError(e.message);
  } finally {
    syncForecastStale();
  }
}

/* Clears the forecast only. The workload in the editor is never touched. */
function resetForecast() {
  const sim = state.forecastSim;
  state.forecast = null;
  state.forecastSim = null;
  $('#forecast-view').hidden = true;
  $('#fc-compare').hidden = true;
  $('#fc-reset').disabled = true;
  showForecastError(null);
  syncForecastStale();
  if (sim && state.resultKind !== 'plain') showSchedule(sim.original, 'plain');
}

function renderForecast() {
  const d = state.forecast.data;
  const o = d.observation;
  const f = d.forecast;
  const b = d.bursts;

  $('#fc-assumption').textContent = o.assumption;

  const wl = $('#fc-warnings');
  wl.textContent = '';
  d.warnings.forEach((w, i) => wl.append(stagger(el('li', null, w), i)));
  wl.hidden = d.warnings.length === 0;

  renderTiles($('#fc-tiles'), [
    ['Arrival rate λ', fmt(d.rate.lambda, 3), '/unit',
     `standard error ≈ ${fmt(d.rate.standardError, 3)}`],
    ['Observation window', String(o.duration), 'units',
     `t=${o.start} → ${o.end}, ${o.mode === 'explicit' ? 'as given' : 'inferred'}`],
    ['Observed', String(o.processes), 'processes',
     o.mode === 'explicit'
       ? `${plural(o.events, 'arrival', 'arrivals')} in λ · ${plural(o.intervals, 'gap', 'gaps')}`
       : `${plural(o.events, 'gap', 'gaps')} between arrivals in λ`],
    ['Expected arrivals', fmt(f.expected, 2), '',
     `E[K] = λ × ${f.horizon}, over t=${f.start} → ${f.end}`],
    ['P(no arrivals)', prob(f.pZero), '', `P(at least one): ${prob(f.pAtLeastOne)}`],
    ['90% of outcomes', `${f.range90.low}–${f.range90.high}`, 'arrivals',
     'central range of the Poisson count'],
    ['This draw', String(f.generated), f.generated === 1 ? 'process' : 'processes',
     `seed ${d.seed}${f.truncated ? ` · stopped at the cap of ${f.cap}` : ''}`],
    ['Burst time', fmt(b.mean, 2), 'mean',
     b.sd === null ? 'sd needs at least 2 samples'
                   : `sd ${fmt(b.sd, 2)} · ${b.samples} samples, ${b.min}–${b.max}`]
  ]);

  renderArrivalChart();
  renderPmf();
  renderDiagnostics();
  renderPredictedTable();
}

/* Nice round tick values across [a, b] for an axis. */
function niceTicks(a, b, want) {
  const span = Math.max(1, b - a);
  const raw = span / want;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = Math.max(1, [1, 2, 5, 10].map((m) => m * mag).find((s) => s >= raw) || mag * 10);
  const out = [];
  for (let t = Math.ceil(a / step) * step; t <= b; t += step) out.push(t);
  return out;
}

function renderArrivalChart() {
  const d = state.forecast.data;
  const o = d.observation;
  const f = d.forecast;
  const observed = JSON.parse(state.forecast.basis);
  const host = $('#fc-arrivals');
  const ruler = $('#fc-arrivals-ruler');
  host.textContent = '';
  ruler.textContent = '';

  const t0 = Math.min(o.start, ...observed.map((p) => p.arrival));
  const t1 = f.end;
  const span = Math.max(1, t1 - t0);
  const x = (t) => ((t - t0) / span) * 100;

  const band = (cls, a, b, tip) => {
    const n = el('div', `fc-band ${cls}`);
    n.style.left = `${x(a)}%`;
    n.style.width = `${x(b) - x(a)}%`;
    attachTooltip(n, tip);
    host.append(n);
  };
  band('fc-band-window', o.start, o.end,
       `<b>Observation window</b><br>t=${o.start} to t=${o.end} (${o.duration} units, ${o.mode === 'explicit' ? 'as given' : 'inferred from the arrivals'})`);
  band('fc-band-horizon', f.start, f.end,
       `<b>Forecast horizon</b><br>t=${f.start} to t=${f.end}: arrivals expected ${fmt(f.expected, 2)}, this draw ${f.generated}`);

  /* processes arriving at the same instant stack upwards */
  const stack = {};
  let levels = 1;
  const dot = (p, predicted, i) => {
    const lvl = stack[p.arrival] = (stack[p.arrival] || 0) + 1;
    levels = Math.max(levels, lvl);
    const n = stagger(el('div', `arr-dot${predicted ? ' predicted' : ''}`), i);
    n.style.left = `${x(p.arrival)}%`;
    n.style.setProperty('--lvl', String(lvl - 1));
    attachTooltip(n, `<b>${esc(p.id)}</b> ${predicted ? 'predicted' : 'observed'} arrival`
      + `<br>t=${p.arrival} · burst ${p.burst} · priority ${p.priority}`);
    host.append(n);
  };
  observed.forEach((p, i) => dot(p, false, i));
  d.predicted.forEach((p, i) => dot(p, true, observed.length + i));
  host.style.setProperty('--levels', String(Math.min(levels, 12)));

  host.setAttribute('aria-label',
    `${observed.length} observed arrivals from t=${t0}; ${d.predicted.length} predicted arrivals between t=${f.start} and t=${f.end}`);

  niceTicks(t0, t1, 8).forEach((t, i) => {
    const tick = stagger(el('div', 'tick', String(t)), i);
    tick.style.left = `${x(t)}%`;
    ruler.append(tick);
  });
}

function renderPmf() {
  const f = state.forecast.data.forecast;
  const host = $('#fc-pmf');
  const axis = $('#fc-pmf-axis');
  host.textContent = '';
  axis.textContent = '';
  const pm = f.pmf;
  if (!pm.length) return;

  const maxP = Math.max(...pm.map((b) => b.p)) || 1;
  pm.forEach((b, i) => {
    const bar = stagger(el('div', 'pmf-bar'), i);
    if (b.k < f.range90.low || b.k > f.range90.high) bar.classList.add('out');
    if (b.k === f.generated && !f.truncated) bar.classList.add('drawn');
    bar.style.height = `${Math.max(1, (b.p / maxP) * 100)}%`;
    attachTooltip(bar, `P(K = ${b.k}) = ${prob(b.p)}`
      + (b.k === f.generated ? `<br>${f.truncated ? 'the cap' : 'this draw'}` : ''));
    host.append(bar);
  });
  host.setAttribute('aria-label',
    `Poisson probabilities of ${pm[0].k} to ${pm[pm.length - 1].k} arrivals; expected ${fmt(f.expected, 2)}`);

  const step = Math.max(1, Math.ceil(pm.length / 8));
  pm.forEach((b, i) => {
    if (i % step !== 0 && i !== pm.length - 1) return;
    const t = el('span', null, String(b.k));
    t.style.left = `${((i + 0.5) / pm.length) * 100}%`;
    axis.append(t);
  });

  const shown = f.generated >= pm[0].k && f.generated <= pm[pm.length - 1].k;
  let note = `P(K = k) for K ~ Poisson(λh = ${fmt(f.expected, 2)}). Full-strength bars are the central 90% `
    + `(${f.range90.low}–${f.range90.high}); faint bars lie outside it.`;
  if (f.truncated) {
    note += ` This draw hit the cap of ${f.cap}, so it is not a sample from this distribution`
      + ` (P(K > ${f.cap}): ${prob(f.pCapExceeded)}).`;
  } else {
    note += shown ? ` The outlined bar is this draw (K = ${f.generated}).`
                  : ` This draw (K = ${f.generated}) lies outside the bars shown.`;
    if (f.capFromUser) note += ` P(K > cap of ${f.cap}): ${prob(f.pCapExceeded)}.`;
  }
  $('#fc-pmf-note').textContent = note;
}

function renderDiagnostics() {
  const d = state.forecast.data;
  const o = d.observation;
  const s = d.stability;
  const sp = s.split;
  const ds = s.dispersion;
  const tb = $('#fc-diag tbody');
  tb.textContent = '';

  const rows = [
    ['Arrivals the checks use',
     `${s.testEvents} ${o.mode === 'explicit' ? '(every arrival in the window)'
                                                : '(those strictly between the first and last, which define the window)'}`],
    ['Rate, first vs second half',
     sp.valid
       ? `${fmt(sp.firstRate, 3)} vs ${fmt(sp.secondRate, 3)} per unit (${sp.firstCount} / ${sp.secondCount}); `
         + `exact binomial p = ${pval(sp.pValue)}: `
         + (sp.pValue < 0.05 ? 'the rate appears to change' : 'no evidence of a change')
       : `not run: needs at least ${sp.minEvents} arrivals, has ${s.testEvents}`],
    ['Count dispersion',
     ds.valid
       ? `variance/mean = ${fmt(ds.varianceToMean, 2)} over ${ds.bins} equal bins (Poisson ≈ 1); `
         + (ds.pOver < 0.05 ? `burstier than Poisson, p = ${pval(ds.pOver)}`
            : ds.pUnder < 0.05 ? `more regular than Poisson, p = ${pval(ds.pUnder)}`
            : 'consistent with Poisson at the 5% level')
       : `not run: needs at least ${ds.minEvents} arrivals, has ${s.testEvents}`],
    ['Simultaneous observed arrivals', String(s.simultaneousArrivals)],
    ['Predicted arrivals sharing a time unit',
     `${d.forecast.tiedArrivals}${d.forecast.tiedArrivals ? " (ordered by the scheduler's tie-break rules)" : ''}`],
    ['Mean observed gap', o.meanGap === null ? '— (one arrival)' : `${fmt(o.meanGap, 2)} units`],
    ['Random seed', `${d.seed} (same seed and settings reproduce this forecast exactly)`]
  ];
  rows.forEach(([k, v], i) => {
    const tr = stagger(el('tr'), i);
    tr.append(el('td', null, k));
    tr.append(el('td', null, v));
    tb.append(tr);
  });
}

function renderPredictedTable() {
  const pred = state.forecast.data.predicted;
  const tb = $('#fc-table tbody');
  tb.textContent = '';
  pred.forEach((p, i) => {
    const tr = stagger(el('tr', 'row-predicted'), i);
    [p.id, p.arrival, p.burst, p.priority].forEach((v) => tr.append(el('td', null, String(v))));
    const src = el('td');
    src.append(el('span', 'tag', 'forecast'));
    tr.append(src);
    tb.append(tr);
  });
  $('#fc-table-empty').hidden = pred.length > 0;
}

function renderForecastComparison() {
  const d = state.forecastSim;
  const a = d.augmented.algorithm;
  $('#fc-algo').textContent = a.needsQuantum ? `${a.key} (quantum ${a.quantum})` : a.key;

  const tb = $('#fc-cmp-table tbody');
  tb.textContent = '';
  d.comparison.forEach((r, i) => {
    const tr = stagger(el('tr'), i);
    const show = (v) => (r.decimals ? fmt(v, r.decimals) : String(v));
    tr.append(el('td', null, r.label));
    tr.append(el('td', null, show(r.original)));
    tr.append(el('td', null, show(r.augmented)));
    /* the sign of a difference the engine computed, spelled out */
    const same = Math.abs(r.change) < 1e-9;
    const td = el('td', 'change', same ? '0' : `${r.change > 0 ? '+' : '−'}${show(Math.abs(r.change))}`);
    td.append(el('span', 'change-word', same ? 'no change' : r.change > 0 ? 'higher' : 'lower'));
    tr.append(td);
    tb.append(tr);
  });
}

/* ───────────────────────────── theme ───────────────────────────── */

function applyTheme(mode) {
  const root = document.documentElement;
  if (mode === 'auto') root.removeAttribute('data-theme');
  else root.setAttribute('data-theme', mode);
  $('#theme-label').textContent =
    mode === 'auto' ? 'Auto' : mode === 'dark' ? 'Dark' : 'Light';
  try { localStorage.setItem('schedulify-theme', mode); } catch (_) { /* ignore */ }
}

function initTheme() {
  let mode = 'auto';
  try { mode = localStorage.getItem('schedulify-theme') || 'auto'; }
  catch (_) { /* private mode: stay on auto */ }
  applyTheme(mode);
  $('#theme-toggle').addEventListener('click', () => {
    const order = ['auto', 'light', 'dark'];
    let cur = 'auto';
    try { cur = localStorage.getItem('schedulify-theme') || 'auto'; } catch (_) {}
    applyTheme(order[(order.indexOf(cur) + 1) % order.length]);
  });
}

/* ───────────────────────────── wiring ───────────────────────────── */

function wireControls() {
  $('#add-proc').addEventListener('click', () => {
    if (state.rows.length >= state.limits.maxProcesses) return;
    const maxA = state.rows.reduce((m, r) => Math.max(m, Number(r.arrival) || 0), 0);
    addRow({ id: `P${nextRowSeq}`, arrival: maxA, burst: 3, priority: 0 });
  });

  $('#randomise').addEventListener('click', () => {
    const n = Math.max(3, Math.min(6, state.rows.length || 4));
    const procs = [];
    let t = 0;
    for (let i = 0; i < n; i++) {
      procs.push({
        id: `P${i + 1}`,
        arrival: t,
        burst: 1 + Math.floor(Math.random() * 8),
        priority: Math.floor(Math.random() * 5)
      });
      t += Math.floor(Math.random() * 4);
    }
    nextRowSeq = 1;
    setRows(procs);
  });

  const qr = $('#quantum-range');
  const qi = $('#quantum-input');
  const setQ = (v) => {
    const q = Math.max(state.limits.minQuantum,
                       Math.min(state.limits.maxQuantum, Math.round(v) || 1));
    state.quantum = q;
    qr.value = String(Math.min(Number(qr.max), q));
    qi.value = String(q);
  };
  qr.addEventListener('input', () => setQ(qr.valueAsNumber));
  qi.addEventListener('input', () => setQ(qi.valueAsNumber));

  $('#example-select').addEventListener('change', (ev) => {
    const idx = ev.target.value;
    if (idx === '') { $('#example-desc').textContent = ''; return; }
    const ex = state.examples[Number(idx)];
    if (!ex) return;
    nextRowSeq = 1;
    setRows(ex.processes);
    setQ(ex.quantum);
    $('#example-desc').textContent = ex.description;
  });

  $('#run').addEventListener('click', run);
  $('#compare').addEventListener('click', compareAll);

  $('#fc-window-mode').addEventListener('change', (ev) => {
    $('#fc-window').hidden = ev.target.value !== 'explicit';
  });
  $('#fc-generate').addEventListener('click', generateForecast);
  $('#fc-simulate').addEventListener('click', simulateForecast);
  $('#fc-reset').addEventListener('click', resetForecast);
  $('#fc-show-original').addEventListener('click', () => {
    if (state.forecastSim) showSchedule(state.forecastSim.original, 'original');
  });
  $('#fc-show-augmented').addEventListener('click', () => {
    if (state.forecastSim) showSchedule(state.forecastSim.augmented, 'augmented');
  });

  $('#play').addEventListener('click', togglePlay);
  $('#step-fwd').addEventListener('click', () => stepTo(+1));
  $('#step-back').addEventListener('click', () => stepTo(-1));
  $('#reset-clock').addEventListener('click', () => {
    stopPlayback();
    setClock(state.result ? state.result.window.start : 0);
  });

  const sp = $('#speed');
  sp.addEventListener('input', () => {
    state.speed = sp.valueAsNumber;
    $('#speed-out').textContent = `${state.speed}×`;
  });

  $('#scrub').addEventListener('input', (ev) => {
    if (!state.result) return;
    stopPlayback();
    const w = state.result.window;
    const frac = ev.target.valueAsNumber / 100;
    setClock(w.start + frac * (w.end - w.start));
  });

  /* Space toggles playback, arrows step. */
  document.addEventListener('keydown', (ev) => {
    if (!state.result) return;
    const tag = (ev.target.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
    if (ev.code === 'Space') { ev.preventDefault(); togglePlay(); }
    else if (ev.code === 'ArrowRight') { ev.preventDefault(); stepTo(+1); }
    else if (ev.code === 'ArrowLeft') { ev.preventDefault(); stepTo(-1); }
  });

  window.addEventListener('resize', () => {
    if (state.result) buildRuler();
  });
}

async function boot() {
  initTheme();
  wireControls();

  try {
    const meta = await api('/api/algorithms');
    state.algorithms = meta.algorithms;
    state.limits = meta.limits;
    state.quantum = meta.defaultQuantum;
    $('#engine-version').textContent = `engine v${meta.version}`;
    $('#quantum-input').min = String(meta.limits.minQuantum);
    $('#quantum-input').max = String(meta.limits.maxQuantum);
    renderAlgorithms();

    const ex = await api('/api/examples');
    state.examples = ex.examples;
    const sel = $('#example-select');
    ex.examples.forEach((e, i) => {
      const o = el('option', null, `${e.name} (${e.processes.length} processes)`);
      o.value = String(i);
      sel.append(o);
    });

    /* Start on the textbook SJF/SRTF workload so there is something to run. */
    const seed = state.examples.find((e) => /SJF/i.test(e.name)) || state.examples[0];
    if (seed) {
      nextRowSeq = 1;
      setRows(seed.processes);
      state.quantum = seed.quantum;
      $('#quantum-input').value = String(seed.quantum);
      $('#quantum-range').value = String(Math.min(12, seed.quantum));
      $('#example-desc').textContent = seed.description;
      $('#example-select').value = String(state.examples.indexOf(seed));
    } else {
      addRow();
    }
  } catch (e) {
    showError(`could not reach the C engine: ${e.message}`);
    addRow();
  }
}

boot();
