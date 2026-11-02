/* Service worker Sans-Trace : notifications système + mode hors ligne léger */
const CACHE = "sans-trace-v5";
const SHELL = ["./", "./index.html", "./manifest.json", "./icon-192.png", "./icon-512.png", "./badge.png"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).catch(() => {}));
  self.skipWaiting();
});

self.addEventListener("activate", (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

// réseau d'abord, cache en secours (uniquement notre propre site)
self.addEventListener("fetch", (e) => {
  const req = e.request;
  if (req.method !== "GET" || new URL(req.url).origin !== self.location.origin) return;
  e.respondWith(
    fetch(req)
      .then((res) => { const copy = res.clone(); caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {}); return res; })
      .catch(() => caches.match(req).then((r) => r || caches.match("./index.html")))
  );
});

// Notification poussée par un serveur (FCM / Web Push) : s'affiche même appli fermée
self.addEventListener("push", (e) => {
  let d = {};
  try { d = e.data ? e.data.json() : {}; } catch (err) { d = { body: e.data && e.data.text() }; }
  const n = d.notification || d;
  const title = n.title || "Sans-Trace";
  e.waitUntil(self.registration.showNotification(title, {
    body: n.body || "Nouveau message",
    tag: n.tag || "push",
    renotify: true,
    icon: "icon-192.png",
    badge: "badge.png",
    vibrate: [200, 100, 200],
    data: { url: "./" },
  }));
});

// Toucher la notification : ouvre / ramène l'appli au premier plan
self.addEventListener("notificationclick", (e) => {
  e.notification.close();
  const url = (e.notification.data && e.notification.data.url) || "./";
  e.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((list) => {
      for (const c of list) { if ("focus" in c) return c.focus(); }
      return self.clients.openWindow(url);
    })
  );
});
