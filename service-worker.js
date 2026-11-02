const CACHE_NAME = 'sans-trace-v1';
const ASSETS_TO_CACHE = [
  '/',
  '/index.html'
];

// Installation du Service Worker et mise en cache des fichiers essentiels
self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      return cache.addAll(ASSETS_TO_CACHE);
    })
  );
  self.skipWaiting();
});

// Activation et nettoyage des anciens caches
self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((cacheNames) => {
      return Promise.all(
        cacheNames.map((cache) => {
          if (cache !== CACHE_NAME) {
            return caches.delete(cache);
          }
        })
      );
    })
  );
  self.clients.claim();
});

// Interception des requêtes réseau (Stratégie Network falling back to cache)
self.addEventListener('fetch', (event) => {
  // On évite de mettre en cache les requêtes non-GET ou externes (comme Firebase)
  if (event.request.method !== 'GET') return;

  event.respondWith(
    fetch(event.request)
      .then((response) => {
        // Si la requête réseau réussit, on met à jour le cache dynamiquement
        const responseClone = response.clone();
        caches.open(CACHE_NAME).then((cache) => {
          cache.put(event.request, responseClone);
        });
        return response;
      })
      .catch(() => {
        // Si le réseau échoue (ex: hors-ligne), on récupère depuis le cache
        return caches.match(event.request);
      })
  );
});
            
