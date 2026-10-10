// Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
// SPDX-License-Identifier: Apache-2.0
//
// 机器人控制台里的语音面板。ptt_server 在 /voice/panel.js 提供，控制台网页在 HTTPS 下加载它，挂到 #voice-panel，
// 切到「语音」时调 window.voicePanel.setActive(true)。
// 按住说话：按下时向服务端占用话筒（同时打断机器人），录音，松手后转成 16 kHz 单声道 wav 上传。
// 对话窗口显示最近的对话（所有人的），机器人回答边生成边显示。接口要带控制台的配对令牌（同源 sessionStorage）。
// 控制台广播 operator-status 事件（控制权、状态、动作阶段），面板据此显示现在能不能让机器人做动作。
'use strict';
(() => {
  const PRE_ROLL = 3;       // 按下前保留的缓冲块数（4096 点一块，约 0.25 s），防止吃掉第一个字
  const TAIL_MS = 250;      // 松手后再录一点，防止吃掉最后一个字
  const EXAMPLES = ['你是谁？', '你能做什么？', '讲个笑话', '今天适合做什么？'];
  const BUSY_PHASES = ['BLEND_IN', 'PLAYING', 'HOLDING', 'BLEND_OUT'];
  // Lucide 图标（ISC 许可，和控制台同一套）
  const ICONS = {
    mic: '<path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><line x1="12" x2="12" y1="19" y2="22"/>',
    messages: '<path d="M14 9a2 2 0 0 1-2 2H6l-4 4V4a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2z"/><path d="M18 9h2a2 2 0 0 1 2 2v11l-4-4h-6a2 2 0 0 1-2-2v-1"/>',
    grid: '<rect width="7" height="7" x="3" y="3" rx="1"/><rect width="7" height="7" x="14" y="3" rx="1"/><rect width="7" height="7" x="14" y="14" rx="1"/><rect width="7" height="7" x="3" y="14" rx="1"/>',
    x: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
    hand: '<path d="M18 11V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2"/><path d="M14 10V4a2 2 0 0 0-2-2a2 2 0 0 0-2 2v2"/><path d="M10 10.5V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2v8"/><path d="M18 8a2 2 0 1 1 4 0v6a8 8 0 0 1-8 8h-2c-2.8 0-4.5-.86-5.99-2.34l-3.6-3.6a2 2 0 0 1 2.83-2.82L7 15"/>',
  };
  const icon = (name) => `<svg aria-hidden="true"><use href="#vp-i-${name}"/></svg>`;

  const storage = (area, key, value) => {  // 隐私模式下 storage 可能不可用
    try {
      if (value === undefined) return area.getItem(key);
      area.setItem(key, value);
    } catch (_) { /* 不可用就不记 */ }
    return value;
  };
  const client = (() => {
    let id = storage(localStorage, 'ptt_client');
    if (!id) {
      id = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, '0')).join('');
      storage(localStorage, 'ptt_client', id);
    }
    return id;
  })();
  // 控制台把配对令牌存在本页 sessionStorage（operator.session），重新配对后会变，每次请求现取
  const api = (path, method = 'GET', body, extra = '') => fetch(`/voice/api/${path}?client=${client}${extra}`, {
    method, body, cache: 'no-store', headers: {'X-Operator-Token': storage(sessionStorage, 'operator.session') || ''}});
  const sleep = (ms) => new Promise((ok) => setTimeout(ok, ms));

  // ---- 界面 ----
  const css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = '/voice/panel.css';
  document.head.append(css);
  const sprite = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  sprite.setAttribute('class', 'vp-sprite');
  sprite.setAttribute('aria-hidden', 'true');
  sprite.innerHTML = Object.entries(ICONS).map(([k, v]) => `<symbol id="vp-i-${k}" viewBox="0 0 24 24">${v}</symbol>`).join('');
  document.body.prepend(sprite);

  let root = document.getElementById('voice-panel');
  if (!root) {
    root = document.createElement('section');
    root.id = 'voice-panel';
    root.hidden = true;
    (document.querySelector('main') || document.body).append(root);
  }
  root.classList.add('voice-panel');
  root.dataset.state = 'start';
  root.innerHTML = `
    <div class="vp-chat">
      <div class="vp-chat-head"><h2>${icon('messages')}<span class="vp-robot">机器人</span></h2><span class="vp-note"></span></div>
      <div class="vp-scroll"></div>
    </div>
    <p class="vp-action" data-tone="off"><span class="vp-dot"></span><span class="vp-action-text">正在连接机器人…</span></p>
    <div class="vp-skills" hidden><div class="vp-chips vp-actions"></div>
      <button class="vp-chip vp-more">${icon('grid')}<span class="vp-all">全部</span></button></div>
    <div class="vp-dock">
      <p class="vp-status" role="status"></p>
      <button class="primary vp-enable">${icon('mic')}开启麦克风</button>
      <div class="vp-talk-wrap"><button class="vp-talk">${icon('mic')}<strong class="vp-talk-label">按住说话</strong></button></div>
    </div>`;
  const backdrop = document.createElement('div');
  backdrop.className = 'vp-backdrop';
  const sheet = document.createElement('section');
  sheet.className = 'vp-sheet';
  sheet.setAttribute('aria-label', '我会这些动作');
  sheet.innerHTML = `
    <div class="vp-sheet-head"><h2>${icon('hand')}我会这些动作</h2>
      <button class="icon vp-close" aria-label="关闭">${icon('x')}</button></div>
    <p class="vp-sheet-note">直接说出来就行，比如“挥个手”“比个心”</p>
    <div class="vp-chips wrap vp-sheet-actions"></div>
    <p class="vp-sheet-note">也可以随便聊</p>
    <div class="vp-chips wrap vp-examples"></div>`;
  document.body.append(backdrop, sheet);
  const $ = (cls) => root.querySelector(cls) || sheet.querySelector(cls);
  const talk = $('.vp-talk'), box = $('.vp-scroll');

  // ---- 状态 ----
  let ctx = null, chunks = [], ring = [], recording = false, pressing = false, startedAt = 0, holdTimer = null, tick = null;
  let maxHoldS = 15, status = null, local = '', localTimer = null, pressReq = null, offline = false, denied = false;
  let active = false, wake = null, infoLoaded = false, infoLoading = false, op = null, lastAction = '';
  let stick = true;  // 对话停在最底下：来新消息、窗口变矮都跟到底；往上翻看历史时不动

  const robotName = () => (document.getElementById('robot')?.textContent || '').trim() || '机器人';

  function show(text, ms) {
    local = text;
    clearTimeout(localTimer);
    if (ms) localTimer = setTimeout(() => { local = ''; render(); }, ms);
    render();
  }

  const liveTurn = () => status && status.history.find((t) => t.id === status.live_id);
  // 现在按不了：没配对、没准备好、别人按着、机器人在回答别人（和服务端 press 的拒绝条件一致）
  const blocked = () => denied || !status || !status.alive || !status.ready || status.holder === 'other' || status.answering_other;

  function statusText() {
    if (recording) {
      const s = Math.min(maxHoldS, Math.floor((Date.now() - startedAt) / 1000));
      return `正在录音 ${s} s / ${maxHoldS} s，松手发送`;
    }
    if (local) return local;
    if (denied) return '请先扫码配对机器人，才能语音对话';
    if (offline) return '连不上语音服务，正在重试…';
    if (!status) return '正在连接…';
    if (!status.alive) return '语音暂时用不了（可能是机器人没联网），正在重试…';
    if (!status.ready) return '语音服务正在启动…';
    if (status.holder === 'other') return '别人正在说话，请稍等';
    const turn = liveTurn(), mine = !!turn && turn.mine;
    if (status.state === 'recognizing') return mine ? '正在听你说了什么…' : '正在听别人说的话…';
    if (status.state === 'thinking') return mine ? '机器人正在回答 · 按住可打断' : '机器人正在回答别人，请稍等';
    return ctx ? '按住说话，松手发送' : '首次使用要允许浏览器使用麦克风';
  }

  function render() {
    const busy = blocked() && !recording;
    root.dataset.state = !ctx ? 'start' : recording ? 'rec' : busy ? 'busy'
      : status && status.state !== 'idle' ? 'answer' : 'idle';
    $('.vp-status').textContent = statusText();
    talk.disabled = busy;
    talk.classList.toggle('rec', recording);
    $('.vp-talk-label').textContent = recording ? '松手发送' : busy ? '请稍等' : '按住说话';
    $('.vp-robot').textContent = robotName();
    renderAction();
    renderChat();
  }

  // ---- 动作状态：控制台广播的状态里有当前控制者（名字#会话号，语音服务叫 voice）和动作阶段 ----
  function actionLabel(key) {
    const c = op && op.catalog, s = op && op.status;
    if (!c || !key) return key || '';
    const trajectory = s?.trajectory_enabled ?? c.trajectory_enabled;
    const list = trajectory ? c.actions : (c.policies || []).find((p) => p.name === s?.policy)?.actions;
    return (list || []).find((a) => a.key === key)?.display_name || key;
  }

  // 最近一次让机器人做动作的是不是本机
  function lastActor() {
    const turn = status && [...status.history].reverse().find((t) => (t.pending.length || t.tools.length) && t.ago_s < 60);
    return turn ? (turn.mine ? '你' : '其他人') : '';
  }

  function actionState() {
    if (!op || !op.authenticated) return ['off', '还没连上机器人，现在只能聊天'];
    const s = op.status;
    if (!op.online || !s) return ['off', '机器人控制链路不在线，现在只能聊天'];
    if (s.fault?.latched) return ['off', '机器人有故障，现在只能聊天'];
    const owner = s.owner || '';
    const playing = BUSY_PHASES.includes(s.interaction?.phase);
    if (owner.startsWith('voice#') || (playing && !owner)) {
      const name = actionLabel(s.interaction?.action), who = lastActor();
      return ['busy', `正在做${name ? `「${name}」` : '动作'}${who ? ` · ${who}让它做的` : ''}`];
    }
    if (owner) return ['other', s.owns_control ? '你正握着控制权，语音做不了动作' : '其他人正在手动控制 · 现在只能聊天'];
    const trajectory = s.trajectory_enabled ?? op.catalog?.trajectory_enabled;
    if (s.state !== (trajectory ? 'TRAJECTORY' : 'RL')) return ['off', '机器人还没进入动作模式，现在只能聊天'];
    return ['ok', '可以让它做动作'];
  }

  function renderAction() {
    const [tone, text] = actionState();
    const key = tone + text;
    if (key === lastAction) return;
    lastAction = key;
    $('.vp-action').dataset.tone = tone;
    $('.vp-action-text').textContent = text;
    $('.vp-skills').dataset.tone = tone;
  }

  // ---- 对话 ----
  const el = (tag, cls, text) => {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  };
  const svg = (name) => {
    const node = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', `#vp-i-${name}`);
    node.append(use);
    return node;
  };
  const ago = (s) => (s < 60 ? '刚刚' : s < 3600 ? `${Math.floor(s / 60)} 分钟前` : `${Math.floor(s / 3600)} 小时前`);

  function bubble(side, other, label, text, extra) {
    const item = el('div', `vp-msg ${side}${other ? ' other' : ''}`);
    const body = el('div', 'vp-bubble', text);
    if (extra) extra(body);
    item.append(el('span', 'vp-who', label), body);
    return item;
  }

  function sys(cls, iconName, text) {
    const chip = el('div', `vp-sys ${cls}`);
    if (iconName) chip.append(svg(iconName));
    chip.append(text.replace(/。$/, ''));
    return chip;
  }

  function renderChat() {
    if (!status) return;
    const items = [], resets = new Set(status.resets || []), name = robotName();
    let lastTime = '';
    for (const t of status.history) {
      const when = ago(t.ago_s);
      if (when !== lastTime) items.push(el('div', 'vp-time', when));
      lastTime = when;
      const other = !t.mine, who = other ? '其他人' : '你';
      if (t.heard === null) items.push(bubble('me', other, who, '…', (b) => b.classList.add('faint')));
      else if (t.heard === '') items.push(bubble('me', other, who, '（没听清，再说一次吧）', (b) => b.classList.add('faint')));
      else items.push(bubble('me', other, who, t.heard));
      const streaming = t.reply === null && t.id === status.live_id && status.state === 'thinking';
      const text = t.reply || t.live;  // 被打断时对话记录里可能没有这一轮的回答，就留着已经显示的部分
      if (text || streaming) {
        items.push(bubble('robot', other, name, text, streaming ? (b) => b.append(el('span', 'vp-cursor')) : null));
      }
      (t.lookups || []).forEach((p) => items.push(sys('pending', '', p + '…')));
      t.pending.forEach((p) => items.push(sys('pending', 'hand', p + '…')));
      t.tools.forEach((r) => {  // 失败的工具结果语音流水线会加「错误: 」前缀（voice_pipeline.cpp），用错误样式显示
        const failed = /^错误[:：]\s*/.exec(r);
        items.push(failed ? sys('error', '', r.slice(failed[0].length)) : sys('', 'hand', r));
      });
      if (t.interrupted) items.push(sys('', '', '已打断'));
      if (t.error) items.push(sys('error', '', '出错：' + t.error));
      if (resets.has(t.id)) items.push(el('div', 'vp-divider', '一段时间没人说话，机器人已忘掉之前的对话'));
    }
    if (!items.length) {
      const empty = el('p', 'vp-empty');
      empty.append('按住下面的按钮说话', el('br'), '比如“打个招呼”“你是谁”');
      items.push(empty);
    }
    box.replaceChildren(...items);
    if (stick) box.scrollTop = box.scrollHeight;
    $('.vp-note').textContent = status.history.length ? '往上翻看更早的' : '';
  }

  box.addEventListener('scroll', () => { stick = box.scrollHeight - box.scrollTop - box.clientHeight < 48; });
  new ResizeObserver(() => { if (stick) box.scrollTop = box.scrollHeight; }).observe(box);

  // ---- 和服务端的往来 ----
  async function loadInfo() {
    if (infoLoading) return;
    infoLoading = true;
    while (!infoLoaded) {
      try {
        const r = await api('info');
        if (r.ok) {
          const info = await r.json(), chip = (text) => el('span', 'vp-chip', text);
          if (info.actions && info.actions.length) {
            $('.vp-actions').replaceChildren(...info.actions.map(chip));
            $('.vp-sheet-actions').replaceChildren(...info.actions.map(chip));
            $('.vp-all').textContent = `全部 ${info.actions.length} 个`;
            $('.vp-skills').hidden = false;
          }
          $('.vp-examples').replaceChildren(...EXAMPLES.map(chip));
          infoLoaded = true;
          infoLoading = false;
          render();
          return;
        }
      } catch (_) { /* 重试 */ }
      await sleep(3000);
    }
  }

  async function poll() {
    let since = -1;
    for (;;) {
      if (!active) {
        await new Promise((ok) => { wake = ok; });
        since = -1;
      }
      try {
        const r = await api('status', 'GET', undefined, `&since=${since}`);
        if (r.status === 401) {  // 没配对或令牌变了：控制台重新配对后自动恢复
          denied = true;
          offline = false;
          render();
          await sleep(3000);
          continue;
        }
        if (!r.ok) throw new Error(String(r.status));
        status = await r.json();
        since = status.seq;
        maxHoldS = status.max_hold_s || maxHoldS;
        denied = false;
        offline = false;
        render();
      } catch (_) {
        offline = true;
        render();
        await sleep(2000);
      }
    }
  }

  window.addEventListener('operator-status', (e) => {
    op = e.detail;
    if (active) renderAction();
  });

  $('.vp-more').onclick = () => { document.body.dataset.voiceSheet = 'open'; };
  $('.vp-close').onclick = backdrop.onclick = () => { document.body.dataset.voiceSheet = ''; };

  $('.vp-enable').onclick = async () => {
    if (!window.isSecureContext || !navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      show('这个浏览器不能录音，请换用微信或系统浏览器打开');
      return;
    }
    try {
      // ?raw=1 关掉浏览器的回声消除、降噪和自动增益：有的手机降噪会把人声几乎全部抹成静音
      const processing = new URLSearchParams(location.search).get('raw') !== '1';
      const stream = await navigator.mediaDevices.getUserMedia(
        {audio: {echoCancellation: processing, noiseSuppression: processing, autoGainControl: processing}});
      ctx = new (window.AudioContext || window.webkitAudioContext)();
      await ctx.resume();
      const source = ctx.createMediaStreamSource(stream);
      const proc = ctx.createScriptProcessor(4096, 1, 1);
      proc.onaudioprocess = (e) => {
        const block = new Float32Array(e.inputBuffer.getChannelData(0));
        if (recording) {
          chunks.push(block);
        } else {
          ring.push(block);
          if (ring.length > PRE_ROLL) ring.shift();
        }
      };
      source.connect(proc);
      proc.connect(ctx.destination);  // 输出全是 0，接上是为了让部分浏览器持续回调
      show('');
    } catch (err) {
      show('没有拿到麦克风权限：' + err.name);
    }
  };

  async function press(e) {
    e.preventDefault();
    if (!ctx || pressing || talk.disabled) return;
    talk.setPointerCapture(e.pointerId);
    ctx.resume();
    pressing = true;
    recording = true;
    chunks = ring.slice();
    startedAt = Date.now();
    show('');
    tick = setInterval(render, 250);
    holdTimer = setTimeout(() => release(null), maxHoldS * 1000);
    pressReq = api('press', 'POST').then((r) => r.json())
      .catch(() => ({ok: false, error: '网络不通，请稍后再试'}));
    const r = await pressReq;
    if (!r.ok) abort(r.error || '现在不能说话');
  }

  function abort(reason) {
    if (!pressing && !recording) return;
    recording = false;
    pressing = false;
    clearTimeout(holdTimer);
    clearInterval(tick);
    show(reason, 2500);
  }

  async function release(e) {
    if (e) e.preventDefault();
    if (!pressing) return;
    const held = Date.now() - startedAt;
    clearTimeout(holdTimer);
    clearInterval(tick);
    pressing = false;
    // 等占用话筒的请求先回来，release / 上传不会跑到它前面
    if (!(await pressReq).ok) return;
    if (held < 400) {
      recording = false;
      render();
      api('release', 'POST');
      show('按住说话，说完再松手', 2000);
      return;
    }
    await sleep(TAIL_MS);
    if (!recording) return;  // 期间被服务端拒绝
    recording = false;
    const wav = encodeWav(downsample(chunks, ctx.sampleRate, 16000), 16000);
    show('发送中…');
    stick = true;  // 自己刚说的话要看到
    try {
      const r = await (await api('audio', 'POST', wav)).json();
      if (r.ok) show(''); else show(r.error, 2500);
    } catch (err) {
      show('发送失败，请再试一次', 2500);
    }
  }

  talk.addEventListener('pointerdown', press);
  talk.addEventListener('pointerup', release);
  talk.addEventListener('pointercancel', release);
  talk.addEventListener('contextmenu', (e) => e.preventDefault());

  // 控制台切换「控制 | 语音」时调用；切走时正在录的直接放弃
  function setActive(on) {
    active = !!on;
    root.hidden = !active;
    if (!active) {
      if (pressing || recording) {
        abort('');
        api('release', 'POST');
      }
      document.body.dataset.voiceSheet = '';
      return;
    }
    if (wake) {
      wake();
      wake = null;
    }
    loadInfo();
    lastAction = '';
    render();
  }

  window.voicePanel = {setActive};
  window.dispatchEvent(new Event('voice-panel-ready'));
  poll();

  function downsample(parts, from, to) {
    let length = 0;
    parts.forEach((p) => { length += p.length; });
    const input = new Float32Array(length);
    let offset = 0;
    parts.forEach((p) => { input.set(p, offset); offset += p.length; });
    if (from === to) return input;
    const ratio = from / to, out = new Float32Array(Math.floor(length / ratio));
    for (let i = 0; i < out.length; i++) {
      const begin = Math.floor(i * ratio), end = Math.min(Math.floor((i + 1) * ratio), length);
      let sum = 0;
      for (let j = begin; j < end; j++) sum += input[j];
      out[i] = sum / Math.max(1, end - begin);
    }
    return out;
  }

  function encodeWav(samples, rate) {
    const buffer = new ArrayBuffer(44 + samples.length * 2), view = new DataView(buffer);
    const text = (o, s) => { for (let i = 0; i < s.length; i++) view.setUint8(o + i, s.charCodeAt(i)); };
    text(0, 'RIFF'); view.setUint32(4, 36 + samples.length * 2, true); text(8, 'WAVEfmt ');
    view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
    view.setUint32(24, rate, true); view.setUint32(28, rate * 2, true); view.setUint16(32, 2, true);
    view.setUint16(34, 16, true); text(36, 'data'); view.setUint32(40, samples.length * 2, true);
    for (let i = 0; i < samples.length; i++) {
      const s = Math.max(-1, Math.min(1, samples[i]));
      view.setInt16(44 + i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true);
    }
    return new Blob([view], {type: 'audio/wav'});
  }
})();
