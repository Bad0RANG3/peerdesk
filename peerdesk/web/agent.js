/* PeerDesk agent panel: shows the code, session state and capture settings. */
(() => {
  "use strict";
  const $ = (id) => document.getElementById(id);
  let ws = null;
  let state = {};

  function fmtCode(v) {
    const d = (v || "").replace(/\D/g, "");
    return d.replace(/(.{3})(?=.)/g, "$1 ") || "— — —";
  }

  function toast(text) {
    const el = $("toast");
    el.textContent = text;
    el.classList.add("show");
    clearTimeout(toast._t);
    toast._t = setTimeout(() => el.classList.remove("show"), 1600);
  }

  function log(text, cls = "") {
    const el = $("log");
    const row = document.createElement("div");
    const t = new Date().toLocaleTimeString("zh-CN", { hour12: false });
    row.innerHTML = `<span class="t">${t}</span><span class="${cls}">${text}</span>`;
    el.appendChild(row);
    while (el.childElementCount > 200) el.removeChild(el.firstChild);
    el.scrollTop = el.scrollHeight;
  }

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  }

  function setStatus(kind, text, path) {
    $("dot").className = "dot " + kind;
    $("statusText").textContent = text;
    $("pathText").textContent = path || "";
    const pill = $("modePill");
    if (kind === "live") { pill.textContent = path && path.includes("relay") ? "中继" : "已连接"; pill.className = "pill green"; }
    else if (kind === "warn") { pill.textContent = "打洞中"; pill.className = "pill amber"; }
    else { pill.textContent = "待机"; pill.className = "pill"; }
  }

  function connectWS() {
    ws = new WebSocket(`ws://${location.host}/ws`);
    ws.onmessage = (ev) => {
      if (typeof ev.data !== "string") return;
      let msg; try { msg = JSON.parse(ev.data); } catch { return; }
      handle(msg);
    };
    ws.onclose = () => {
      log("本地桥断开，3 秒后重连", "warn");
      setTimeout(connectWS, 3000);
    };
    ws.onopen = () => log("本地桥就绪");
  }

  function handle(msg) {
    switch (msg.t) {
      case "ready":
        state = msg;
        $("code").textContent = fmtCode(msg.code);
        $("serverPill").textContent = msg.server || "—";
        if (msg.name) document.title = `${msg.name} · PeerDesk`;
        fillMonitors(msg.monitors || []);
        applySettings(msg.settings || {});
        log(`注册成功，连接码 ${fmtCode(msg.code)}`, "ok");
        break;
      case "link":
        if (msg.state === "direct") setStatus("live", "控制端已连接 · P2P 直连", msg.path);
        else if (msg.state === "relay") setStatus("live", "控制端已连接 · 中继转发", msg.path);
        else if (msg.state === "punching") setStatus("warn", "控制端接入中 · 打洞", msg.path);
        else if (msg.state === "closed") setStatus("", "未连接控制端");
        log(`链路：${msg.state}${msg.relay ? "（中继）" : ""}`);
        break;
      case "stats":
        log(`帧率 ${msg.fps} fps · 码率 ${msg.mbps} Mbps · 质量 ${msg.quality} · ${msg.kb} KB/帧`);
        break;
      case "notice":
        log(msg.text || "", "ok");
        toast(msg.text || "");
        break;
      case "settings":
        applySettings(msg.settings || {});
        break;
      case "error":
        log(msg.text || msg.reason || "错误", "warn");
        break;
      case "log":
        log(msg.text || "");
        break;
    }
  }

  function fillMonitors(list) {
    const sel = $("monitor");
    sel.innerHTML = "";
    (list.length ? list : [{ index: 0, w: 0, h: 0, primary: true }]).forEach((m) => {
      const opt = document.createElement("option");
      opt.value = m.index;
      opt.textContent = `显示器 ${m.index + 1}${m.primary ? "（主）" : ""} · ${m.w}×${m.h}`;
      sel.appendChild(opt);
    });
  }

  function applySettings(s) {
    if (s.monitor !== undefined) $("monitor").value = String(s.monitor);
    if (s.fps !== undefined) $("fps").value = String(s.fps);
    if (s.max_width !== undefined) $("width").value = String(s.max_width);
    if (s.quality !== undefined) $("quality").value = String(s.quality);
    if (s.clipboard !== undefined) $("allowClip").checked = !!s.clipboard;
    if (s.skip_static !== undefined) $("skipStatic").checked = !!s.skip_static;
  }

  function pushSettings() {
    send({
      t: "settings",
      settings: {
        monitor: +$("monitor").value || 0,
        fps: +$("fps").value || 30,
        max_width: +$("width").value || 1920,
        quality: +$("quality").value || 62,
        clipboard: $("allowClip").checked,
        skip_static: $("skipStatic").checked,
      },
    });
  }

  ["monitor", "fps", "width", "quality", "allowClip", "skipStatic"].forEach((id) => {
    $(id).addEventListener("change", pushSettings);
  });
  $("allowInput").addEventListener("change", () => {
    log($("allowInput").checked ? "已允许键鼠控制" : "已禁止键鼠控制", "warn");
    send({ t: "allow_input", v: $("allowInput").checked });
  });

  $("btnCopy").onclick = async () => {
    const text = ($("code").textContent || "").replace(/\s/g, "");
    try {
      await navigator.clipboard.writeText(text);
      toast("连接码已复制");
    } catch {
      const r = document.createRange();
      r.selectNodeContents($("code"));
      const sel = getSelection(); sel.removeAllRanges(); sel.addRange(r);
      document.execCommand("copy");
      toast("连接码已复制");
    }
  };

  $("btnRegen").onclick = () => {
    send({ t: "regen" });
    log("正在更换连接码…");
  };

  connectWS();
})();
