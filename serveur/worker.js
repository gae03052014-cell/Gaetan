// Sans-Trace : Worker de test (diagnostic)
// Sert à vérifier que Cloudflare déploie bien le code depuis GitHub.
// Une fois que l'adresse affiche « Sans-Trace push OK », on remet la version complète.

const ENTETES = {
  "content-type": "text/plain; charset=utf-8",
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, POST, OPTIONS",
  "access-control-allow-headers": "content-type, authorization",
  "cache-control": "no-store",
};

export default {
  async fetch(request) {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: ENTETES });
    }
    return new Response("Sans-Trace push OK", { status: 200, headers: ENTETES });
  },
};
