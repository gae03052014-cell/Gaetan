// service-worker.js

const CACHE_NAME = 'sans-trace-v1';

// Installation du Service Worker
self.addEventListener('install', (event) => {
  self.skipWaiting();
});

// Activation et prise de contrôle immédiate
self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

// Gestion des notifications reçues via un serveur push (si applicable)
self.addEventListener('push', (event) => {
  const data = event.data ? event.data.json() : {};
  const title = data.title || "Sans Trace 🔒";
  const options = {
    body: data.body || "Nouveau message chiffré reçu sans laisser de trace.",
    icon: "icon.png",
    badge: "icon.png",
    vibrate: [200, 100, 200]
  };
  event.waitUntil(self.registration.showNotification(title, options));
});

// Action lorsque l'utilisateur clique sur la notification
self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  event.waitUntil(
    clients.matchAll({ type: 'window', includeUncontrolled: true }).then((clientList) => {
      // Si l'application est déjà ouverte, on la met au premier plan
      for (const client of clientList) {
        if ('focus' in client) {
          return client.focus();
        }
      }
      // Sinon, on ouvre une nouvelle fenêtre
      if (clients.openWindow) {
        return clients.openWindow('/');
      }
    })
  );
});
