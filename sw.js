/* Service worker Sans-Trace v6 : notifications push (appli fermée), actions, hors ligne léger */
const CACHE = "sans-trace-v7";
const SHELL = ["./", "./index.html", "./manifest.json", "./icon-192.png", "./icon-512.png", "./badge.png"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).catch(() => {}));
  self.skipWaiting();
});
self.addEventListener("activate", (e) => {
  e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k !== CACHE && k !== "st-cfg").map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener("fetch", (e) => {
  const req = e.request;
  if (req.method !== "GET" || new URL(req.url).origin !== self.location.origin) return;
  e.respondWith(
    fetch(req)
      .then((res) => { const copy = res.clone(); caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {}); return res; })
      .catch(() => caches.match(req).then((r) => r || caches.match("./index.html")))
  );
});

async function readCfg() {
  try { const c = await caches.open("st-cfg"); const r = await c.match("cfg.json"); return r ? await r.json() : null; } catch (e) { return null; }
}
const fbKey = (cfg, k) => cfg.fb.replace(/\/+$/, "") + "/s/" + encodeURIComponent(k) + ".json";
const convId = (a, b) => [a, b].sort().join("__");

function dndActive(d) {
  if (!d || !d.on) return false;
  const t = (x) => { const p = String(x || "00:00").split(":"); return (parseInt(p[0], 10) || 0) * 60 + (parseInt(p[1], 10) || 0); };
  const now = new Date(); const m = now.getHours() * 60 + now.getMinutes(); const a = t(d.from), b = t(d.to);
  return a <= b ? m >= a && m < b : m >= a || m < b;
}
async function show(n) {
  const cfgD = await readCfg();
  const quiet = cfgD && dndActive(cfgD.dnd);
  const isCall = n.tag === "call";
  const chat = (n.tag || "").indexOf("msg-") === 0;
  const opts = {
    body: n.body, tag: n.tag || "msg", renotify: true, icon: "icon-192.png", badge: "badge.png",
    vibrate: isCall ? [400, 200, 400, 200, 400] : [200, 100, 200], requireInteraction: isCall,
    timestamp: n.ts || Date.now(), data: { url: "./", tag: n.tag },
  };
  if (quiet) { opts.silent = true; delete opts.vibrate; opts.requireInteraction = false; }
  if (chat) opts.actions = [{ action: "reply", type: "text", title: "Répondre", placeholder: "Ta réponse…" }, { action: "read", title: "Marquer comme lu" }];
  return self.registration.showNotification(n.title || "Sans-Trace", opts);
}

// Push reçu (même appli fermée) : on lit les notifications en attente dans Firebase
self.addEventListener("push", (e) => {
  e.waitUntil((async () => {
    const cfg = await readCfg();
    let items = [];
    if (cfg && cfg.fb && cfg.me) {
      try {
        const r = await fetch(fbKey(cfg, "notif:" + cfg.me));
        let v = await r.json();
        if (typeof v === "string") v = JSON.parse(v);
        items = Array.isArray(v) ? v : [];
        if (items.length) await fetch(fbKey(cfg, "notif:" + cfg.me), { method: "DELETE" });
      } catch (err) { /* réseau */ }
    }
    const wins = await self.clients.matchAll({ type: "window", includeUncontrolled: true });
    if (wins.some((c) => c.visibilityState === "visible" && c.focused)) return; // l'appli est ouverte : elle prévient elle-même
    if (!items.length) items = [{ title: "Sans-Trace", body: "Nouveau message", tag: "msg" }];
    const by = {};
    items.forEach((i) => { const t = i.tag || "msg"; (by[t] = by[t] || []).push(i); });
    for (const t in by) {
      const l = by[t], last = l[l.length - 1];
      await show({ title: last.title, body: l.length > 1 ? l.length + " nouveaux messages · " + last.body : last.body, tag: t, ts: last.ts });
    }
  })());
});

self.addEventListener("notificationclick", (e) => {
  const n = e.notification;
  const tag = (n.data && n.data.tag) || n.tag || "";
  const other = tag.indexOf("msg-") === 0 ? tag.slice(4) : null;
  n.close();
  e.waitUntil((async () => {
    const cfg = await readCfg();
    if (e.action === "read" && other && cfg) {
      const cid = convId(cfg.me, other);
      try {
        await fetch(fbKey(cfg, "unread:" + cid + ":" + cfg.me), { method: "PUT", body: JSON.stringify("0") });
        await fetch(fbKey(cfg, "read:" + cid + ":" + cfg.me), { method: "PUT", body: JSON.stringify(String(Date.now())) });
      } catch (err) { /* réseau */ }
      return;
    }
    const wins = await self.clients.matchAll({ type: "window", includeUncontrolled: true });
    if (e.action === "reply" && other && e.reply) {
      const payload = { type: "reply", to: other, text: e.reply };
      if (wins.length) { wins[0].postMessage(payload); return; }
      return self.clients.openWindow("./?reply=" + encodeURIComponent(JSON.stringify(payload)));
    }
    const open = other ? { type: "open", to: other } : null;
    if (wins.length) { const c = wins[0]; if (open) c.postMessage(open); return c.focus(); }
    return self.clients.openWindow(open ? "./?open=" + encodeURIComponent(other) : "./");
  })());
});
