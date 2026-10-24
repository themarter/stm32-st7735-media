const $ = function (id) { return document.getElementById(id); };
const ui = {
  connect:$("connect"), connectionLabel:$("connection-label"), dropZone:$("drop-zone"), input:$("media-input"),
  preview:$("preview"), resolution:$("resolution"), fps:$("fps"), mediaKind:$("media-kind"), canvasSize:$("canvas-size"),
  previewResolution:$("preview-resolution"), frameCount:$("frame-count"), outputSize:$("output-size"),
  send:$("send"), stop:$("stop"), export:$("export"), progress:$("progress-bar"), progressLabel:$("progress-label"),
  progressPercent:$("progress-percent"), protocolState:$("protocol-state"), diagnostic:$("diagnostic-log")
};
const ctx = ui.preview.getContext("2d", { willReadFrequently:true });
const TILE = 16, MAX_FRAMES = 120, DATA_PAYLOAD_MAX = 1800;
const CMD = { HELLO:1, FRAME_BEGIN:16, FRAME_END:17 };
const EVT = { ACK:128, HELLO:129 };
const state = { selectedFile:null, frames:[], packageBytes:null, width:128, height:160, orientation:0,
  port:null, reader:null, writer:null, readTask:null, transportReady:false, streaming:false, stopRequested:false,
  sequence:1, pending:new Map(), baud:921600, animationKind:false };
let objectUrl = null;

function log(message) {
  const stamp = new Date().toLocaleTimeString();
  const previous = ui.diagnostic.textContent;
  ui.diagnostic.textContent = (previous.indexOf("连接后会显示") === 0 ? "" : previous + "\n") + "[" + stamp + "] " + message;
  ui.diagnostic.scrollTop = ui.diagnostic.scrollHeight;
}
function progress(percent, message) {
  ui.progress.style.width = Math.max(0,Math.min(100,percent)) + "%";
  ui.progressLabel.textContent = message;
  ui.progressPercent.textContent = Math.round(percent) + "%";
}
function setConnection(connected, text) {
  ui.connectionLabel.textContent = text;
  document.querySelector(".connection").classList.toggle("connected", connected);
  ui.connect.textContent = connected ? "串口已连接" : "连接串口";
}
function layout() {
  const base = ui.resolution.value.split("x").map(Number);
  state.width = state.orientation % 2 ? base[1] : base[0];
  state.height = state.orientation % 2 ? base[0] : base[1];
  ui.preview.width = state.width;
  ui.preview.height = state.height;
  ui.canvasSize.textContent = state.width + " × " + state.height;
  ui.previewResolution.textContent = state.width + " × " + state.height;
  ui.preview.style.aspectRatio = state.width + " / " + state.height;
}
function concat(parts) {
  let total = 0;
  for (const part of parts) total += part.length;
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) { out.set(part,offset); offset += part.length; }
  return out;
}
function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i=0;i<bytes.length;i++) {
    crc ^= bytes[i];
    for (let bit=0;bit<8;bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function writeU16(view, offset, value) { view.setUint16(offset,value,true); }
function writeU32(view, offset, value) { view.setUint32(offset,value >>> 0,true); }
function cobsEncode(input) {
  const out = [0]; let codeIndex = 0, code = 1;
  for (const byte of input) {
    if (byte === 0) { out[codeIndex] = code; code = 1; codeIndex = out.length; out.push(0); }
    else { out.push(byte); if (++code === 0xff) { out[codeIndex] = code; code = 1; codeIndex = out.length; out.push(0); } }
  }
  out[codeIndex] = code;
  return new Uint8Array(out);
}
function cobsDecode(input) {
  const out = [];
  for (let read=0;read<input.length;) {
    const code = input[read++];
    if (!code || read + code - 1 > input.length) return null;
    for (let i=1;i<code;i++) out.push(input[read++]);
    if (code < 0xff && read < input.length) out.push(0);
  }
  return new Uint8Array(out);
}
function wirePacket(channel, seq, payload) {
  const raw = new Uint8Array(3 + payload.length + 4);
  raw[0] = channel; raw[1] = seq & 255; raw[2] = (seq >>> 8) & 255;
  raw.set(payload,3);
  writeU32(new DataView(raw.buffer),raw.length-4,crc32(raw.subarray(0,raw.length-4)));
  const encoded = cobsEncode(raw);
  const framed = new Uint8Array(encoded.length + 2);
  framed[0] = 0; framed.set(encoded,1); framed[framed.length-1] = 0;
  return framed;
}
function handleWireFrame(encoded) {
  const raw = cobsDecode(encoded);
  if (!raw || raw.length < 7) return;
  const view = new DataView(raw.buffer,raw.byteOffset,raw.byteLength);
  const expected = view.getUint32(raw.length-4,true);
  if (crc32(raw.subarray(0,raw.length-4)) !== expected) { log("忽略 CRC 错误的数据帧"); return; }
  if (raw[0] !== 2) return;
  const seq = raw[1] | (raw[2] << 8);
  const payload = raw.subarray(3,raw.length-4);
  const waiter = state.pending.get(seq);
  if (!waiter) { if (payload.length && payload[0] === EVT.HELLO) log("收到设备能力信息"); return; }
  state.pending.delete(seq);
  if (!payload.length) { waiter.reject(new Error("设备返回空事件")); return; }
  if (payload[0] === EVT.ACK) {
    if (payload[1] !== 0) waiter.reject(new Error("STM32 拒绝数据包，状态码 " + payload[1]));
    else waiter.resolve(payload);
  } else if (payload[0] === EVT.HELLO) {
    waiter.resolve(payload);
  } else {
    waiter.reject(new Error("未预期的设备事件 0x" + payload[0].toString(16)));
  }
}
async function readSerialLoop() {
  let pending = [];
  try {
    while (state.reader) {
      const result = await state.reader.read();
      if (result.done) break;
      for (const byte of result.value) {
        if (byte === 0) {
          if (pending.length) handleWireFrame(new Uint8Array(pending));
          pending = [];
        } else if (pending.length < 4096) pending.push(byte);
        else { pending = []; log("接收帧超出缓冲区，已丢弃"); }
      }
    }
  } catch (error) {
    if (state.port) log("串口接收结束：" + error.message);
  }
}
async function sendPacket(channel,payload,timeoutMs) {
  if (!state.writer) throw new Error("串口尚未连接");
  const seq = state.sequence++ & 0xffff;
  const promise = new Promise(function(resolve,reject) {
    const timer = setTimeout(function() {
      state.pending.delete(seq);
      reject(new Error("设备确认超时（序号 " + seq + "），请检查接收固件和串口接线"));
    },timeoutMs || 2500);
    state.pending.set(seq,{
      resolve:function(value){clearTimeout(timer);resolve(value);},
      reject:function(error){clearTimeout(timer);reject(error);}
    });
  });
  await state.writer.write(wirePacket(channel,seq,payload));
  return promise;
}
async function hello() {
  const reply = await sendPacket(0,new Uint8Array([CMD.HELLO]),1800);
  if (reply[0] !== EVT.HELLO || reply.length < 6) throw new Error("设备没有返回 ST73 握手信息");
  const view = new DataView(reply.buffer,reply.byteOffset,reply.byteLength);
  const width = view.getUint16(1,true), height = view.getUint16(3,true), version = reply[5];
  log("接收端识别成功：固件协议 v" + version + "，屏幕 " + width + " × " + height);
  const valid = (width === state.width && height === state.height) ||
                (width === state.height && height === state.width);
  if (!valid) {
    ui.protocolState.textContent = "分辨率不匹配";
    state.transportReady = false;
    throw new Error("设备报告 " + width + " × " + height + "，当前页面画幅为 " + state.width + " × " + state.height);
  }
  state.transportReady = true;
  ui.protocolState.textContent = "ST73 v" + version + " · 已握手";
  ui.send.disabled = state.frames.length === 0;
}
async function connectSerial() {
  if (state.port) { await disconnectSerial(); return; }
  if (!("serial" in navigator)) {
    log("当前浏览器不支持 Web Serial。请使用最新版 Chrome/Edge，并通过 HTTPS 或 localhost 打开页面。");
    return;
  }
  try {
    state.port = await navigator.serial.requestPort();
    await state.port.open({ baudRate:state.baud, bufferSize:4096 });
    state.writer = state.port.writable.getWriter();
    state.reader = state.port.readable.getReader();
    state.readTask = readSerialLoop();
    setConnection(true,"串口已打开 · 正在握手");
    ui.connect.disabled = false;
    log("串口已打开，波特率 " + state.baud + "。正在检查 ST73 接收固件。");
    try { await hello(); }
    catch (error) {
      ui.protocolState.textContent = "尚未识别接收固件";
      state.transportReady = false;
      log(error.message);
      log("可继续离线转换并导出 .st73；实时投屏需要先集成 stm32-st7735/firmware 中的接收协议。");
    }
    ui.send.disabled = !state.transportReady || state.frames.length === 0;
  } catch (error) {
    log("连接失败：" + error.message);
    await disconnectSerial();
  }
}
async function disconnectSerial() {
  state.transportReady = false;
  for (const waiter of state.pending.values()) waiter.reject(new Error("串口已断开"));
  state.pending.clear();
  try { if (state.reader) { await state.reader.cancel(); state.reader.releaseLock(); } } catch {}
  try { if (state.writer) { await state.writer.close(); } } catch {}
  try { if (state.port) await state.port.close(); } catch {}
  state.reader = state.writer = state.port = null;
  ui.protocolState.textContent = "等待 STM32 接收端";
  setConnection(false,"未连接设备");
  ui.send.disabled = true;
  ui.stop.disabled = true;
  log("串口已断开");
}
function drawCover(source) {
  const width = state.width, height = state.height;
  ctx.fillStyle = "#000"; ctx.fillRect(0,0,width,height);
  const sw = source.displayWidth || source.videoWidth || source.naturalWidth || source.width;
  const sh = source.displayHeight || source.videoHeight || source.naturalHeight || source.height;
  if (!sw || !sh) throw new Error("媒体尺寸无效");
  const scale = Math.max(width/sw,height/sh);
  const dw = sw*scale, dh = sh*scale;
  ctx.drawImage(source,(width-dw)/2,(height-dh)/2,dw,dh);
}
function rawFromCanvas() {
  const rgba = ctx.getImageData(0,0,state.width,state.height).data;
  const output = new Uint8Array(state.width*state.height*2);
  for (let s=0,d=0;s<rgba.length;s+=4,d+=2) {
    const pixel = ((rgba[s]&0xf8)<<8) | ((rgba[s+1]&0xfc)<<3) | (rgba[s+2]>>3);
    output[d] = pixel >>> 8; output[d+1] = pixel & 255;
  }
  return output;
}
function previewRaw(raw) {
  const rgba = ctx.createImageData(state.width,state.height);
  for (let s=0,d=0;s<raw.length;s+=2,d+=4) {
    const pixel = (raw[s]<<8)|raw[s+1];
    rgba.data[d] = ((pixel>>>11)&31)*255/31;
    rgba.data[d+1] = ((pixel>>>5)&63)*255/63;
    rgba.data[d+2] = (pixel&31)*255/31;
    rgba.data[d+3] = 255;
  }
  ctx.putImageData(rgba,0,0);
}
function extractTile(frame,x,y,w,h) {
  const out = new Uint8Array(w*h*2);
  for (let row=0;row<h;row++) {
    const start = ((y+row)*state.width+x)*2;
    out.set(frame.subarray(start,start+w*2),row*w*2);
  }
  return out;
}
function sameBytes(a,b) {
  if (!b || a.length !== b.length) return false;
  for (let i=0;i<a.length;i++) if (a[i] !== b[i]) return false;
  return true;
}
function rle(input) {
  const out = [];
  for (let i=0;i<input.length;) {
    let count = 1;
    while (i+count*2<input.length && count<256 && input[i]===input[i+count*2] && input[i+1]===input[i+count*2+1]) count++;
    out.push(count-1,input[i],input[i+1]); i += count*2;
  }
  return new Uint8Array(out);
}
function tileRecord(frame,previous,x,y,w,h,first) {
  const current = extractTile(frame,x,y,w,h);
  if (!first && previous && sameBytes(current,extractTile(previous,x,y,w,h))) return null;
  const compressed = rle(current);
  const useRle = compressed.length < current.length;
  const payload = useRle ? compressed : current;
  const header = new Uint8Array(7);
  header.set([x,y,w,h,useRle?1:0]);
  new DataView(header.buffer).setUint16(5,payload.length,true);
  return concat([header,payload]);
}
function encodeFrame(frame,previous,first) {
  const records = [];
  let tileCount = 0;
  for (let y=0;y<state.height;y+=TILE) {
    for (let x=0;x<state.width;x+=TILE) {
      const w = Math.min(TILE,state.width-x), h = Math.min(TILE,state.height-y);
      const record = tileRecord(frame,previous,x,y,w,h,first);
      if (record) { records.push(record); tileCount++; }
    }
  }
  return { records:records, tileCount:tileCount, data:concat(records) };
}
function makePackage(frames) {
  const bodyParts = [];
  for (let i=0;i<frames.length;i++) {
    const encoded = encodeFrame(frames[i].pixels,i ? frames[i-1].pixels : null,i===0);
    const header = new Uint8Array(5);
    const view = new DataView(header.buffer);
    writeU16(view,0,frames[i].delay);
    header[2] = i===0 ? 1 : 0;
    writeU16(view,3,encoded.tileCount);
    bodyParts.push(header,encoded.data);
  }
  const body = concat(bodyParts);
  const header = new Uint8Array(24);
  header.set([83,84,55,51],0); // ST73
  header[4] = 1; header[5] = frames.length>1 ? 1 : 0;
  header[6] = state.orientation; header[7] = TILE;
  const view = new DataView(header.buffer);
  writeU16(view,8,state.width); writeU16(view,10,state.height);
  writeU16(view,12,frames.length); writeU16(view,14,Number(ui.fps.value));
  writeU32(view,16,body.length); writeU32(view,20,crc32(body));
  return concat([header,body]);
}
function human(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + " KiB";
  return (bytes/(1024*1024)).toFixed(2) + " MiB";
}
async function waitSeek(video,time) {
  if (Math.abs(video.currentTime-time)<0.015) return;
  await new Promise(function(resolve,reject) {
    const timer = setTimeout(function(){cleanup();reject(new Error("视频抽帧超时"));},5000);
    function cleanup(){clearTimeout(timer);video.removeEventListener("seeked",done);video.removeEventListener("error",failed);}
    function done(){cleanup();resolve();}
    function failed(){cleanup();reject(new Error("视频抽帧失败"));}
    video.addEventListener("seeked",done,{once:true}); video.addEventListener("error",failed,{once:true});
    video.currentTime = time;
  });
}
async function loadImage(file) {
  if (file.type === "image/gif" && "ImageDecoder" in window) {
    const decoder = new ImageDecoder({data:new Uint8Array(await file.arrayBuffer()),type:"image/gif",preferAnimation:true});
    try {
      await decoder.tracks.ready;
      const count = Math.min(MAX_FRAMES,decoder.tracks.selectedTrack ? decoder.tracks.selectedTrack.frameCount : 1);
      const frames = [];
      for (let i=0;i<count;i++) {
        const result = await decoder.decode({frameIndex:i,completeFramesOnly:true});
        drawCover(result.image);
        frames.push({pixels:rawFromCanvas(),delay:Math.max(20,Math.min(2000,Math.round((result.image.duration||83333)/1000)))});
        result.image.close();
        progress(5+Math.round((i+1)/count*55),"正在解析 GIF 帧 " + (i+1) + "/" + count);
      }
      return frames;
    } finally { decoder.close(); }
  }
  if (file.type.startsWith("video/")) {
    const video = document.createElement("video");
    video.muted = true; video.playsInline = true; video.preload = "auto";
    if (objectUrl) URL.revokeObjectURL(objectUrl);
    objectUrl = URL.createObjectURL(file); video.src = objectUrl;
    await new Promise(function(resolve,reject) {
      video.onloadedmetadata = resolve; video.onerror = function(){reject(new Error("浏览器无法读取该视频格式"));};
    });
    if (video.readyState < 2) {
      await new Promise(function(resolve,reject) {
        const timer = setTimeout(function(){cleanup();reject(new Error("视频首帧加载超时"));},5000);
        function cleanup(){clearTimeout(timer);video.removeEventListener("loadeddata",ready);video.removeEventListener("error",failed);}
        function ready(){cleanup();resolve();}
        function failed(){cleanup();reject(new Error("视频首帧加载失败"));}
        video.addEventListener("loadeddata",ready,{once:true});
        video.addEventListener("error",failed,{once:true});
      });
    }
    const fps = Number(ui.fps.value);
    const duration = Math.min(video.duration,MAX_FRAMES/fps);
    const count = Math.max(1,Math.min(MAX_FRAMES,Math.floor(duration*fps)));
    const frames = [];
    for (let i=0;i<count;i++) {
      await waitSeek(video,Math.min(Math.max(0,duration-0.001),i/fps));
      drawCover(video);
      frames.push({pixels:rawFromCanvas(),delay:Math.round(1000/fps)});
      progress(5+Math.round((i+1)/count*55),"正在解析视频帧 " + (i+1) + "/" + count);
    }
    video.pause(); video.removeAttribute("src"); video.load();
    URL.revokeObjectURL(objectUrl); objectUrl = null;
    return frames;
  }
  const image = new Image();
  const url = URL.createObjectURL(file);
  try {
    await new Promise(function(resolve,reject){image.onload=resolve;image.onerror=function(){reject(new Error("无法解码图片，请尝试 PNG/JPEG/GIF/WebP"));};image.src=url;});
    drawCover(image);
    return [{pixels:rawFromCanvas(),delay:1000}];
  } finally { URL.revokeObjectURL(url); }
}
async function prepareMedia(file) {
  if (!file) return;
  if (!file.type.startsWith("image/") && !file.type.startsWith("video/")) {
    log("不支持的文件类型：" + file.type); return;
  }
  state.selectedFile = file; state.frames = []; state.packageBytes = null;
  state.animationKind = file.type === "image/gif" || file.type.startsWith("video/");
  ui.mediaKind.textContent = file.type.startsWith("video/") ? "视频" : file.type==="image/gif" ? "GIF 动画" : "静态图片";
  ui.frameCount.textContent = "解析中…"; ui.outputSize.textContent = "—";
  ui.export.disabled = true; ui.send.disabled = true;
  layout();
  progress(2,"正在读取媒体…");
  try {
    state.frames = await loadImage(file);
    if (!state.frames.length) throw new Error("未解析到媒体帧");
    previewRaw(state.frames[0].pixels);
    state.packageBytes = makePackage(state.frames);
    ui.frameCount.textContent = state.frames.length + (state.frames.length>=MAX_FRAMES ? "（已限帧）" : "");
    ui.outputSize.textContent = human(state.packageBytes.length);
    ui.export.disabled = false;
    ui.send.disabled = !state.transportReady;
    progress(100,"媒体转换完成");
    log("已转换 " + file.name + "： " + state.width + " × " + state.height + "，" + state.frames.length + " 帧，包大小 " + human(state.packageBytes.length));
  } catch (error) {
    state.frames=[]; ui.frameCount.textContent="失败"; ui.outputSize.textContent="—";
    progress(0,"转换失败：" + error.message); log(error.message);
  }
}
function savePackage() {
  if (!state.packageBytes) return;
  const blob = new Blob([state.packageBytes],{type:"application/octet-stream"});
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  const stem = (state.selectedFile ? state.selectedFile.name.replace(/.[^.]+$/,"") : "media").replace(/[^a-zA-Z0-9._-]+/g,"-").slice(0,48) || "media";
  link.href=url; link.download=stem+".st73"; document.body.appendChild(link); link.click(); link.remove();
  setTimeout(function(){URL.revokeObjectURL(url);},1000);
  log("已导出 ST73 媒体包。");
}
async function sendFrame(index,frameId,first) {
  const item = state.frames[index];
  const previous = first ? null : state.frames[index-1].pixels;
  const encoded = encodeFrame(item.pixels,previous,first);
  const begin = new Uint8Array(12);
  const view = new DataView(begin.buffer);
  begin[0]=CMD.FRAME_BEGIN; writeU16(view,1,frameId); writeU16(view,3,state.width);
  writeU16(view,5,state.height); begin[7]=state.orientation;
  writeU16(view,8,item.delay); writeU16(view,10,encoded.tileCount);
  await sendPacket(0,begin,2500);
  let chunkParts=[],chunkSize=4,chunkNo=0;
  const tileTotal = encoded.data;
  // Re-split only at complete tile-record boundaries so the MCU never sees a partial tile.
  let offset=0;
  while (offset<tileTotal.length) {
    const len = 7 + tileTotal[offset+5] + (tileTotal[offset+6]<<8);
    const record = tileTotal.subarray(offset,offset+len);
    if (chunkSize+record.length>DATA_PAYLOAD_MAX && chunkParts.length) {
      const payload = new Uint8Array(chunkSize);
      const dv = new DataView(payload.buffer);
      writeU16(dv,0,frameId); writeU16(dv,2,chunkNo++);
      let p=4; for (const part of chunkParts) {payload.set(part,p);p+=part.length;}
      await sendPacket(1,payload,4000);
      chunkParts=[];chunkSize=4;
    }
    chunkParts.push(record);chunkSize+=record.length;offset+=len;
  }
  if (chunkParts.length) {
    const payload = new Uint8Array(chunkSize);
    const dv = new DataView(payload.buffer);
    writeU16(dv,0,frameId); writeU16(dv,2,chunkNo++);
    let p=4; for (const part of chunkParts) {payload.set(part,p);p+=part.length;}
    await sendPacket(1,payload,4000);
  }
  const end = new Uint8Array(3); end[0]=CMD.FRAME_END; writeU16(new DataView(end.buffer),1,frameId);
  await sendPacket(0,end,2500);
  return encoded.tileCount;
}
async function startStreaming() {
  if (!state.transportReady || !state.frames.length || state.streaming) return;
  state.streaming=true;state.stopRequested=false;ui.send.disabled=true;ui.stop.disabled=false;ui.export.disabled=true;
  try {
    let frameId=0,framesSent=0;
    while (!state.stopRequested) {
      for (let i=0;i<state.frames.length && !state.stopRequested;i++) {
        const start=performance.now();
        const first = i===0;
        const tileCount=await sendFrame(i,frameId++ & 0xffff,first);
        framesSent++;
        const pct=state.frames.length===1 ? 100 : ((i+1)/state.frames.length*100);
        progress(pct,"已显示第 " + (i+1) + "/" + state.frames.length + " 帧 · 更新 " + tileCount + " 个分块");
        if (state.frames.length===1) {
          log("静态图片已发送到 ST7735。");
          state.stopRequested=true;
          break;
        }
        const delay=1000/Number(ui.fps.value)-(performance.now()-start);
        if (delay>0) await new Promise(function(resolve){setTimeout(resolve,delay);});
      }
    }
    if (state.frames.length>1) log("动画投屏结束，累计发送 " + framesSent + " 帧。");
  } catch (error) {
    progress(0,"投屏失败：" + error.message);log("投屏失败：" + error.message);
  } finally {
    state.streaming=false;ui.stop.disabled=true;ui.export.disabled=!state.packageBytes;
    ui.send.disabled=!state.transportReady||!state.frames.length;
  }
}
function stopStreaming() { state.stopRequested=true;ui.stop.disabled=true;log("已请求停止动画；屏幕保持最后一帧。"); }
ui.connect.addEventListener("click",connectSerial);
ui.input.addEventListener("change",function(){prepareMedia(ui.input.files && ui.input.files[0]);});
ui.export.addEventListener("click",savePackage);
ui.send.addEventListener("click",startStreaming);
ui.stop.addEventListener("click",stopStreaming);
ui.resolution.addEventListener("change",function(){layout();if(state.selectedFile)prepareMedia(state.selectedFile);});
ui.fps.addEventListener("change",function(){if(state.selectedFile)prepareMedia(state.selectedFile);});
document.querySelectorAll(".orientation-choice").forEach(function(button) {
  button.addEventListener("click",function() {
    state.orientation=Number(button.dataset.orientation);
    document.querySelectorAll(".orientation-choice").forEach(function(other){other.classList.toggle("active",other===button);});
    layout();
    if(state.selectedFile) prepareMedia(state.selectedFile);
  });
});
["dragenter","dragover"].forEach(function(type){ui.dropZone.addEventListener(type,function(event){event.preventDefault();ui.dropZone.classList.add("dragging");});});
["dragleave","drop"].forEach(function(type){ui.dropZone.addEventListener(type,function(event){event.preventDefault();ui.dropZone.classList.remove("dragging");});});
ui.dropZone.addEventListener("drop",function(event){prepareMedia(event.dataTransfer.files && event.dataTransfer.files[0]);});
navigator.serial && navigator.serial.addEventListener("disconnect",function(){if(state.port)disconnectSerial();});
layout();
