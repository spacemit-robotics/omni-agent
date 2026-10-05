// Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
// SPDX-License-Identifier: Apache-2.0
//
// 按住说话：按下时向服务端占用话筒（同时打断机器人），录音，松手后转成 16 kHz 单声道 wav 上传。
'use strict';

const $ = (id) => document.getElementById(id);
const client = (() => {
  let id = localStorage.getItem('ptt_client');
  if (!id) {
    id = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, '0')).join('');
    localStorage.setItem('ptt_client', id);
  }
  return id;
})();
const api = (path) => `api/${path}?client=${client}`;

const PRE_ROLL = 3;       // 按下前保留的缓冲块数（4096 点一块，约 0.25 s），防止吃掉第一个字
const TAIL_MS = 250;      // 松手后再录一点，防止吃掉最后一个字
let ctx = null, chunks = [], ring = [], recording = false, pressing = false, startedAt = 0, holdTimer = null;
let maxHoldS = 15, status = null, local = '', pressReq = null;

function show(text) { local = text; render(); }

function render() {
  const talk = $('talk');
  let text = local;
  if (!text && status) {
    if (!status.alive) text = '机器人服务已停止';
    else if (!status.ready) text = '机器人正在启动…';
    else if (status.holder === 'other') text = '别人正在说话，请稍等';
    else if (status.state === 'recognizing') text = '正在听你说了什么…';
    else if (status.state === 'thinking') text = status.turn && status.turn.mine ? '机器人正在回答' : '机器人正在回答别人';
    else text = '按住按钮说话';
  }
  $('status').textContent = text;
  talk.disabled = !!status && (!status.ready || status.holder === 'other') && !recording;

  const box = $('turn');
  box.textContent = '';
  const turn = status && status.turn;
  if (!turn) return;
  const add = (cls, line) => { const d = document.createElement('div'); d.className = cls; d.textContent = line; box.append(d); };
  if (turn.heard !== null) add('me', (turn.mine ? '你说：' : '有人说：') + (turn.heard || '（没听清，再说一次吧）'));
  if (turn.reply) add('robot', '机器人：' + turn.reply);
  (turn.tools || []).forEach((t) => add('action', '动作：' + t));
  if (turn.error) add('action', '出错：' + turn.error);
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
      render();
    } catch (err) {
      await new Promise((ok) => setTimeout(ok, 2000));
    }
  }
}

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
    $('enable').hidden = true;
    $('talk').hidden = false;
    show('');
  } catch (err) {
    show('没有拿到麦克风权限：' + err.name);
  }
};

async function press(e) {
  e.preventDefault();
  if (!ctx || pressing) return;
  $('talk').setPointerCapture(e.pointerId);
  ctx.resume();
  pressing = true;
  recording = true;
  chunks = ring.slice();
  startedAt = Date.now();
  $('talk').classList.add('rec');
  $('talk').textContent = '松手发送';
  show('正在录音…');
  holdTimer = setTimeout(() => release(null), maxHoldS * 1000);
  pressReq = fetch(api('press'), {method: 'POST'}).then((r) => r.json())
    .catch(() => ({ok: false, error: '网络不通，请稍后再试'}));
  const r = await pressReq;
  if (!r.ok) abort(r.error);
}

function stopRecording() {
  clearTimeout(holdTimer);
  pressing = false;
  $('talk').classList.remove('rec');
  $('talk').textContent = '按住说话';
}

function abort(reason) {
  if (!pressing && !recording) return;
  recording = false;
  stopRecording();
  show(reason);
  setTimeout(() => show(''), 2500);
}

async function release(e) {
  if (e) e.preventDefault();
  if (!pressing) return;
  stopRecording();
  const held = Date.now() - startedAt;
  // 等占用话筒的请求先回来，release / 上传不会跑到它前面
  if (!(await pressReq).ok) return;
  if (held < 400) {
    recording = false;
    fetch(api('release'), {method: 'POST'});
    show('按住说话，说完再松手');
    setTimeout(() => show(''), 2000);
    return;
  }
  await new Promise((ok) => setTimeout(ok, TAIL_MS));
  if (!recording) return;  // 期间被服务端拒绝
  recording = false;
  const wav = encodeWav(downsample(chunks, ctx.sampleRate, 16000), 16000);
  show('发送中…');
  try {
    const r = await (await fetch(api('audio'), {method: 'POST', headers: {'Content-Type': 'audio/wav'}, body: wav})).json();
    show(r.ok ? '' : r.error);
    if (!r.ok) setTimeout(() => show(''), 2500);
  } catch (err) {
    show('发送失败，请再试一次');
    setTimeout(() => show(''), 2500);
  }
}

const talk = $('talk');
talk.addEventListener('pointerdown', press);
talk.addEventListener('pointerup', release);
talk.addEventListener('pointercancel', release);
talk.addEventListener('contextmenu', (e) => e.preventDefault());
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
