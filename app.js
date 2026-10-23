const $ = (id) => document.getElementById(id);
const ui = {
  connect:$("connect"), connectionLabel:$("connection-label"), protocolState:$("protocol-state"),
  dropZone:$("drop-zone"), input:$("media-input"), preview:$("preview"), fit:$("fit-mode"),
  threshold:$("threshold"), thresholdValue:$("threshold-value"), invert:$("invert"), fps:$("fps"),
  frameCount:$("frame-count"), outputSize:$("output-size"), send:$("send"), stop:$("stop"),
  endSession:$("end-session"), export:$("export"), progressBar:$("progress-bar"),
  progressLabel:$("progress-label"), progressPercent:$("progress-percent"), log:$("diagnostic-log")
};
const ctx = ui.preview.getContext("2d", {willReadFrequently:true});
const WIDTH = 128, HEIGHT = 64, FRAME_BYTES = WIDTH * HEIGHT / 8;
const CHUNK_BYTES = 256, MAX_FRAMES = 120;
const CMD = {HELLO:1, FRAME_BEGIN:0x10, FRAME_END:0x11, SESSION_STOP:0x12};
const EVT = {ACK:0x80, HELLO:0x81};
const state = {
  selectedFile:null, grayFrames:[], frames:[], packageBytes:null, port:null, reader:null,
  writer:null, readTask:null, pending:new Map(), sequence:1, baud:115200, transportReady:false,
  streaming:false, stopRequested:false, sessionActive:false
};
let objectUrl = null;

function log(message) {
  const stamp = new Date().toLocaleTimeString();
  const current = ui.log.textContent;
  ui.log.textContent = (current.startsWith("连接设备后") ? "" : current + "\n") + "[" + stamp + "] " + message;
  ui.log.scrollTop = ui.log.scrollHeight;
}
function progress(value, message) {
  ui.progressBar.style.width = Math.max(0, Math.min(100, value)) + "%";
  ui.progressLabel.textContent = message;
  ui.progressPercent.textContent = Math.round(value) + "%";
}
function setConnection(connected, message) {
  ui.connectionLabel.textContent = message;
  document.querySelector(".connection").classList.toggle("connected", connected);
  ui.connect.textContent = connected ? "断开连接" : "连接 STM32";
}
function concat(parts) {
  const size = parts.reduce((sum, part) => sum + part.length, 0);
  const output = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) { output.set(part, offset); offset += part.length; }
  return output;
}
function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function cobsEncode(input) {
  const output = [0];
  let codeIndex = 0, code = 1;
  for (const byte of input) {
    if (byte === 0) {
      output[codeIndex] = code;
      code = 1; codeIndex = output.length; output.push(0);
    } else {
      output.push(byte);
      if (++code === 0xff) {
        output[codeIndex] = code;
        code = 1; codeIndex = output.length; output.push(0);
      }
    }
  }
  output[codeIndex] = code;
  return new Uint8Array(output);
}
function cobsDecode(input) {
  const output = [];
  for (let read = 0; read < input.length;) {
    const code = input[read++];
    if (!code || read + code - 1 > input.length) return null;
    for (let i = 1; i < code; i++) output.push(input[read++]);
    if (code < 0xff && read < input.length) output.push(0);
  }
  return new Uint8Array(output);
}
function packet(channel, sequence, payload) {
  const raw = new Uint8Array(3 + payload.length + 4);
  raw[0] = channel; raw[1] = sequence & 255; raw[2] = sequence >>> 8;
  raw.set(payload, 3);
  new DataView(raw.buffer).setUint32(raw.length - 4, crc32(raw.subarray(0, raw.length - 4)), true);
  const encoded = cobsEncode(raw);
  const output = new Uint8Array(encoded.length + 2);
  output[0] = 0; output.set(encoded, 1); output[output.length - 1] = 0;
  return output;
}
function handleWireFrame(encoded) {
  const raw = cobsDecode(encoded);
  if (!raw || raw.length < 7) return;
  const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  if (view.getUint32(raw.length - 4, true) !== crc32(raw.subarray(0, raw.length - 4))) {
    log("丢弃 CRC 校验失败的数据包"); return;
  }
  if (raw[0] !== 2) return;
  const seq = raw[1] | (raw[2] << 8);
  const payload = raw.subarray(3, raw.length - 4);
  const waiter = state.pending.get(seq);
  if (!waiter) return;
  state.pending.delete(seq);
  if (!payload.length) { waiter.reject(new Error("STM32 返回空事件")); return; }
  if (payload[0] === EVT.ACK) {
    if (payload.length < 2 || payload[1] !== 0) waiter.reject(new Error("STM32 拒绝数据，状态码 " + (payload[1] ?? "?")));
    else waiter.resolve(payload);
  } else if (payload[0] === EVT.HELLO) waiter.resolve(payload);
  else waiter.reject(new Error("未知设备事件 0x" + payload[0].toString(16)));
}
async function readSerialLoop() {
  let pending = [];
  try {
    while (state.reader) {
      const part = await state.reader.read();
      if (part.done) break;
      for (const byte of part.value) {
        if (byte === 0) {
          if (pending.length) handleWireFrame(new Uint8Array(pending));
          pending = [];
        } else if (pending.length < 400) pending.push(byte);
        else { pending = []; log("设备返回包超出长度，已丢弃"); }
      }
    }
  } catch (error) { if (state.port) log("串口接收结束：" + error.message); }
}
async function sendPacket(channel, payload, timeoutMs) {
  if (!state.writer) throw new Error("串口尚未连接");
  const seq = state.sequence++ & 0xffff;
  const response = new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      state.pending.delete(seq);
      reject(new Error("等待 STM32 确认超时（包序号 " + seq + "），请检查接收固件与串口接线"));
    }, timeoutMs || 3000);
    state.pending.set(seq, {
      resolve(value) { clearTimeout(timer); resolve(value); },
      reject(error) { clearTimeout(timer); reject(error); }
    });
  });
  try {
    await state.writer.write(packet(channel, seq, payload));
  } catch (error) {
    state.pending.delete(seq);
    throw error;
  }
  return response;
}
function u16(view, offset, value) { view.setUint16(offset, value, true); }
async function hello() {
  const reply = await sendPacket(0, new Uint8Array([CMD.HELLO]), 2500);
  if (reply[0] !== EVT.HELLO || reply.length < 6) throw new Error("设备没有返回 OLED STREAM 握手信息");
  const view = new DataView(reply.buffer, reply.byteOffset, reply.byteLength);
  const width = view.getUint16(1, true), height = view.getUint16(3, true), version = reply[5];
  if (width !== WIDTH || height !== HEIGHT) throw new Error("设备屏幕为 " + width + "×" + height + "，网页要求 128×64 SSD1306");
  state.transportReady = true;
  ui.protocolState.textContent = "OLED STREAM v" + version + " · 128×64 · 已握手";
  ui.send.disabled = state.frames.length === 0;
  ui.endSession.disabled = false;
  log("识别到 SSD1306 接收端，协议 v" + version + "，128×64 单色 I²C 屏幕");
}
async function connectSerial() {
  if (state.port) { await disconnectSerial(); return; }
  if (!("serial" in navigator)) {
    log("当前浏览器不支持 Web Serial。请使用最新版 Chrome/Edge，并通过 HTTPS 或 localhost 打开网页。");
    return;
  }
  try {
    state.port = await navigator.serial.requestPort();
    await state.port.open({baudRate:state.baud, bufferSize:1024});
    state.writer = state.port.writable.getWriter();
    state.reader = state.port.readable.getReader();
    state.readTask = readSerialLoop();
    setConnection(true, "串口已打开");
    log("串口已打开，波特率 " + state.baud + "。发送握手确认请求。");
    try { await hello(); }
    catch (error) {
      state.transportReady = false;
      ui.protocolState.textContent = "未识别 OLED 接收固件";
      log(error.message);
      log("网页可离线转换并导出 .oled；实时投屏要求在 STM32 上集成 firmware/oled_stream_rx.c。");
    }
  } catch (error) {
    log("连接失败：" + error.message);
    await closePort();
  }
  refreshButtons();
}
async function closePort() {
  state.transportReady = false;
  for (const waiter of state.pending.values()) waiter.reject(new Error("串口已断开"));
  state.pending.clear();
  const reader = state.reader, writer = state.writer, port = state.port;
  state.reader = state.writer = state.port = null;
  try { if (reader) { await reader.cancel(); reader.releaseLock(); } } catch {}
  try { if (writer) writer.releaseLock(); } catch {}
  try { if (port) await port.close(); } catch {}
  setConnection(false, "串口未连接");
  ui.protocolState.textContent = "等待 STM32 接收端";
  refreshButtons();
}
async function disconnectSerial() {
  if (state.streaming) { state.stopRequested = true; log("等待当前帧结束后断开串口"); }
  try {
    if (state.transportReady && state.writer && !state.streaming) {
      await sendPacket(0, new Uint8Array([CMD.SESSION_STOP]), 1200);
      state.sessionActive = false;
    }
  } catch (error) { log("退出投屏模式提示：" + error.message); }
  await closePort();
  log("串口已断开");
}
function fitSource(source) {
  ctx.fillStyle = "#000"; ctx.fillRect(0, 0, WIDTH, HEIGHT);
  const sourceW = source.displayWidth || source.videoWidth || source.naturalWidth || source.width;
  const sourceH = source.displayHeight || source.videoHeight || source.naturalHeight || source.height;
  if (!sourceW || !sourceH) throw new Error("媒体尺寸无效");
  const scale = ui.fit.value === "contain" ? Math.min(WIDTH/sourceW, HEIGHT/sourceH) : Math.max(WIDTH/sourceW, HEIGHT/sourceH);
  const destW = sourceW * scale, destH = sourceH * scale;
  ctx.drawImage(source, (WIDTH-destW)/2, (HEIGHT-destH)/2, destW, destH);
  const rgba = ctx.getImageData(0, 0, WIDTH, HEIGHT).data;
  const gray = new Uint8Array(WIDTH * HEIGHT);
  for (let src = 0, dst = 0; src < rgba.length; src += 4, dst++) {
    gray[dst] = Math.round((rgba[src] * 299 + rgba[src+1] * 587 + rgba[src+2] * 114) / 1000);
  }
  return gray;
}
function waitVideoFrame(video, time) {
  if (video.readyState >= 2 && Math.abs(video.currentTime-time) < 0.015) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { cleanup(); reject(new Error("视频抽帧超时")); }, 6000);
    function cleanup() { clearTimeout(timer); video.removeEventListener("seeked", done); video.removeEventListener("loadeddata", done); video.removeEventListener("error", failed); }
    function done() { cleanup(); resolve(); }
    function failed() { cleanup(); reject(new Error("视频解码失败")); }
    video.addEventListener("seeked", done, {once:true});
    video.addEventListener("loadeddata", done, {once:true});
    video.addEventListener("error", failed, {once:true});
    if (Math.abs(video.currentTime-time) >= 0.015) video.currentTime = time;
  });
}
async function decodeMedia(file) {
  if (file.type.startsWith("video/")) {
    if (objectUrl) URL.revokeObjectURL(objectUrl);
    objectUrl = URL.createObjectURL(file);
    const video = document.createElement("video");
    video.muted = true; video.playsInline = true; video.preload = "auto"; video.src = objectUrl;
    await new Promise((resolve, reject) => {
      video.onloadedmetadata = resolve;
      video.onerror = () => reject(new Error("无法读取该视频格式"));
    });
    if (video.readyState < 2) {
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("视频首帧加载超时")), 6000);
        video.addEventListener("loadeddata", () => {clearTimeout(timer); resolve();}, {once:true});
        video.addEventListener("error", () => {clearTimeout(timer); reject(new Error("视频首帧加载失败"));}, {once:true});
      });
    }
    const fps = Number(ui.fps.value);
    const duration = Math.min(Number.isFinite(video.duration) ? video.duration : MAX_FRAMES/fps, MAX_FRAMES/fps);
    const count = Math.max(1, Math.min(MAX_FRAMES, Math.floor(duration * fps)));
    const grayFrames = [];
    for (let i=0; i<count; i++) {
      const time = Math.min(Math.max(0, duration-0.001), i/fps);
      await waitVideoFrame(video, time);
      grayFrames.push({gray:fitSource(video), delay:Math.round(1000/fps)});
      progress(5 + 55 * (i+1)/count, "正在解析视频帧 " + (i+1) + "/" + count);
    }
    video.pause(); video.removeAttribute("src"); video.load();
    URL.revokeObjectURL(objectUrl); objectUrl = null;
    return grayFrames;
  }
  if (file.type === "image/gif" && "ImageDecoder" in window) {
    const decoder = new ImageDecoder({data:new Uint8Array(await file.arrayBuffer()), type:"image/gif", preferAnimation:true});
    try {
      await decoder.tracks.ready;
      const track = decoder.tracks.selectedTrack;
      const count = Math.min(MAX_FRAMES, track ? track.frameCount : 1);
      const grayFrames = [];
      for (let i=0; i<count; i++) {
        const result = await decoder.decode({frameIndex:i, completeFramesOnly:true});
        grayFrames.push({gray:fitSource(result.image), delay:Math.max(50,Math.min(2000,Math.round((result.image.duration || 250000)/1000)))});
        result.image.close();
        progress(5 + 55 * (i+1)/count, "正在解析 GIF 帧 " + (i+1) + "/" + count);
      }
      return grayFrames;
    } finally { decoder.close(); }
  }
  const image = new Image();
  const url = URL.createObjectURL(file);
  try {
    await new Promise((resolve, reject) => { image.onload = resolve; image.onerror = () => reject(new Error("无法解码图片")); image.src = url; });
    return [{gray:fitSource(image), delay:1000}];
  } finally { URL.revokeObjectURL(url); }
}
function packGray(gray) {
  const frame = new Uint8Array(FRAME_BYTES);
  const threshold = Number(ui.threshold.value), invert = ui.invert.checked;
  for (let y=0; y<HEIGHT; y++) {
    for (let x=0; x<WIDTH; x++) {
      let lit = gray[y*WIDTH+x] >= threshold;
      if (invert) lit = !lit;
      if (lit) frame[(y >> 3)*WIDTH+x] |= 1 << (y & 7);
    }
  }
  return frame;
}
function previewFrame(frame) {
  const image = ctx.createImageData(WIDTH, HEIGHT);
  for (let y=0; y<HEIGHT; y++) {
    for (let x=0; x<WIDTH; x++) {
      const bit = frame[(y >> 3)*WIDTH+x] & (1 << (y & 7));
      const v = bit ? 245 : 0, offset = (y*WIDTH+x)*4;
      image.data[offset] = v; image.data[offset+1] = v; image.data[offset+2] = v; image.data[offset+3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
}
function refreshEncodedFrames() {
  state.frames = state.grayFrames.map(item => ({pixels:packGray(item.gray), delay:item.delay}));
  if (state.frames.length) previewFrame(state.frames[0].pixels);
  state.packageBytes = state.frames.length ? makePackage(state.frames) : null;
  ui.frameCount.textContent = state.frames.length ? String(state.frames.length) + (state.frames.length >= MAX_FRAMES ? "（达到上限）" : "") : "—";
  ui.outputSize.textContent = state.packageBytes ? human(state.packageBytes.length) : "—";
  ui.export.disabled = !state.packageBytes;
  refreshButtons();
}
function makePackage(frames) {
  const bodies = [];
  for (const item of frames) {
    const frameHead = new Uint8Array(4);
    const view = new DataView(frameHead.buffer);
    u16(view, 0, item.delay);
    u16(view, 2, item.pixels.length);
    bodies.push(frameHead, item.pixels);
  }
  const body = concat(bodies);
  const header = new Uint8Array(24);
  header.set([79,76,69,68], 0); // OLED
  header[4] = 1; header[5] = ui.invert.checked ? 1 : 0;
  const view = new DataView(header.buffer);
  u16(view, 6, WIDTH); u16(view, 8, HEIGHT); u16(view, 10, frames.length); u16(view, 12, Number(ui.fps.value));
  view.setUint32(14, body.length, true); view.setUint32(18, crc32(body), true);
  header[22] = 1; // SSD1306 page layout, LSB is top pixel within each 8-pixel page
  return concat([header, body]);
}
function human(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + " KiB";
  return (bytes/(1024*1024)).toFixed(2) + " MiB";
}
async function prepareMedia(file) {
  if (!file) return;
  if (!file.type.startsWith("image/") && !file.type.startsWith("video/")) {
    log("只支持图片、GIF 或视频文件"); return;
  }
  state.selectedFile = file; state.grayFrames = []; state.frames = []; state.packageBytes = null;
  ui.frameCount.textContent = "解析中…"; ui.outputSize.textContent = "—";
  ui.export.disabled = true; ui.send.disabled = true; ui.endSession.disabled = true;
  progress(2, "正在读取媒体…");
  try {
    state.grayFrames = await decodeMedia(file);
    if (!state.grayFrames.length) throw new Error("未读取到媒体帧");
    refreshEncodedFrames();
    progress(100, "转换完成");
    log("已转换 " + file.name + " → 128×64 单色帧，共 " + state.frames.length + " 帧；每帧 1,024 字节");
  } catch (error) {
    state.grayFrames = []; state.frames = []; state.packageBytes = null;
    ui.frameCount.textContent = "失败"; ui.outputSize.textContent = "—";
    ui.export.disabled = true; ui.send.disabled = true;
    progress(0, "转换失败：" + error.message); log(error.message);
  }
}
function savePackage() {
  if (!state.packageBytes) return;
  const blob = new Blob([state.packageBytes], {type:"application/octet-stream"});
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  const name = (state.selectedFile ? state.selectedFile.name.replace(/\.[^.]+$/, "") : "media").replace(/[^a-zA-Z0-9._-]+/g, "-").slice(0,48) || "media";
  link.href = url; link.download = name + ".oled";
  document.body.appendChild(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  log("已导出 OLED 媒体包");
}
async function sendFrame(index, frameId) {
  const item = state.frames[index];
  const begin = new Uint8Array(9), beginView = new DataView(begin.buffer);
  begin[0] = CMD.FRAME_BEGIN; u16(beginView, 1, frameId); u16(beginView, 3, WIDTH); u16(beginView, 5, HEIGHT); u16(beginView, 7, item.delay);
  await sendPacket(0, begin, 3000);
  for (let offset=0; offset<item.pixels.length; offset+=CHUNK_BYTES) {
    const count = Math.min(CHUNK_BYTES, item.pixels.length-offset);
    const data = new Uint8Array(4+count), view = new DataView(data.buffer);
    u16(view, 0, frameId); u16(view, 2, offset);
    data.set(item.pixels.subarray(offset, offset+count), 4);
    await sendPacket(1, data, 3500);
    const packetProgress = (offset+count)/item.pixels.length;
    progress(packetProgress*100, "正在发送第 " + (index+1) + "/" + state.frames.length + " 帧 · " + (offset+count) + "/" + item.pixels.length + " B");
  }
  const end = new Uint8Array(3), endView = new DataView(end.buffer);
  end[0] = CMD.FRAME_END; u16(endView, 1, frameId);
  await sendPacket(0, end, 5000);
}
async function startStreaming() {
  if (!state.transportReady || !state.frames.length || state.streaming) return;
  state.streaming = true; state.stopRequested = false; state.sessionActive = true;
  ui.send.disabled = true; ui.stop.disabled = false; ui.export.disabled = true; ui.endSession.disabled = true;
  try {
    let frameId = 0, sent = 0;
    while (!state.stopRequested) {
      for (let i=0; i<state.frames.length && !state.stopRequested; i++) {
        const start = performance.now();
        await sendFrame(i, frameId++ & 0xffff);
        sent++;
        progress((i+1)/state.frames.length*100, "已确认第 " + (i+1) + "/" + state.frames.length + " 帧");
        if (state.frames.length === 1) { state.stopRequested = true; break; }
        const wait = state.frames[i].delay - (performance.now()-start);
        if (wait > 0) await new Promise(resolve => setTimeout(resolve, wait));
      }
    }
    log("投屏任务已停止，本次确认发送 " + sent + " 帧");
  } catch (error) {
    progress(0, "投屏失败：" + error.message);
    log("投屏失败：" + error.message);
  } finally {
    state.streaming = false;
    ui.stop.disabled = true; ui.export.disabled = !state.packageBytes; ui.endSession.disabled = !state.transportReady;
    refreshButtons();
  }
}
function stopStreaming() {
  state.stopRequested = true;
  ui.stop.disabled = true;
  log("已请求停止动画，屏幕保留最后一帧；STM32 仍保持投屏模式");
}
async function endSession() {
  if (!state.transportReady || state.streaming) return;
  try {
    await sendPacket(0, new Uint8Array([CMD.SESSION_STOP]), 2000);
    state.sessionActive = false;
    log("已退出投屏模式；STM32 可以恢复其他 OLED 页面");
  } catch (error) { log("退出投屏模式失败：" + error.message); }
  refreshButtons();
}
function refreshButtons() {
  ui.send.disabled = !state.transportReady || !state.frames.length || state.streaming;
  ui.stop.disabled = !state.streaming;
  ui.export.disabled = !state.packageBytes || state.streaming;
  ui.endSession.disabled = !state.transportReady || state.streaming;
}
ui.connect.addEventListener("click", connectSerial);
ui.input.addEventListener("change", () => prepareMedia(ui.input.files && ui.input.files[0]));
ui.export.addEventListener("click", savePackage);
ui.send.addEventListener("click", startStreaming);
ui.stop.addEventListener("click", stopStreaming);
ui.endSession.addEventListener("click", endSession);
ui.threshold.addEventListener("input", () => { ui.thresholdValue.textContent = ui.threshold.value; refreshEncodedFrames(); });
ui.invert.addEventListener("change", refreshEncodedFrames);
ui.fps.addEventListener("change", () => { state.grayFrames = state.grayFrames.map(item => ({gray:item.gray, delay:Math.round(1000/Number(ui.fps.value))})); refreshEncodedFrames(); });
ui.fit.addEventListener("change", () => { if (state.selectedFile) prepareMedia(state.selectedFile); });
["dragenter","dragover"].forEach(type => ui.dropZone.addEventListener(type, event => { event.preventDefault(); ui.dropZone.classList.add("dragging"); }));
["dragleave","drop"].forEach(type => ui.dropZone.addEventListener(type, event => { event.preventDefault(); ui.dropZone.classList.remove("dragging"); }));
ui.dropZone.addEventListener("drop", event => prepareMedia(event.dataTransfer.files && event.dataTransfer.files[0]));
if ("serial" in navigator) navigator.serial.addEventListener("disconnect", () => { if (state.port) closePort(); });
previewFrame(new Uint8Array(FRAME_BYTES));
refreshButtons();