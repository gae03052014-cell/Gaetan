// Installation du service worker
self.addEventListener('install', (event) => {
  self.skipWaiting();
});

// Activation du service worker
self.addEventListener('activate', (event) => {
  event.waitUntil(clients.claim());
});
