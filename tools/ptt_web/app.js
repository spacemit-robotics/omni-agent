// Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
// SPDX-License-Identifier: Apache-2.0
//
// 按住说话：按下时向服务端占用话筒（同时打断机器人），录音，松手后转成 16 kHz 单声道 wav 上传。
// 对话窗口显示最近的对话（所有人的），机器人回答边生成边显示；动作清单来自服务端 api/info。
'use strict';

const $ = (id) => document.getElementById(id);
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
const api = (path) => `api/${path}?client=${client}`;

const PRE_ROLL = 3;       // 按下前保留的缓冲块数（4096 点一块，约 0.25 s），防止吃掉第一个字
const TAIL_MS = 250;      // 松手后再录一点，防止吃掉最后一个字
const EXAMPLES = ['你是谁？', '你能做什么？', '讲个笑话', '今天适合做什么？'];
let ctx = null, chunks = [], ring = [], recording = false, pressing = false, startedAt = 0, holdTimer = null, tick = null;
let maxHoldS = 15, status = null, local = '', localTimer = null, pressReq = null, offline = false;
let stick = true;  // 对话停在最底下：来新消息、窗口变矮（动作清单加载出来）都跟到底；往上翻看历史时不动

// 从机器人控制台跳过来时带 ?back=<控制台地址>：显示「控制模式」切回去。访客扫码进来没有这个参数，不显示。
const back = (() => {
  const raw = new URLSearchParams(location.search).get('back') || storage(sessionStorage, 'ptt_back') || '';
  try {
    const url = new URL(raw);
    if (url.protocol === 'http:' || url.protocol === 'https:') return storage(sessionStorage, 'ptt_back', url.href);
  } catch (_) { /* 不是网址 */ }
  return '';
})();
if (back) {
  $('mode-switch').hidden = false;
  $('to-control').href = back;
  $('product').textContent = '机器人控制台';
}

function show(text, ms) {
  local = text;
  clearTimeout(localTimer);
  if (ms) localTimer = setTimeout(() => { local = ''; render(); }, ms);
  render();
}

const liveTurn = () => status && status.history.find((t) => t.id === status.live_id);

// 现在按不了：没准备好、别人按着、机器人在回答别人（和服务端 press 的拒绝条件一致）
const blocked = () => !status || !status.alive || !status.ready || status.holder === 'other' || status.answering_other;

function statusText() {
  if (recording) {
    const s = Math.min(maxHoldS, Math.floor((Date.now() - startedAt) / 1000));
    return `正在录音 ${s} s / ${maxHoldS} s，松手发送`;
  }
  if (local) return local;
  if (offline) return '连不上机器人，正在重试…';
  if (!status) return '正在连接…';
  if (!status.alive) return '机器人服务已停止';
  if (!status.ready) return '机器人正在启动…';
  if (status.holder === 'other') return '别人正在说话，请稍等';
  const turn = liveTurn(), mine = !!turn && turn.mine;
  if (status.state === 'recognizing') return mine ? '正在听你说了什么…' : '正在听别人说的话…';
  if (status.state === 'thinking') return mine ? '机器人正在回答 · 按住可打断' : '机器人正在回答别人，请稍等';
  return ctx ? '按住说话，松手发送' : '首次使用要允许浏览器使用麦克风';
}

function render() {
  const busy = blocked() && !recording;
  document.body.dataset.state = !ctx ? 'start' : recording ? 'rec' : busy ? 'busy'
    : status && status.state !== 'idle' ? 'answer' : 'idle';
  $('status').textContent = statusText();
  $('talk').disabled = busy;
  $('talk').classList.toggle('rec', recording);
  $('talk-label').textContent = recording ? '松手发送' : busy ? '请稍等' : '按住说话';
  const online = !offline && !!status && status.alive && status.ready;
  $('connection').textContent = online ? '已连接' : offline ? '已断开' : '连接中';
  $('connection').classList.toggle('online', online);
  renderChat();
}

const el = (tag, cls, text) => {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
};
const icon = (name) => {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  use.setAttribute('href', `#i-${name}`);
  svg.append(use);
  return svg;
};
const ago = (s) => (s < 60 ? '刚刚' : s < 3600 ? `${Math.floor(s / 60)} 分钟前` : `${Math.floor(s / 3600)} 小时前`);

function bubble(side, other, label, text, extra) {
  const box = el('div', `msg ${side}${other ? ' other' : ''}`);
  const body = el('div', 'bubble', text);
  if (extra) extra(body);
  box.append(el('span', 'who', label), body);
  return box;
}

function sys(cls, iconName, text) {
  const chip = el('div', `sys ${cls}`);
  if (iconName) chip.append(icon(iconName));
  chip.append(text.replace(/。$/, ''));
  return chip;
}

function renderChat() {
  const box = $('chat');
  if (!status) return;
  const items = [], resets = new Set(status.resets || []);
  let lastTime = '';
  for (const t of status.history) {
    const when = ago(t.ago_s);
    if (when !== lastTime) items.push(el('div', 'time', when));
    lastTime = when;
    const other = !t.mine, who = other ? '其他人' : '你';
    if (t.heard === null) items.push(bubble('me', other, who, '…', (b) => b.classList.add('faint')));
    else if (t.heard === '') items.push(bubble('me', other, who, '（没听清，再说一次吧）', (b) => b.classList.add('faint')));
    else items.push(bubble('me', other, who, t.heard));
    const streaming = t.reply === null && t.id === status.live_id && status.state === 'thinking';
    const text = t.reply || t.live;  // 被打断时对话记录里可能没有这一轮的回答，就留着已经显示的部分
    if (text || streaming) {
      items.push(bubble('robot', other, $('robot-name').textContent, text,
        streaming ? (b) => b.append(el('span', 'cursor')) : null));
    }
    t.pending.forEach((p) => items.push(sys('pending', 'hand', p + '…')));
    t.tools.forEach((r) => items.push(sys('', 'hand', r)));
    if (t.interrupted) items.push(sys('', '', '已打断'));
    if (t.error) items.push(sys('error', '', '出错：' + t.error));
    if (resets.has(t.id)) items.push(el('div', 'divider', '一段时间没人说话，机器人已忘掉之前的对话'));
  }
  if (!items.length) {
    const empty = el('p', 'empty-chat');
    empty.append('按住下面的按钮说话', el('br'), '比如“打个招呼”“你是谁”');
    items.push(empty);
  }
  box.replaceChildren(...items);
  if (stick) box.scrollTop = box.scrollHeight;
  $('chat-note').textContent = status.history.length ? '往上翻看更早的' : '';
}

async function loadInfo() {
  for (;;) {
    try {
      const r = await fetch(api('info'), {cache: 'no-store'});
      if (r.ok) {
        const info = await r.json();
        $('robot-name').textContent = info.robot || '机器人';
        const chip = (text) => el('span', 'chip', text);
        if (info.actions && info.actions.length) {
          $('actions').replaceChildren(...info.actions.map(chip));
          $('sheet-actions').replaceChildren(...info.actions.map(chip));
          $('all-label').textContent = `全部 ${info.actions.length} 个`;
          $('skills').hidden = false;
        }
        $('sheet-examples').replaceChildren(...EXAMPLES.map(chip));
        render();
        return;
      }
      if (r.status === 404) return;
    } catch (_) { /* 重试 */ }
    await new Promise((ok) => setTimeout(ok, 3000));
  }
}

async function poll() {
  let since = -1;
  for (;;) {
    try {
      const r = await fetch(`${api('status')}&since=${since}`, {cache: 'no-store'});
      if (r.status === 404) { show('链接已失效，请重新扫码'); return; }
      status = await r.json();
      since = status.seq;
      maxHoldS = status.max_hold_s || maxHoldS;
      offline = false;
      render();
    } catch (err) {
      offline = true;
      render();
      await new Promise((ok) => setTimeout(ok, 2000));
    }
  }
}

$('chat').addEventListener('scroll', () => {
  const box = $('chat');
  stick = box.scrollHeight - box.scrollTop - box.clientHeight < 48;
});
new ResizeObserver(() => { if (stick) $('chat').scrollTop = $('chat').scrollHeight; }).observe($('chat'));

$('all').onclick = () => { document.body.dataset.sheet = 'open'; };
$('close-sheet').onclick = $('sheet-backdrop').onclick = () => { document.body.dataset.sheet = ''; };

$('enable').onclick = async () => {
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
  if (!ctx || pressing || $('talk').disabled) return;
  $('talk').setPointerCapture(e.pointerId);
  ctx.resume();
  pressing = true;
  recording = true;
  chunks = ring.slice();
  startedAt = Date.now();
  show('');
  tick = setInterval(render, 250);
  holdTimer = setTimeout(() => release(null), maxHoldS * 1000);
  pressReq = fetch(api('press'), {method: 'POST'}).then((r) => r.json())
    .catch(() => ({ok: false, error: '网络不通，请稍后再试'}));
  const r = await pressReq;
  if (!r.ok) abort(r.error);
}

function stopRecording() {
  clearTimeout(holdTimer);
  clearInterval(tick);
  pressing = false;
  render();
}

function abort(reason) {
  if (!pressing && !recording) return;
  recording = false;
  stopRecording();
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
    fetch(api('release'), {method: 'POST'});
    show('按住说话，说完再松手', 2000);
    return;
  }
  await new Promise((ok) => setTimeout(ok, TAIL_MS));
  if (!recording) return;  // 期间被服务端拒绝
  recording = false;
  const wav = encodeWav(downsample(chunks, ctx.sampleRate, 16000), 16000);
  show('发送中…');
  stick = true;  // 自己刚说的话要看到
  try {
    const r = await (await fetch(api('audio'), {method: 'POST', headers: {'Content-Type': 'audio/wav'}, body: wav})).json();
    if (r.ok) show(''); else show(r.error, 2500);
  } catch (err) {
    show('发送失败，请再试一次', 2500);
  }
}

const talk = $('talk');
talk.addEventListener('pointerdown', press);
talk.addEventListener('pointerup', release);
talk.addEventListener('pointercancel', release);
talk.addEventListener('contextmenu', (e) => e.preventDefault());
render();
loadInfo();
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
