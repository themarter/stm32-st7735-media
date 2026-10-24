'use strict';

const $ = (id) => document.getElementById(id);

const ui = {
  link: $('link'), linkLabel: $('link-label'), linkDetail: $('link-detail'),
  connect: $('connect'),
  drop: $('drop'), file: $('file'), fileInfo: $('file-info'),
  canvas: $('canvas'), frameTag: $('frame-tag'),
  statSize: $('stat-size'), statBytes: $('stat-bytes'), statEta: $('stat-eta'),
  resolution: $('resolution'), baud: $('baud'),
  send: $('send'), bar: $('bar'), status: $('status'), log: $('log')
};

const ctx = ui.canvas.getContext('2d', { willReadFrequently: true });

const HEADER_SIZE = 9;
const WRITE_CHUNK = 8192;
const SYNC = [0xA5, 0x5A];

const state = {
  image: null,
  width: 128,
  height: 160,
  rotation: 0,
  pixels: null,
  port: null,
  writer: null,
  reader: null,
  baud: 921600,
  sending: false,
  ackResolve: null
};

/* ---------- 小工具 ---------- */

let logStarted = false;

function log(message) {
  if (!logStarted) { ui.log.textContent = ''; logStarted = true; }
  const stamp = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  ui.log.textContent += (ui.log.textContent ? '\n' : '') + '[' + stamp + '] ' + message;
  ui.log.scrollTop = ui.log.scrollHeight;
}

function setStatus(text, kind) {
  ui.status.textContent = text;
  ui.status.className = 'status' + (kind ? ' ' + kind : '');
}

function setLink(mode, label, detail) {
  ui.link.dataset.state = mode;
  ui.linkLabel.textContent = label;
  if (detail !== undefined) { ui.linkDetail.textContent = detail; }
}

function progress(percent) {
  ui.bar.style.width = Math.max(0, Math.min(100, percent)) + '%';
}

function bytesText(n) {
  return n.toLocaleString('en-US') + ' B';
}

/* ---------- 画布 ---------- */

function paintEmpty() {
  ctx.fillStyle = '#080d0e';
  ctx.fillRect(0, 0, state.width, state.height);
  ctx.fillStyle = 'rgba(255,255,255,0.14)';
  ctx.font = '600 9px ui-monospace, monospace';
  ctx.textAlign = 'center';
  ctx.fillText('NO SIGNAL', state.width / 2, state.height / 2 + 3);
  state.pixels = null;
}

function render() {
  const w = state.width;
  const h = state.height;
  const img = state.image;

  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);

  const sw = img.naturalWidth || img.width;
  const sh = img.naturalHeight || img.height;
  if (!sw || !sh) { return; }

  /* cover：等比放大到铺满，超出的部分裁掉。屏幕就这么大，留黑边没意义。 */
  const scale = Math.max(w / sw, h / sh);
  const dw = sw * scale;
  const dh = sh * scale;

  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(img, (w - dw) / 2, (h - dh) / 2, dw, dh);

  pack();
  updateStats();
}

function pack() {
  const w = state.width;
  const h = state.height;
  const rgba = ctx.getImageData(0, 0, w, h).data;
  const out = new Uint8Array(w * h * 2);

  for (let s = 0, d = 0; s < rgba.length; s += 4, d += 2) {
    const r = rgba[s] & 0xF8;
    const g = rgba[s + 1] & 0xFC;
    const b = rgba[s + 2] & 0xF8;
    const v = (r << 8) | (g << 3) | (b >> 3);
    out[d] = v >> 8;
    out[d + 1] = v & 0xFF;
  }
  state.pixels = out;
}

/* ---------- 布局与统计 ---------- */

function layout() {
  const size = ui.resolution.value.split('x').map(Number);
  const swap = state.rotation % 2 === 1;

  state.width = swap ? size[1] : size[0];
  state.height = swap ? size[0] : size[1];

  ui.canvas.width = state.width;
  ui.canvas.height = state.height;
  ui.canvas.style.aspectRatio = state.width + ' / ' + state.height;

  /* 预览按「宽度优先、高度封顶」放大，小屏模组也不会被拉成一条 */
  const scale = Math.min(178 / state.width, 262 / state.height);
  ui.canvas.style.width = Math.round(state.width * scale) + 'px';
  ui.canvas.style.height = Math.round(state.height * scale) + 'px';

  const label = state.width + ' × ' + state.height;
  ui.frameTag.textContent = label;
  ui.statSize.textContent = label;

  if (state.image) { render(); } else { paintEmpty(); updateStats(); }
}

function updateStats() {
  if (!state.pixels) {
    ui.statBytes.textContent = '—';
    ui.statEta.textContent = '—';
    refreshSend();
    return;
  }

  const total = HEADER_SIZE + state.pixels.length;
  ui.statBytes.textContent = bytesText(total);

  /* 串口一字节 10 位（8 数据 + 起始 + 停止），再加 12% 余量 */
  const baud = Number(ui.baud.value);
  const seconds = total * 10 / baud * 1.12;
  ui.statEta.textContent = seconds < 1
    ? Math.round(seconds * 1000) + ' ms'
    : seconds.toFixed(seconds < 10 ? 1 : 0) + ' s';

  refreshSend();
}

function refreshSend() {
  ui.send.disabled = !(state.pixels && state.writer && !state.sending);
}

/* ---------- 图片 ---------- */

function decodeImage(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => { URL.revokeObjectURL(url); resolve(img); };
    img.onerror = () => { URL.revokeObjectURL(url); reject(new Error('浏览器解不开这个格式')); };
    img.src = url;
  });
}

async function useFile(file) {
  if (!file) { return; }
  if (file.type && !file.type.startsWith('image/')) {
    log('不是图片：' + (file.name || file.type));
    setStatus('这里只收图片。', 'err');
    return;
  }

  try {
    const img = await decodeImage(file);
    state.image = img;
    ui.fileInfo.textContent = file.name + ' · ' + (img.naturalWidth || img.width) + '×' + (img.naturalHeight || img.height);
    log('载入 ' + file.name + '。');
    if (file.type === 'image/gif') { log('GIF 只取第一帧——这个版本不做动画。'); }
    layout();
    setStatus('已经转成 RGB565，可以发送。');
  } catch (err) {
    log('解码失败：' + err.message);
    setStatus('这张图读不了，换 PNG 或 JPEG 试试。', 'err');
  }
}

/* ---------- 串口 ---------- */

async function readLoop() {
  const tail = [];

  try {
    while (state.reader) {
      const result = await state.reader.read();
      if (result.done) { break; }
      const value = result.value;
      if (!value) { continue; }

      for (let i = 0; i < value.length; i++) {
        tail.push(value[i]);
        if (tail.length > 3) { tail.shift(); }
        if (tail.length === 3 && tail[0] === 0x5A && tail[1] === 0xA5) {
          const status = tail[2];
          tail.length = 0;
          if (state.ackResolve) {
            const resolve = state.ackResolve;
            state.ackResolve = null;
            resolve(status);
          } else {
            log('设备主动回执：status=' + status);
          }
        }
      }
    }
  } catch (err) {
    if (state.port) { log('串口读取中断：' + err.message); }
  }
}

function waitAck(ms) {
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      state.ackResolve = null;
      resolve(null);
    }, ms);
    state.ackResolve = (status) => {
      clearTimeout(timer);
      state.ackResolve = null;
      resolve(status);
    };
  });
}

async function toggleConnect() {
  if (state.port) { await disconnect(); return; }

  if (!('serial' in navigator)) {
    log('这个浏览器没有 Web Serial。换 Chrome / Edge，并用 https 或 localhost 打开。');
    setStatus('当前浏览器不支持串口。', 'err');
    return;
  }

  try {
    const port = await navigator.serial.requestPort();
    const baud = Number(ui.baud.value);
    await port.open({ baudRate: baud });

    state.port = port;
    state.baud = baud;
    state.writer = port.writable.getWriter();
    state.reader = port.readable.getReader();
    readLoop();

    ui.connect.textContent = '断开';
    setLink('on', '串口已连接', baud + ' · 8-N-1');
    log('串口打开：' + baud + ' 8-N-1。');
    setStatus(state.pixels ? '可以发送了。' : '先选一张图。');
    refreshSend();
  } catch (err) {
    if (err && err.name === 'NotFoundError') {
      log('没有选择串口设备，已取消。');
    } else {
      log('连接失败：' + (err && err.message ? err.message : String(err)));
      setStatus('串口打不开，多半是被别的程序占着。', 'err');
    }
    await disconnect();
  }
}

async function disconnect() {
  const hadPort = !!state.port;

  try { if (state.reader) { await state.reader.cancel(); state.reader.releaseLock(); } } catch (err) { /* 已断开 */ }
  try { if (state.writer) { state.writer.releaseLock(); } } catch (err) { /* 已释放 */ }
  try { if (state.port) { await state.port.close(); } } catch (err) { /* 已关闭 */ }

  state.port = null;
  state.writer = null;
  state.reader = null;
  state.ackResolve = null;

  ui.connect.textContent = '连接串口';
  setLink('idle', '串口未连接', '需要 Chrome 或 Edge');
  refreshSend();
  if (hadPort) { log('串口已断开。'); }
}

/* ---------- 协议 ---------- */

function buildHeader() {
  const header = new Uint8Array(HEADER_SIZE);

  header[0] = SYNC[0];
  header[1] = SYNC[1];
  header[2] = 0x01;                       /* 唯一的命令：显示图片 */
  header[3] = state.width & 0xFF;
  header[4] = (state.width >> 8) & 0xFF;
  header[5] = state.height & 0xFF;
  header[6] = (state.height >> 8) & 0xFF;
  header[7] = state.rotation & 0x03;

  let sum = 0;
  for (let i = 0; i < HEADER_SIZE - 1; i++) { sum = (sum + header[i]) & 0xFF; }
  header[HEADER_SIZE - 1] = sum;

  return header;
}

async function send() {
  if (state.sending || !state.pixels || !state.writer) { return; }

  state.sending = true;
  refreshSend();
  ui.send.textContent = '发送中…';
  setLink('busy', '正在发送', '别拔线');
  setStatus('开始发送…');

  const header = buildHeader();
  const body = state.pixels;
  const total = HEADER_SIZE + body.length;
  const started = performance.now();
  let written = 0;

  try {
    await state.writer.write(header);
    written = HEADER_SIZE;

    for (let offset = 0; offset < body.length; offset += WRITE_CHUNK) {
      const end = Math.min(offset + WRITE_CHUNK, body.length);
      await state.writer.write(body.subarray(offset, end));
      written = HEADER_SIZE + end;

      const elapsed = (performance.now() - started) / 1000;
      progress(written / total * 100);
      setStatus(bytesText(written) + ' / ' + bytesText(total) +
        (elapsed > 0.15 ? ' · ' + (written / 1024 / elapsed).toFixed(0) + ' KB/s' : ''));
    }

    const elapsed = (performance.now() - started) / 1000;
    log('送出 ' + bytesText(total) + '，用时 ' + elapsed.toFixed(2) + ' s' +
      (elapsed > 0.15 ? '，平均 ' + (total / 1024 / elapsed).toFixed(1) + ' KB/s' : '') + '。');

    setStatus('字节发完了，等屏幕回执…');
    const ack = await waitAck(1500);

    if (ack === 0x00) {
      progress(100);
      setStatus('屏幕确认收到，画面应该已经刷上去了。', 'ok');
      log('设备回执 OK：整帧写屏完成。');
    } else if (ack === 0x01) {
      setStatus('固件说尺寸超出面板，像素被丢了。检查分辨率。', 'err');
      log('设备回执 BAD_SIZE。');
    } else if (ack === null) {
      progress(100);
      setStatus('字节全发出去了，但没等到回执。先看上电图案在不在。');
      log('没有回执：固件可能是旧版，或者 STM32 的 TX 到 USB-TTL 没接。');
    } else {
      progress(100);
      setStatus('设备回了未知状态 ' + ack + '。');
      log('未知回执：' + ack);
    }
  } catch (err) {
    progress(0);
    setStatus('发送中断：' + err.message, 'err');
    log('发送失败：' + err.message);
  } finally {
    state.sending = false;
    ui.send.textContent = '发送到屏幕';
    refreshSend();
    if (state.port) { setLink('on', '串口已连接', state.baud + ' · 8-N-1'); }
  }
}

/* ---------- 事件 ---------- */

ui.connect.addEventListener('click', toggleConnect);
ui.send.addEventListener('click', send);

ui.file.addEventListener('change', () => {
  useFile(ui.file.files && ui.file.files[0]);
});

['dragenter', 'dragover'].forEach((type) => {
  ui.drop.addEventListener(type, (event) => {
    event.preventDefault();
    ui.drop.classList.add('dragging');
  });
});

['dragleave', 'drop'].forEach((type) => {
  ui.drop.addEventListener(type, (event) => {
    event.preventDefault();
    ui.drop.classList.remove('dragging');
  });
});

ui.drop.addEventListener('drop', (event) => {
  const files = event.dataTransfer && event.dataTransfer.files;
  if (files && files[0]) { useFile(files[0]); }
});

ui.resolution.addEventListener('change', layout);

document.querySelectorAll('.rot button').forEach((button) => {
  button.addEventListener('click', () => {
    state.rotation = Number(button.dataset.rot);
    document.querySelectorAll('.rot button').forEach((other) => {
      other.classList.toggle('on', other === button);
    });
    layout();
  });
});

ui.baud.addEventListener('change', () => {
  updateStats();
  if (state.port) {
    log('波特率改成 ' + ui.baud.value + ' 了，断开重连才生效。');
    setStatus('波特率改动需要断开重连。');
  }
});

if ('serial' in navigator) {
  navigator.serial.addEventListener('disconnect', () => {
    if (state.port) {
      log('设备被拔掉了。');
      disconnect();
    }
  });
}

layout();
setStatus('先选一张图。');
