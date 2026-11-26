/* Hermes web UI: Live ops console + Flight mode.
   Talks to backend/hermes_backend.py over one WebSocket; video comes from MediaMTX over WebRTC (WHEP). */
(function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const token = new URLSearchParams(location.search).get('token');
  const withToken = (url) => token ? url + (url.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(token) : url;
  const TELLO_HFOV = 82.6;  // degrees, used to turn a box offset into a rotation hint

  const S = {
    config: null, events: new Map(), timeline: {}, health: null, dets: {}, drone: null,
    mode: 'live', focus: null, filter: 'active', paused: false, windowMin: 10,
    seenEvents: null, thumbTried: new Set(), stream: null, videoState: 'connecting',
    spark: [], sparkTracks: new Set(),
  };

  /* ---------------- helpers ---------------- */
  function toast(text, kind) {
    const t = document.createElement('div');
    t.className = 'toast' + (kind ? ' ' + kind : '');
    t.innerHTML = '<span class="dot"></span><span></span>';
    t.lastChild.textContent = text;
    $('toasts').appendChild(t);
    setTimeout(() => t.remove(), 5000);
  }
  const ago = (ts) => {
    const s = Math.max(0, Math.round(Date.now() / 1000 - ts));
    if (s < 60) return s + ' s ago';
    if (s < 3600) return Math.floor(s / 60) + ' m ' + (s % 60) + ' s ago';
    return Math.floor(s / 3600) + ' h ago';
  };
  const clock = (ts) => new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const srcById = (i) => (S.config && S.config.sources.find((s) => s.index === Number(i))) || null;
  const srcName = (i) => { const s = srcById(i); return s ? s.name : 'Source ' + i; };

  /* ---------------- WebSocket ---------------- */
  let ws = null, wsRetry = 1000;
  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(withToken(proto + '//' + location.host + '/ws'));
    ws.onopen = () => { wsRetry = 1000; setConn(true); };
    ws.onclose = (e) => {
      setConn(false, e.code === 4401 ? 'Access token required' : null);
      setTimeout(connect, wsRetry); wsRetry = Math.min(10000, wsRetry * 2);
    };
    ws.onmessage = (e) => handle(JSON.parse(e.data));
  }
  function send(msg) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(msg)); }
  setInterval(() => send({ type: 'heartbeat' }), 2000);   // keeps the drone's operator watchdog happy

  function setConn(ok, why) {
    const p = $('pill-conn');
    p.hidden = ok;
    if (!ok) { p.className = 'stat bad'; p.lastChild.textContent = why || 'Backend disconnected'; }
  }

  function handle(m) {
    switch (m.type) {
      case 'hello': S.config = m.config; S.drone = m.config.drone; setupMedia(); renderWall(); renderFlight(); break;
      case 'events':
        if (m.full) S.events = new Map(m.events.map((e) => [e.id, e]));
        else m.events.forEach((e) => S.events.set(e.id, e));
        announceNew(); renderEvents(); renderWallChips(); renderHeaderFires();
        break;
      case 'timeline': S.timeline = m.timeline; S.timelineNow = m.now; renderTimeline(); break;
      case 'health': S.health = m.health; renderHealth(); break;
      case 'dets': Object.assign(S.dets, m.dets); renderGuide(); trackSpark(m.dets); break;
      case 'drone': S.drone = m.drone; renderFlight(); break;
      case 'drone_result': if (!m.ok) droneMsg(m.error); break;
    }
  }

  function announceNew() {
    const ids = new Set(S.events.keys());
    if (S.seenEvents) {
      ids.forEach((id) => {
        if (!S.seenEvents.has(id)) {
          const e = S.events.get(id);
          toast('Fire #' + e.track + ' detected on ' + srcName(e.src), 'fire');
        }
      });
    }
    S.seenEvents = ids;
  }

  /* ---------------- video (WebRTC via MediaMTX WHEP, or the demo poster) ---------------- */
  let pc = null;
  async function setupMedia() {
    const v = S.config.video;
    if (v.mode === 'demo') { S.videoState = 'ready'; $('demo-badge').hidden = false; return; }
    if (pc) return;
    try {
      S.videoState = 'connecting'; updateVideoStates();
      pc = new RTCPeerConnection();
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.ontrack = (ev) => { S.stream = ev.streams[0] || new MediaStream([ev.track]); S.videoState = 'ready'; attachStreams(); updateVideoStates(); };
      pc.onconnectionstatechange = () => {
        if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) { retryMedia(); }
      };
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      await new Promise((res) => { if (pc.iceGatheringState === 'complete') return res(); pc.addEventListener('icegatheringstatechange', () => pc.iceGatheringState === 'complete' && res()); setTimeout(res, 1500); });
      const url = location.protocol + '//' + location.hostname + ':' + v.whep_port + v.whep_path;
      const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/sdp' }, body: pc.localDescription.sdp });
      if (!r.ok) throw new Error('MediaMTX answered ' + r.status);
      await pc.setRemoteDescription({ type: 'answer', sdp: await r.text() });
    } catch (err) {
      console.warn('video:', err);
      retryMedia();
    }
  }
  function retryMedia() {
    if (pc) { try { pc.close(); } catch (e) { /* already closed */ } pc = null; }
    S.stream = null; S.videoState = 'offline'; updateVideoStates();
    setTimeout(setupMedia, 5000);
  }
  function mediaEl() {
    const v = S.config.video;
    if (v.mode === 'demo') { const img = new Image(); img.src = v.poster; img.alt = ''; return img; }
    const vid = document.createElement('video');
    vid.muted = true; vid.autoplay = true; vid.playsInline = true;
    if (S.stream) vid.srcObject = S.stream;
    return vid;
  }
  function attachStreams() {
    document.querySelectorAll('.crop > video').forEach((vid) => { if (vid.srcObject !== S.stream) vid.srcObject = S.stream; });
  }
  function updateVideoStates() {
    const text = S.videoState === 'ready' ? '' : S.videoState === 'connecting'
      ? 'Connecting video…' : 'Video offline. Is MediaMTX running on port ' + (S.config ? S.config.video.whep_port : 8889) + '? Retrying.';
    document.querySelectorAll('.video-state').forEach((el) => { el.hidden = !text; el.textContent = text; });
  }

  /* A tile shows one source by cropping the tiled pipeline output. */
  function makeTile(src) {
    const g = S.config.grid;
    const tile = document.createElement('div');
    tile.className = 'tile';
    tile.dataset.src = src.index;
    tile.style.setProperty('--cols', g.cols); tile.style.setProperty('--rows', g.rows);
    tile.style.setProperty('--col', src.col); tile.style.setProperty('--row', src.row);
    tile.innerHTML = '<div class="crop"></div><div class="video-state" hidden></div>'
      + '<div class="tl"><span class="chip name"></span></div><div class="tr"><span class="chip fires" hidden></span><span class="chip fps"></span></div>';
    tile.querySelector('.crop').appendChild(mediaEl());
    tile.querySelector('.name').textContent = src.name;
    tile.querySelector('.name').title = src.uri;
    return tile;
  }

  /* ---------------- live: wall ---------------- */
  function renderWall() {
    if (!S.config) return;
    const wall = $('wall');
    wall.innerHTML = '';
    const sources = S.config.sources;
    $('wall-empty').hidden = sources.length > 0;
    if (!sources.length) return;
    const shown = S.focus !== null ? sources.filter((s) => s.index === S.focus) : sources;
    const g = S.config.grid;
    wall.style.gridTemplateColumns = S.focus !== null ? '1fr' : 'repeat(' + g.cols + ', 1fr)';
    wall.style.gridTemplateRows = S.focus !== null ? '1fr' : 'repeat(' + g.rows + ', 1fr)';
    shown.forEach((src) => {
      const tile = makeTile(src);
      tile.addEventListener('click', () => setFocus(S.focus === src.index ? null : src.index));
      tile.tabIndex = 0; tile.setAttribute('role', 'button');
      tile.setAttribute('aria-label', (S.focus === src.index ? 'Show all cameras' : 'Focus ' + src.name));
      tile.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); tile.click(); } });
      wall.appendChild(tile);
    });
    $('wall-head').hidden = S.focus === null;
    if (S.focus !== null) $('focus-title').textContent = srcName(S.focus);
    updateVideoStates(); renderWallChips();
  }
  function setFocus(i) { S.focus = i; renderWall(); }
  $('unfocus').addEventListener('click', () => setFocus(null));

  function activeOn(src) {
    let n = 0;
    S.events.forEach((e) => { if (e.src === src && e.visible && e.status !== 'acknowledged') n++; });
    return n;
  }
  function renderWallChips() {
    if (!S.config) return;
    const fps = (S.health && S.health.pipeline.fps) || {};
    document.querySelectorAll('#wall .tile').forEach((tile) => {
      const src = Number(tile.dataset.src), n = activeOn(src);
      const chip = tile.querySelector('.fires');
      chip.hidden = !n; chip.className = 'chip fires fire';
      chip.innerHTML = '<span class="dot"></span>' + n + (n === 1 ? ' fire' : ' fires');
      tile.classList.toggle('alarm', n > 0);
      tile.querySelector('.fps').textContent = (fps[src] != null ? fps[src] : '–') + ' fps';
    });
  }

  /* ---------------- live: events ---------------- */
  const ACTIVE = ['new', 'active', 'intermittent'];
  function renderEvents() {
    const all = [...S.events.values()];
    const list = all.filter((e) => S.filter === 'all' || (S.filter === 'acknowledged' ? e.status === 'acknowledged' : ACTIVE.includes(e.status)));
    $('count-active').textContent = all.filter((e) => ACTIVE.includes(e.status)).length;
    $('count-ack').textContent = all.filter((e) => e.status === 'acknowledged').length;
    $('events-empty').hidden = list.length > 0;
    $('events-empty').textContent = all.length ? 'Nothing in this view.' : 'No fires detected. Events appear here the moment the detector tracks one.';
    const order = ['new', 'active', 'intermittent', 'acknowledged', 'ended'];
    list.sort((a, b) => (order.indexOf(a.status) - order.indexOf(b.status)) || (b.last_seen - a.last_seen));
    // Each card's DOM is built once and then only its text changes, so buttons never get
    // swapped out from under the pointer.
    const box = $('events');
    S.cards = S.cards || new Map();
    const keep = new Set();
    list.forEach((e, i) => {
      let el = S.cards.get(e.id);
      if (!el) { el = buildCard(e); S.cards.set(e.id, el); }
      updateCard(el, e);
      keep.add(e.id);
      if (box.children[i] !== el) box.insertBefore(el, box.children[i] || null);
    });
    S.cards.forEach((el, id) => { if (!keep.has(id)) { el.remove(); S.cards.delete(id); } });
    list.forEach(maybeThumb);
  }
  const STATUS_LABEL = { new: 'New', active: 'Active', intermittent: 'Intermittent', acknowledged: 'Acknowledged', ended: 'Ended' };
  const FLAME = '<svg class="i" viewBox="0 0 24 24"><path d="M12 3c1 3 4 4.5 4 8.5a4 4 0 0 1-8 0c0-2 1-3 2-4 0 2 1 3 2 3 0-3-1-5 0-7.5Z"/></svg>';
  function buildCard(e) {
    const el = document.createElement('article');
    el.className = 'ev enter';
    el.innerHTML = '<span class="thumb">' + FLAME + '</span>'
      + '<div><h4><span class="ttl"></span> <span class="sev"></span></h4><dl>'
      + '<dt>Source</dt><dd class="f-src"></dd><dt class="f-seen-l"></dt><dd class="f-seen"></dd>'
      + '<dt>Confidence</dt><dd class="f-conf"></dd></dl></div>'
      + '<div class="acts"><button class="btn f-focus">Focus</button><button class="btn f-ack">Acknowledge</button></div>';
    el.querySelector('.f-focus').dataset.focus = e.src;
    el.querySelector('.f-ack').dataset.ack = e.id;
    setTimeout(() => el.classList.remove('enter'), 400);
    return el;
  }
  function setText(el, sel, text) { const n = el.querySelector(sel); if (n.textContent !== text) n.textContent = text; }
  function updateCard(el, e) {
    el.classList.toggle('new', e.status === 'new');
    setText(el, '.ttl', 'Fire #' + e.track);
    const sev = el.querySelector('.sev');
    sev.className = 'sev ' + e.status; setText(el, '.sev', STATUS_LABEL[e.status]);
    setText(el, '.f-src', srcName(e.src));
    if (e.status === 'intermittent') { setText(el, '.f-seen-l', 'Seen'); setText(el, '.f-seen', e.appearances + ' times · last ' + ago(e.last_seen)); }
    else if (e.status === 'ended') { setText(el, '.f-seen-l', 'Last seen'); setText(el, '.f-seen', clock(e.last_seen)); }
    else { setText(el, '.f-seen-l', 'First seen'); setText(el, '.f-seen', clock(e.first_seen) + ' · ' + ago(e.first_seen)); }
    setText(el, '.f-conf', e.peak_conf ? e.peak_conf.toFixed(2) + ' peak' : 'tracking');
    el.querySelector('.f-focus').classList.toggle('primary', e.status === 'new');
    el.querySelector('.f-ack').hidden = e.status === 'acknowledged';
    if (e.thumb && el.dataset.thumb !== e.thumb) {
      el.dataset.thumb = e.thumb;
      const img = new Image(); img.className = 'thumb'; img.alt = 'Fire #' + e.track + ' snapshot'; img.src = withToken(e.thumb);
      el.querySelector('.thumb').replaceWith(img);
    }
  }
  $('events').addEventListener('click', (ev) => {
    const b = ev.target.closest('button'); if (!b) return;
    if (b.dataset.ack) send({ type: 'ack', id: b.dataset.ack });
    if (b.dataset.focus !== undefined) { switchMode('live'); setFocus(Number(b.dataset.focus)); }
  });
  document.querySelectorAll('.rail .seg button').forEach((b) => b.addEventListener('click', () => {
    S.filter = b.dataset.filter;
    document.querySelectorAll('.rail .seg button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
    renderEvents();
  }));

  /* Grab a snapshot of a fire from the live video and store it with the event. */
  function maybeThumb(e) {
    if (e.thumb || !e.visible || S.thumbTried.has(e.id) || !S.config) return;
    const src = srcById(e.src), media = document.querySelector('.crop > video, .crop > img');
    if (!src || !media) return;
    const w = media.videoWidth || media.naturalWidth, h = media.videoHeight || media.naturalHeight;
    if (!w || !h || (media.tagName === 'VIDEO' && media.readyState < 2)) return;
    S.thumbTried.add(e.id);
    const g = S.config.grid;
    // box (per-source muxer coords) -> tiled frame -> media pixels, padded 40%
    const tw = w / g.cols, th = h / g.rows, sx = tw / g.src_width, sy = th / g.src_height;
    let [x, y, bw, bh] = e.box;
    const pad = 0.4;
    x -= bw * pad / 2; y -= bh * pad / 2; bw *= 1 + pad; bh *= 1 + pad;
    const cx = Math.max(0, src.col * tw + x * sx), cy = Math.max(0, src.row * th + y * sy);
    const cw = Math.min(tw, bw * sx), ch = Math.min(th, bh * sy);
    if (cw < 4 || ch < 4) return;
    const canvas = document.createElement('canvas');
    canvas.width = 176; canvas.height = 128;
    const ctx = canvas.getContext('2d');
    const scale = Math.max(canvas.width / cw, canvas.height / ch);
    const dw = canvas.width / scale, dh = canvas.height / scale;
    ctx.drawImage(media, cx + (cw - dw) / 2, cy + (ch - dh) / 2, dw, dh, 0, 0, canvas.width, canvas.height);
    let data;
    try { data = canvas.toDataURL('image/jpeg', 0.8); } catch (err) { return; }
    fetch(withToken('/api/events/' + encodeURIComponent(e.id) + '/thumb'), {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ data }),
    }).catch(() => S.thumbTried.delete(e.id));
  }

  /* ---------------- live: timeline + health ---------------- */
  $('tl-pause').addEventListener('click', () => {
    S.paused = !S.paused;
    $('tl-pause').setAttribute('aria-pressed', String(S.paused));
    $('tl-pause').lastChild.textContent = S.paused ? 'Resume' : 'Pause';
    if (!S.paused) renderTimeline();
  });
  document.querySelectorAll('[data-window]').forEach((b) => b.addEventListener('click', () => {
    S.windowMin = Number(b.dataset.window);
    document.querySelectorAll('[data-window]').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
    renderTimeline(true);
  }));
  function renderTimeline(force) {
    if (!S.config || (S.paused && !force)) return;
    const now = S.timelineNow || Date.now() / 1000, span = S.windowMin * 60, start = now - span;
    const pct = (t) => Math.max(0, Math.min(100, (t - start) / span * 100));
    $('lanes').innerHTML = S.config.sources.map((src) => {
      const bars = (S.timeline[src.index] || []).filter(([a, b]) => b >= start).map(([a, b]) =>
        '<i class="bar" style="left:' + pct(a) + '%;width:' + Math.max(0.3, pct(b) - pct(a)) + '%"></i>').join('');
      const marks = [...S.events.values()].filter((e) => e.src === src.index && e.first_seen >= start)
        .map((e) => '<i class="mark" title="Fire #' + e.track + ' first seen ' + clock(e.first_seen) + '" style="left:' + pct(e.first_seen) + '%"></i>').join('');
      return '<div class="lane"><span title="' + esc(src.uri) + '">' + esc(src.name) + '</span><div class="track">' + bars + marks + '</div></div>';
    }).join('');
    const ticks = 5;
    $('axis').innerHTML = Array.from({ length: ticks + 1 }, (_, i) => '<span>' + (i === ticks ? 'now' : '−' + Math.round(S.windowMin * (1 - i / ticks)) + ' min') + '</span>').join('');
  }
  function renderHealth() {
    const h = S.health; if (!h) return;
    const d = h.device;
    $('kpi-gpu').textContent = d.gpu != null ? d.gpu + '%' : 'n/a';
    $('kpi-temp').textContent = d.temp != null ? Math.round(d.temp) + '°C' : 'n/a';
    $('kpi-mem').textContent = d.mem != null ? d.mem + '%' : 'n/a';
    const p = h.pipeline, pill = $('pill-pipe');
    const label = { running: 'Pipeline running', stalled: 'Pipeline stalled', offline: 'Pipeline offline' }[p.state];
    pill.className = 'stat ' + ({ running: 'ok', stalled: 'warn', offline: 'bad' }[p.state]);
    pill.lastChild.textContent = label;
    const fps = Object.values(p.fps), n = S.config ? S.config.sources.length : 0;
    $('pill-src').textContent = n + (n === 1 ? ' source' : ' sources') + (fps.length ? ' · ' + Math.round(fps.reduce((a, b) => a + b, 0) / fps.length) + ' fps' : '');
    $('fps-rows').innerHTML = (S.config ? S.config.sources : []).map((s) => {
      const f = p.fps[s.index] || 0;
      return '<div class="fpsrow"><span>' + esc(s.name) + '</span><span class="meter' + (f < 20 ? ' low' : '') + '"><i style="width:' + Math.min(100, f / 30 * 100) + '%"></i></span><span>' + f + '</span></div>';
    }).join('');
    renderWallChips();
  }
  function renderHeaderFires() {
    let n = 0; S.events.forEach((e) => { if (ACTIVE.includes(e.status) && e.visible) n++; });
    const p = $('pill-fires');
    p.hidden = !n; p.className = 'stat fires';
    p.lastChild.textContent = n + (n === 1 ? ' fire in view' : ' fires in view');
  }
  setInterval(() => { renderEvents(); renderTimeline(); }, 5000);   // keep "x s ago" and the window moving

  /* ---------------- flight ---------------- */
  const droneSrc = () => S.config && S.config.sources.find((s) => s.is_drone);
  let flightTileFor = null;
  function renderFlight() {
    if (!S.config) return;
    const src = droneSrc(), box = $('flight-video');
    $('flight-empty').hidden = !!src;
    if (src && flightTileFor !== src.index) {
      box.querySelectorAll('.tile').forEach((t) => t.remove());
      box.prepend(makeTile(src));
      flightTileFor = src.index;
      $('hud-src').textContent = src.name + ' · ' + src.uri;
      updateVideoStates();
    }
    const d = S.drone || { available: false };
    $('drone-off').hidden = d.available && d.connected;
    const t = d.telemetry || {};
    $('t-batt').textContent = t.battery != null ? t.battery + '%' : '–';
    const bar = $('t-batt-bar'); bar.style.width = (t.battery || 0) + '%';
    bar.parentElement.className = 'batt' + (t.battery < 10 ? ' crit' : t.battery < 20 ? ' low' : '');
    $('t-time').textContent = t.flight_time != null ? Math.floor(t.flight_time / 60) + ':' + String(t.flight_time % 60).padStart(2, '0') : '–';
    $('t-height').textContent = t.height_cm != null ? (t.height_cm / 100).toFixed(1) + ' m' : '–';
    $('t-temp').textContent = t.temperature != null ? Math.round(t.temperature) + '°C' : '–';
    $('hud-alt').textContent = 'ALT ' + (t.height_cm != null ? (t.height_cm / 100).toFixed(1) + ' m' : '–');
    $('hud-yaw').textContent = 'YAW ' + (t.yaw != null ? ((t.yaw + 360) % 360) + '°' : '–');
    const sp = t.speed ? Math.hypot(t.speed[0], t.speed[1], t.speed[2]) / 10 : null;   // Tello reports dm/s
    $('hud-spd').textContent = 'SPD ' + (sp != null ? sp.toFixed(1) + ' m/s' : '–');
    $('hud-flight').textContent = d.busy ? 'Running: ' + d.busy.replace('_', ' ') : d.flying ? 'Flying' : 'On ground';
    if (d.step_cm) $('step-label').textContent = d.step_cm + ' cm · ' + d.step_deg + '°';

    const arm = $('arm');
    arm.checked = !!d.armed; arm.disabled = !d.connected;
    $('arm-label').textContent = d.armed ? 'Armed' : 'Disarmed';
    const can = d.connected && d.armed && !d.busy;
    $('btn-takeoff').disabled = !can || d.flying;
    $('btn-land').disabled = !can || !d.flying;
    $('btn-emergency').disabled = !d.connected;
    document.querySelectorAll('.pad button').forEach((b) => { b.disabled = !can || !d.flying; });
    // Backend notices (auto-land, emergency) win; a local rejection stays up for 5 s.
    const local = S.localMsg && Date.now() - S.localMsg.at < 5000 ? S.localMsg.text : '';
    $('drone-msg').textContent = d.error || local;
  }
  function droneMsg(text) { S.localMsg = { text: text || '', at: Date.now() }; $('drone-msg').textContent = text || ''; }
  function command(cmd, el) {
    send({ type: 'drone', cmd });
    if (el) { el.classList.add('flash'); setTimeout(() => el.classList.remove('flash'), 200); }
  }
  $('arm').addEventListener('change', (e) => { send({ type: 'arm', on: e.target.checked }); droneMsg(''); });
  document.querySelectorAll('#flight [data-cmd]').forEach((b) => b.addEventListener('click', () => command(b.dataset.cmd, b)));

  // Keyboard: only on the Flight tab, only while armed, never while typing. No shortcut for emergency stop.
  const KEYS = { w: 'forward', a: 'left', s: 'back', d: 'right', ArrowUp: 'up', ArrowDown: 'down',
                 ArrowLeft: 'yaw_left', ArrowRight: 'yaw_right', t: 'takeoff', ' ': 'land' };
  document.addEventListener('keydown', (e) => {
    if (S.mode !== 'flight' || !S.drone || !S.drone.armed || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
    if (/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName) && document.activeElement.id !== 'arm') return;
    const cmd = KEYS[e.key.length === 1 ? e.key.toLowerCase() : e.key];
    if (!cmd) return;
    e.preventDefault();
    command(cmd, document.querySelector('#flight [data-cmd="' + cmd + '"]'));
  });

  // Gamepad: left stick = move, right stick = yaw/altitude, A = take off, B = land.
  let lastRc = 0, padButtons = [];
  function pollGamepad(ts) {
    const pad = (navigator.getGamepads ? [...navigator.getGamepads()] : []).find(Boolean);
    $('gamepad-status').textContent = pad ? 'Gamepad: ' + pad.id.slice(0, 40) + ' · sticks fly, A take off, B land.' : 'Gamepad: not connected.';
    const d = S.drone;
    if (pad && S.mode === 'flight' && d && d.armed) {
      const ax = (i) => (Math.abs(pad.axes[i] || 0) < 0.12 ? 0 : Math.round((pad.axes[i] || 0) * 100));
      if (d.flying && ts - lastRc > 100) { send({ type: 'rc', lr: ax(0), fb: -ax(1), yaw: ax(2), ud: -ax(3) }); lastRc = ts; }
      const pressed = pad.buttons.map((b) => b.pressed);
      if (pressed[0] && !padButtons[0] && !d.flying) command('takeoff');
      if (pressed[1] && !padButtons[1] && d.flying) command('land');
      padButtons = pressed;
    }
    requestAnimationFrame(pollGamepad);
  }
  requestAnimationFrame(pollGamepad);

  /* Point the pilot at the fire: offset of the biggest fire box from the frame centre. */
  function renderGuide() {
    const src = droneSrc(), guide = $('guide');
    if (!src || !S.config) { guide.hidden = true; return; }
    const objs = S.dets[src.index] || [];
    if (!objs.length) { guide.hidden = true; return; }
    const o = objs.reduce((a, b) => (a.bw * a.bh >= b.bw * b.bh ? a : b));
    const cx = (o.x + o.bw / 2) / S.config.grid.src_width - 0.5;   // -0.5 .. 0.5
    const deg = Math.round(cx * TELLO_HFOV / 10) * 10;
    guide.hidden = false;
    const left = cx < 0, centred = Math.abs(cx) < 0.06;
    $('guide-title').textContent = 'Fire #' + o.id + (centred ? ' centred' : left ? ' left of centre' : ' right of centre');
    $('guide-sub').textContent = centred ? 'hold heading' : 'offset ' + (cx > 0 ? '+' : '') + Math.round(cx * 200) + '% · rotate ' + (left ? 'left ' : 'right ') + Math.abs(deg || 10) + '°';
    $('guide-path').setAttribute('d', centred ? 'M5 12h14M12 5v14' : left ? 'M15 18l-6-6 6-6' : 'M9 18l6-6-6-6');
  }

  function trackSpark(dets) {
    const src = droneSrc(); if (!src || !(String(src.index) in dets)) return;
    const objs = dets[src.index] || [];
    objs.forEach((o) => S.sparkTracks.add(o.id));
    const bucket = Math.floor(Date.now() / 5000);
    const last = S.spark[S.spark.length - 1];
    if (last && last.b === bucket) last.n = Math.max(last.n, objs.length);
    else { S.spark.push({ b: bucket, n: objs.length }); if (S.spark.length > 60) S.spark.shift(); }
    const max = Math.max(2, ...S.spark.map((p) => p.n));
    // newest bucket at the right edge; 60 buckets x 5 s = the last 5 minutes
    const pts = S.spark.map((p, i) => ((60 - S.spark.length + i) / 59 * 600).toFixed(1) + ',' + (38 - p.n / max * 32).toFixed(1));
    $('spark').innerHTML = '<polyline fill="none" stroke="#2E3540" stroke-width="1" points="0,38 600,38"/>'
      + (pts.length > 1 ? '<polyline fill="rgba(255,90,54,.18)" stroke="#FF5A36" stroke-width="1.5" points="' + pts[0].split(',')[0] + ',38 ' + pts.join(' ') + ' ' + pts[pts.length - 1].split(',')[0] + ',38"/>' : '');
    $('spark-total').textContent = S.sparkTracks.size + (S.sparkTracks.size === 1 ? ' track' : ' tracks');
  }

  /* ---------------- modes ---------------- */
  function switchMode(mode) {
    S.mode = mode;
    $('live').hidden = mode !== 'live'; $('flight').hidden = mode !== 'flight';
    $('tab-live').setAttribute('aria-selected', String(mode === 'live'));
    $('tab-flight').setAttribute('aria-selected', String(mode === 'flight'));
    if (mode === 'live') renderWall(); else { flightTileFor = null; renderFlight(); }
  }
  $('tab-live').addEventListener('click', () => switchMode('live'));
  $('tab-flight').addEventListener('click', () => switchMode('flight'));

  connect();
})();
