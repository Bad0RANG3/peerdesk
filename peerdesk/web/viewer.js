/* PeerDesk viewer: WebSocket + canvas renderer + input forwarder. */
(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const canvas = $("screen");
  const ctx = canvas.getContext("2d", { alpha: false, desynchronized: true });
  const gate = $("gate");
  const gateText = $("gateText");
  const gateDot = $("gateDot");
  const toolbar = $("toolbar");
  const hud = $("hud");
  const menu = $("menu");
  const toastEl = $("toast");

  let ws = null;
  let connected = false;
  let frameW = 0, frameH = 0;
  let drawRect = { x: 0, y: 0, w: 1, h: 1 };
  let monitors = [];
  let info = {};
  let pendingBitmap = null, decoding = false;

  /* ------------------------------------------------------------------ utils */
  function toast(text, ms = 1800) {
    toastEl.textContent = text;
    toastEl.classList.add("show");
    clearTimeout(toast._t);
    toast._t = setTimeout(() => toastEl.classList.remove("show"), ms);
  }

  function setGate(mode, text) {
    if (mode === "hidden") { gate.classList.add("hidden"); return; }
    gate.classList.remove("hidden");
    gateText.textContent = text;
    gateDot.className = "dot " + (mode === "ok" ? "live" : mode === "warn" ? "warn" : mode === "bad" ? "bad" : "");
  }

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  }

  /* ------------------------------------------------------------- websocket */
  function connectWS() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    ws = new WebSocket(`${proto}//${location.host}/ws`);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      gateText.textContent = "本地桥已就绪";
    };
    ws.onclose = () => {
      connected = false;
      setGate("bad", "本地桥断开，刷新页面重试");
      toolbar.style.display = "none";
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data === "string") {
        let msg; try { msg = JSON.parse(ev.data); } catch { return; }
        onJson(msg);
      } else {
        const bytes = new Uint8Array(ev.data);
        if (bytes.length && bytes[0] === 0x41) onAudio(bytes);   // "A" 声音
        else onFrame(bytes);
      }
    };
  }

  function onJson(msg) {
    switch (msg.t) {
      case "ready":
        if (msg.code) $("code").value = fmtCode(msg.code);
        break;
      case "info":
        info = msg;
        monitors = msg.monitors || [];
        if (msg.settings) {
          $("server").value = $("server").value || (msg.server || "");
        }
        updateMonitorLabel();
        break;
      case "link":
        if (msg.state === "direct" || msg.state === "relay") {
          connected = true;
          gate.classList.add("hidden");
          toolbar.style.display = "flex";
          toast(msg.relay ? "已连接 · 中继模式" : "已连接 · P2P 直连");
        } else if (msg.state === "punching") {
          setGate("warn", "正在打洞…");
        } else if (msg.state === "closed") {
          setGate("bad", "连接已断开");
          toolbar.style.display = "none";
          connected = false;
        }
        break;
      case "stats":
        $("hudFps").textContent = (msg.fps ?? 0) + " fps";
        $("hudMbps").textContent = (msg.mbps ?? 0) + " Mbps";
        $("hudRtt").textContent = (msg.rtt ?? 0) + " ms";
        $("hudMode").textContent = msg.relay ? "中继" : "P2P";
        audioFps = audioFrames; audioFrames = 0;
        $("hudAud").textContent = !audioOn ? "关" : (audioFps ? audioFps + " 帧/s" : "静音");
        break;
      case "audio":
        if (typeof msg.on === "boolean" && !msg.pending) setAudio(msg.on, false);
        break;
      case "clipboard":
        navigator.clipboard?.writeText(msg.s || "").then(
          () => toast("已同步剪贴板"),
          () => toast("浏览器拒绝写入剪贴板")
        );
        break;
      case "error":
        setGate("bad", msg.text || "连接失败");
        break;
      case "notice":
        toast(msg.text || "");
        break;
      case "upload":
        if (msg.state === "begin") toast(`正在发送 ${msg.name}…`);
        else if (msg.state === "progress") {
          const pct = msg.size ? Math.round((msg.sent / msg.size) * 100) : 0;
          $("toast").textContent = `发送 ${msg.name || ""} ${pct}%`;
          $("toast").classList.add("show");
        } else if (msg.state === "done") toast(`已发送 ${msg.name || "文件"}`);
        break;
      case "file_in":
        if (msg.state === "begin") toast(`对端正在发送 ${msg.name}…`);
        else if (msg.state === "progress") {
          $("toast").textContent = `接收中 ${Math.round((msg.got || 0) / 1024)} KB`;
          $("toast").classList.add("show");
        } else if (msg.state === "done") {
          toast(`已接收 ${msg.name}（${Math.round((msg.size || 0) / 1024)} KB）`);
          if (msg.url) {
            const a = document.createElement("a");
            a.href = msg.url; a.download = msg.name || "file";
            document.body.appendChild(a); a.click(); a.remove();
          }
        }
        break;
      case "file_done":
        toast(`对端已保存 ${(msg.path || "").split(/[\\/]/).pop()}`);
        break;
    }
  }

  /* ----------------------------------------------------------------- audio */
  // 服务端送 16 kHz 单声道 µ-law，20 ms 一帧（320 字节），帧头见 peerdesk/wire.py AUDIO_HDR。
  const ULAW = new Float32Array(256);
  for (let i = 0; i < 256; i++) {
    const u = ~i & 0xFF;
    let t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    ULAW[i] = ((u & 0x80) ? 0x84 - t : t - 0x84) / 32768;
  }

  const AUDIO_RATE = 16000;
  const AUDIO_LEAD = 0.06;      // 抖动缓冲目标延迟
  const AUDIO_MAX_LAG = 0.45;   // 落后太多就重排时间轴，宁丢不积压
  let audioCtx = null;
  let audioGain = null;
  let audioNext = 0;
  let audioSeq = -1;
  let audioFrames = 0;
  let audioFps = 0;
  let audioDrops = 0;
  let audioOn = localStorage.getItem("pd.audio") !== "0";

  function ensureAudio() {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return null;
    if (!audioCtx) {
      try {
        audioCtx = new Ctx({ sampleRate: AUDIO_RATE, latencyHint: "interactive" });
      } catch {
        audioCtx = new Ctx();
      }
      audioGain = audioCtx.createGain();
      audioGain.gain.value = 1;
      audioGain.connect(audioCtx.destination);
    }
    if (audioCtx.state === "suspended") audioCtx.resume().catch(() => {});
    return audioCtx;
  }

  function playPcm(samples) {
    const ctx = audioCtx;
    if (!ctx || !samples.length) return;
    const buf = ctx.createBuffer(1, samples.length, AUDIO_RATE);
    const out = buf.getChannelData(0);
    for (let i = 0; i < samples.length; i++) out[i] = ULAW[samples[i]];
    const src = ctx.createBufferSource();
    src.buffer = buf;
    src.connect(audioGain);
    const now = ctx.currentTime;
    if (audioNext < now + 0.005) {
      audioNext = now + AUDIO_LEAD;                    // 刚开或断流，重新排队
    } else if (audioNext - now > AUDIO_MAX_LAG) {
      audioNext = now + AUDIO_LEAD;
      audioDrops++;
    }
    src.start(audioNext);
    audioNext += samples.length / AUDIO_RATE;
    audioFrames++;
  }

  function onAudio(bytes) {
    if (!audioOn || bytes.length < 13) return;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const seq = view.getUint32(1, true);
    const samples = view.getUint16(9, true);
    const prevLen = view.getUint16(11, true);
    let off = 13;
    const fresh = audioSeq < 0 || audioSeq !== seq - 1;
    if (prevLen && fresh && off + prevLen <= bytes.length && seq > 0) {
      if (!ensureAudio()) return;
      playPcm(bytes.subarray(off, off + prevLen));    // 上一帧丢了，用它顶一下
    }
    off += prevLen;
    if (seq === audioSeq || off + samples > bytes.length) return;
    if (!ensureAudio()) return;
    audioSeq = seq;
    playPcm(bytes.subarray(off, off + samples));
  }

  function setAudio(on, notify = true) {
    audioOn = !!on;
    localStorage.setItem("pd.audio", audioOn ? "1" : "0");
    const btn = $("btnAudio");
    if (btn) btn.classList.toggle("active", audioOn);
    const hud = $("hudAud");
    if (hud) hud.textContent = audioOn ? "开" : "关";
    if (audioOn) {
      ensureAudio();
      audioNext = 0;
    } else if (audioCtx) {
      audioNext = 0;
      audioSeq = -1;
    }
    if (notify && connected) send({ t: "audio", on: audioOn });
  }

  /* ---------------------------------------------------------------- upload */
  const UPLOAD_SLICE = 24 * 1024;

  async function uploadFile(file) {
    if (!connected || !ws || ws.readyState !== 1) { setGate("bad", "先连接再发送文件"); return; }
    send({ t: "upload_begin", name: file.name, size: file.size });
    let off = 0;
    while (off < file.size) {
      const buf = new Uint8Array(await file.slice(off, off + UPLOAD_SLICE).arrayBuffer());
      if (!buf.length) break;
      const frame = new Uint8Array(buf.length + 1);
      frame[0] = 0x55;                     // "U"：浏览器→viewer 的上传分块
      frame.set(buf, 1);
      ws.send(frame);
      off += buf.length;
      await new Promise((r) => setTimeout(r, 0));   // 让出主线程，避免卡顿画面
    }
    send({ t: "upload_end" });
  }

  async function uploadFiles(list) {
    for (const file of list) {
      try { await uploadFile(file); } catch (e) { toast("发送失败：" + (e.message || e)); }
    }
  }

  const drop = $("drop");
  let dragDepth = 0;
  window.addEventListener("dragenter", (ev) => {
    if (!connected || !ev.dataTransfer?.types?.includes("Files")) return;
    dragDepth++; ev.preventDefault(); drop.classList.add("show");
  });
  window.addEventListener("dragover", (ev) => {
    if (!connected) return;
    ev.preventDefault(); ev.dataTransfer.dropEffect = "copy";
  });
  window.addEventListener("dragleave", () => {
    dragDepth = Math.max(0, dragDepth - 1);
    if (!dragDepth) drop.classList.remove("show");
  });
  window.addEventListener("drop", (ev) => {
    if (!connected) return;
    ev.preventDefault(); dragDepth = 0; drop.classList.remove("show");
    const files = ev.dataTransfer?.files;
    if (files && files.length) uploadFiles(files);
  });
  $("btnFile").addEventListener("click", () => { if (!connected) { toast("先连接"); return; } $("filePick").click(); });
  $("filePick").addEventListener("change", (ev) => { uploadFiles(ev.target.files); ev.target.value = ""; });

  /* ---------------------------------------------------------------- render */
  function onFrame(bytes) {
    // first byte is the frame tag ("J" for JPEG)
    const jpeg = bytes.subarray(1);
    const blob = new Blob([jpeg], { type: "image/jpeg" });
    createImageBitmap(blob).then((bmp) => {
      pendingBitmap = bmp;
      scheduleDraw();
    }).catch(() => {});
  }

  let drawScheduled = false;
  function scheduleDraw() {
    if (drawScheduled) return;
    drawScheduled = true;
    requestAnimationFrame(() => {
      drawScheduled = false;
      const bmp = pendingBitmap;
      if (!bmp) return;
      pendingBitmap = null;
      paint(bmp);
    });
  }

  function paint(bmp) {
    const dpr = window.devicePixelRatio || 1;
    // Size the bitmap from the viewport, never from layout metrics: a collapsed
    // or unlaid-out ancestor must not shrink the framebuffer to a fraction.
    const cw = Math.max(1, Math.round((window.innerWidth || 1280) * dpr));
    const ch = Math.max(1, Math.round((window.innerHeight || 720) * dpr));
    if (canvas.width !== cw || canvas.height !== ch) {
      canvas.width = cw; canvas.height = ch;
    }
    frameW = bmp.width; frameH = bmp.height;
    const scale = Math.min(cw / bmp.width, ch / bmp.height);
    const w = Math.round(bmp.width * scale), h = Math.round(bmp.height * scale);
    const x = Math.round((cw - w) / 2), y = Math.round((ch - h) / 2);
    drawRect = { x, y, w, h };
    ctx.fillStyle = "#05070b";
    ctx.fillRect(0, 0, cw, ch);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(bmp, x, y, w, h);
    bmp.close?.();
    $("hudRes").textContent = `${frameW}×${frameH} @ ${cw}×${ch}`;
  }

  /* ----------------------------------------------------------------- input */
  function normalize(ev) {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const px = (ev.clientX - rect.left) * dpr;
    const py = (ev.clientY - rect.top) * dpr;
    let nx = (px - drawRect.x) / Math.max(1, drawRect.w);
    let ny = (py - drawRect.y) / Math.max(1, drawRect.h);
    nx = Math.min(1, Math.max(0, nx));
    ny = Math.min(1, Math.max(0, ny));
    return { x: +nx.toFixed(4), y: +ny.toFixed(4) };
  }

  let movePending = null, moveScheduled = false;
  canvas.addEventListener("mousemove", (ev) => {
    if (!connected) return;
    const p = normalize(ev);
    movePending = p;
    if (!moveScheduled) {
      moveScheduled = true;
      requestAnimationFrame(() => {
        moveScheduled = false;
        if (movePending) send({ t: "m", act: "move", x: movePending.x, y: movePending.y });
      });
    }
  });

  const BTN = { 0: "left", 1: "middle", 2: "right", 3: "back", 4: "forward" };
  canvas.addEventListener("mousedown", (ev) => {
    if (!connected) return;
    ev.preventDefault();
    canvas.focus();
    const p = normalize(ev);
    send({ t: "m", act: "down", btn: BTN[ev.button] || "left", x: p.x, y: p.y });
  });
  window.addEventListener("mouseup", (ev) => {
    if (!connected) return;
    const p = normalize(ev);
    send({ t: "m", act: "up", btn: BTN[ev.button] || "left", x: p.x, y: p.y });
  });
  canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());
  canvas.addEventListener("auxclick", (ev) => ev.preventDefault());
  canvas.addEventListener("wheel", (ev) => {
    if (!connected) return;
    ev.preventDefault();
    let dy = -ev.deltaY;
    if (ev.shiftKey || Math.abs(ev.deltaX) > Math.abs(ev.deltaY)) dy = -ev.deltaX;
    const steps = Math.max(-600, Math.min(600, Math.round(dy)));
    send({ t: "m", act: "wheel", dy: steps * 2, h: ev.shiftKey });
  }, { passive: false });

  const KEY_BLOCK = new Set(["Tab", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "ContextMenu"]);
  canvas.addEventListener("keydown", (ev) => {
    if (!connected) return;
    if (ev.ctrlKey && ev.shiftKey && (ev.code === "KeyV" || ev.code === "KeyC")) return;
    if (KEY_BLOCK.has(ev.code) || ev.ctrlKey || ev.altKey) ev.preventDefault();
    if (ev.repeat && !["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Backspace", "Delete"].includes(ev.code)) return;
    send({ t: "k", code: ev.code, down: true });
  });
  canvas.addEventListener("keyup", (ev) => {
    if (!connected) return;
    send({ t: "k", code: ev.code, down: false });
  });
  canvas.addEventListener("paste", (ev) => {
    const text = ev.clipboardData?.getData("text");
    if (text && connected) { send({ t: "text", s: text }); ev.preventDefault(); toast("已粘贴文本"); }
  });
  canvas.addEventListener("blur", () => { /* remote keys stay up: send neutral releases */ });

  /* --------------------------------------------------------------- toolbar */
  const QUALITY = [
    { id: "fluent", label: "流畅", w: 1280, q: 45, fps: 24 },
    { id: "balanced", label: "均衡", w: 1600, q: 62, fps: 30 },
    { id: "hd", label: "高清", w: 1920, q: 72, fps: 40 },
    { id: "uhd", label: "超清", w: 2560, q: 80, fps: 50 },
    { id: "native", label: "原画", w: 3840, q: 88, fps: 60 },
  ];
  let qualityId = "balanced";

  function closeMenu() { menu.classList.remove("open"); menu.innerHTML = ""; }

  function openMenu(kind) {
    menu.innerHTML = "";
    if (kind === "display") {
      const grp = document.createElement("div");
      grp.className = "grp";
      grp.textContent = "选择显示器";
      menu.appendChild(grp);
      (monitors.length ? monitors : [{ index: 0, w: 0, h: 0, primary: true }]).forEach((m) => {
        const b = document.createElement("button");
        b.innerHTML = `<span>显示器 ${m.index + 1}${m.primary ? "（主）" : ""}</span><span>${m.w}×${m.h}</span>`;
        if ((info.settings?.monitor ?? 0) === m.index) b.classList.add("sel");
        b.onclick = () => {
          send({ t: "monitor", i: m.index });
          info.settings = { ...(info.settings || {}), monitor: m.index };
          updateMonitorLabel();
          closeMenu();
        };
        menu.appendChild(b);
      });
    } else if (kind === "quality") {
      const grp = document.createElement("div");
      grp.className = "grp";
      grp.textContent = "画质模式";
      menu.appendChild(grp);
      QUALITY.forEach((q) => {
        const b = document.createElement("button");
        b.innerHTML = `<span>${q.label}</span><span>${q.w}px · ${q.fps}fps</span>`;
        if (q.id === qualityId) b.classList.add("sel");
        b.onclick = () => {
          qualityId = q.id;
          send({ t: "quality", q: q.q, w: q.w, fps: q.fps });
          $("qLabel").textContent = q.label;
          closeMenu();
          toast(`画质：${q.label}`);
        };
        menu.appendChild(b);
      });
    }
    menu.classList.add("open");
  }

  function updateMonitorLabel() {
    const idx = info.settings?.monitor ?? 0;
    const m = monitors.find((x) => x.index === idx);
    $("monLabel").textContent = m ? `显示器 ${idx + 1}` : "显示器";
  }

  document.querySelectorAll("[data-menu]").forEach((btn) => {
    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      const kind = btn.dataset.menu;
      if (menu.classList.contains("open") && menu.dataset.kind === kind) { closeMenu(); return; }
      menu.dataset.kind = kind;
      openMenu(kind);
    });
  });
  document.addEventListener("click", () => closeMenu());

  $("btnClip").onclick = async () => {
    try {
      const text = await navigator.clipboard.readText();
      send({ t: "clip", s: text });
      toast("已发送本机剪贴板");
    } catch {
      toast("浏览器未授权读取剪贴板");
    }
  };

  $("btnKeys").onclick = () => { $("kbdBox").classList.add("open"); $("kbdText").focus(); };
  $("kbdCancel").onclick = () => $("kbdBox").classList.remove("open");
  $("kbdSend").onclick = () => {
    const text = $("kbdText").value;
    if (text) { send({ t: "text", s: text }); toast("文本已发送"); }
    $("kbdText").value = "";
    $("kbdBox").classList.remove("open");
  };

  $("btnCtrlAltDel").onclick = () => send({ t: "k", code: "Delete", down: true }) || setTimeout(() => send({ t: "k", code: "Delete", down: false }), 40);

  $("btnFull").onclick = () => {
    if (!document.fullscreenElement) document.documentElement.requestFullscreen?.();
    else document.exitFullscreen?.();
  };

  $("btnHud").onclick = () => {
    hud.style.display = hud.style.display === "none" ? "block" : "none";
  };

  $("btnAudio").onclick = () => {
    setAudio(!audioOn);
    toast(audioOn ? "已开启对端声音" : "已关闭声音");
  };
  $("btnAudio").classList.toggle("active", audioOn);

  $("btnHangup").onclick = () => {
    send({ t: "hangup" });
    connected = false;
    toolbar.style.display = "none";
    setGate("bad", "已断开");
  };

  /* ------------------------------------------------------------------- gate */
  function fmtCode(v) {
    const d = (v || "").replace(/\D/g, "").slice(0, 9);
    return d.replace(/(.{3})(?=.)/g, "$1 ");
  }
  $("code").addEventListener("input", (ev) => {
    ev.target.value = fmtCode(ev.target.value);
  });
  $("code").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") $("connect").click();
  });

  $("connect").onclick = () => {
    const code = ($("code").value || "").replace(/\D/g, "");
    if (code.length !== 9) { toast("请输入 9 位连接码"); return; }
    const server = ($("server").value || "").trim();
    setGate("warn", "正在连接…");
    send({ t: "connect", code, server });
    setTimeout(() => { if (!connected) setGate("warn", "正在打洞，请稍候…"); }, 1500);
  };

  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && connected) send({ t: "keyframe" });
  });
  window.addEventListener("resize", () => { if (frameW) scheduleDraw(); });

  /* ------------------------------------------------------------------ start */
  connectWS();
  $("server").value = location.hostname ? "" : "";
  $("code").focus();
  canvas.focus();
})();
