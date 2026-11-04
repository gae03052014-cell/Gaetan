/* Sans-Trace : serveur de notifications push (Cloudflare Workers, gratuit).
   Variables à définir dans Cloudflare :
     FIREBASE_URL        (texte)  https://messagame-e5a03-default-rtdb.europe-west1.firebasedatabase.app
     VAPID_PUBLIC        (texte)  la clé publique (voir vapid-privee-A-GARDER-SECRETE.txt)
     VAPID_PRIVATE_JWK   (SECRET) la ligne JSON de la clé privée
     VAPID_SUBJECT       (texte)  mailto:ton-adresse@exemple.com (ou https://ton-site)
*/
const b64u = (buf) => btoa(String.fromCharCode(...new Uint8Array(buf))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const enc = new TextEncoder();

async function vapidHeader(env, endpoint) {
  const key = await crypto.subtle.importKey("jwk", JSON.parse(env.VAPID_PRIVATE_JWK), { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]);
  const aud = new URL(endpoint).origin;
  const head = b64u(enc.encode(JSON.stringify({ typ: "JWT", alg: "ES256" })));
  const body = b64u(enc.encode(JSON.stringify({ aud, exp: Math.floor(Date.now() / 1000) + 12 * 3600, sub: env.VAPID_SUBJECT || "mailto:admin@example.com" })));
  const sig = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, key, enc.encode(head + "." + body));
  return "vapid t=" + head + "." + body + "." + b64u(sig) + ", k=" + env.VAPID_PUBLIC;
}

const fbUrl = (env, k) => env.FIREBASE_URL.replace(/\/+$/, "") + "/s/" + encodeURIComponent(k) + ".json";

export default {
  async fetch(req, env) {
    const cors = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "POST, OPTIONS", "Access-Control-Allow-Headers": "Content-Type" };
    if (req.method === "OPTIONS") return new Response(null, { headers: cors });
    if (req.method !== "POST") return new Response("Sans-Trace push OK", { headers: cors });
    let body;
    try { body = JSON.parse(await req.text()); } catch (e) { return new Response("bad request", { status: 400, headers: cors }); }
    const to = (Array.isArray(body.to) ? body.to : []).filter((x) => typeof x === "string").slice(0, 30);
    let sent = 0;
    for (const id of to) {
      let list = [];
      try {
        const r = await fetch(fbUrl(env, "push:" + id));
        let v = await r.json();
        if (typeof v === "string") v = JSON.parse(v);
        list = Array.isArray(v) ? v : [];
      } catch (e) { continue; }
      const keep = [];
      for (const sub of list) {
        try {
          const res = await fetch(sub.endpoint, { method: "POST", headers: { Authorization: await vapidHeader(env, sub.endpoint), TTL: "86400", Urgency: "high", "Content-Length": "0" } });
          if (res.status === 404 || res.status === 410) continue; // abonnement mort : on l'oublie
          if (res.ok) sent++;
        } catch (e) { /* on garde l'abonnement */ }
        keep.push(sub);
      }
      if (keep.length !== list.length) {
        await fetch(fbUrl(env, "push:" + id), { method: "PUT", body: JSON.stringify(JSON.stringify(keep)) }).catch(() => {});
      }
    }
    return new Response(JSON.stringify({ sent }), { headers: Object.assign({ "Content-Type": "application/json" }, cors) });
  },
};
