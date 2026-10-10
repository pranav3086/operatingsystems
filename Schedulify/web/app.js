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
  result: null,        // the engine's reply, verbatim
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

  state.algorithms.forEach((a) => {
    const b = el('button', 'algo');
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

async function run() {
  showError(null);
  $('#run').disabled = true;
  try {
    const data = await api('/api/simulate', workloadPayload());
    state.result = data;
    $('#empty-state').hidden = true;
    $('#result-view').hidden = false;
    $('#compare-view').hidden = true;
    buildChart();
    stopPlayback();
    setClock(data.window.start);
    startPlayback();
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

  $('#gantt-title').textContent = `${a.key} — ${a.name}`;
  $('#gantt-sub').textContent =
    a.needsQuantum
      ? `${a.kind}, quantum ${a.quantum} · ${r.processes.length} processes · `
        + `t=${r.window.start} to t=${r.window.end}`
      : `${a.kind} · ${r.processes.length} processes · `
        + `t=${r.window.start} to t=${r.window.end}`;

  $('#clock-end').textContent = `/ ${r.window.end}`;

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
    : `<b>${s.id}</b> on the CPU<br>t=${s.start} to t=${s.end} (${dur} unit${dur === 1 ? '' : 's'})`;
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
    const node = el('div', 'seg' + (s.idle ? ' idle' : ''));
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
      const m = el('div', 'marker');
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

  r.processes.forEach((p) => {
    const lane = el('div', 'lane');
    const name = el('div', 'lane-name');
    name.append(el('span', null, p.id));
    name.append(document.createElement('br'));
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
    const tick = el('div', 'tick', String(t));
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

  tiles.forEach(([label, value, unit, note]) => {
    const t = el('div', 'tile');
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

  r.processes.forEach((p) => {
    const tr = el('tr');
    tr.dataset.pid = p.id;
    [p.id, p.arrival, p.burst, p.priority, p.start, p.completion,
     p.turnaround, p.waiting, p.response].forEach((v) => {
      tr.append(el('td', null, String(v)));
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
    const li = el('li');
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

  const ready = $('#ready-queue');
  ready.textContent = '';
  const waiting = r.processes.filter((p) =>
    p.arrival <= t && exec[p.id] < p.burst && !(cur && !cur.idle && cur.id === p.id));
  if (!waiting.length) {
    ready.append(el('span', 'chips-empty', atEnd ? '—' : 'nobody waiting'));
  } else {
    waiting.forEach((p) => {
      const c = el('span', 'chip');
      c.append(el('span', null, p.id));
      c.append(el('span', 'chip-rem', ` ${num(p.burst - exec[p.id])} left`));
      ready.append(c);
    });
  }

  const done = $('#done-list');
  done.textContent = '';
  const finished = r.processes.filter((p) => exec[p.id] >= p.burst);
  if (!finished.length) {
    done.append(el('span', 'chips-empty', 'none yet'));
  } else {
    finished.forEach((p) => {
      const c = el('span', 'chip done');
      c.textContent = `${p.id} @ ${p.completion}`;
      done.append(c);
    });
  }

  $$('#proc-table tbody tr').forEach((tr) => {
    const pid = tr.dataset.pid;
    const p = r.processes.find((x) => x.id === pid);
    tr.classList.toggle('row-running',
      Boolean(cur) && !cur.idle && cur.id === pid && !atEnd);
    tr.classList.toggle('row-done', Boolean(p) && exec[pid] >= p.burst);
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
  $('#play-icon').textContent = '⏸';
  $('#play-text').textContent = 'Pause';
  $('#play').setAttribute('aria-label', 'Pause');
  state.rafId = requestAnimationFrame(frame);
}

function stopPlayback() {
  state.playing = false;
  if (state.rafId) cancelAnimationFrame(state.rafId);
  state.rafId = 0;
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

  data.results.forEach((r) => {
    if (r.error) return;
    const row = el('div', 'cmp-row');
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
