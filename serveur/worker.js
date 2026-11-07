/* Sans-Trace : serveur de notifications push (Cloudflare Worker, gratuit).
   VERSION GITHUB : ne contient AUCUNE clé privée.
   La clé privée se met dans Cloudflare : Paramètres > Variables et secrets > Ajouter
   type « Secret », nom : VAPID_PRIVATE_JWK, valeur : le texte du fichier cle-a-coller-dans-cloudflare.txt
   Assistant IA (facultatif) : Paramètres > Liaisons > Ajouter > Workers AI, nom : AI */
const DEFAULTS = {
  FIREBASE_URL: "https://messagame-e5a03-default-rtdb.europe-west1.firebasedatabase.app",
  VAPID_PUBLIC: "BKHi6DV62mha-ZzNaNAYC1eQ0HCjIue_0aLlQXSQo3ycj_GILS7FroE8Iv_5uuMEyYi-uZxuZqLle3Y2l3qyWz8",
  VAPID_SUBJECT: "mailto:admin@sans-trace.app",
};
const b64u = (buf) => btoa(String.fromCharCode(...new Uint8Array(buf))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const enc = new TextEncoder();

async function vapidHeader(env, endpoint) {
  const key = await crypto.subtle.importKey("jwk", JSON.parse(env.VAPID_PRIVATE_JWK), { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]);
  const aud = new URL(endpoint).origin;
  let sub = String(env.VAPID_SUBJECT || "").trim();
  if (!/^(mailto:|https:)/.test(sub)) sub = sub.indexOf("@") !== -1 ? "mailto:" + sub : DEFAULTS.VAPID_SUBJECT;
  const head = b64u(enc.encode(JSON.stringify({ typ: "JWT", alg: "ES256" })));
  const body = b64u(enc.encode(JSON.stringify({ aud, exp: Math.floor(Date.now() / 1000) + 12 * 3600, sub: sub })));
  const sig = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, key, enc.encode(head + "." + body));
  return "vapid t=" + head + "." + body + "." + b64u(sig) + ", k=" + env.VAPID_PUBLIC;
}

const fbUrl = (env, k) => env.FIREBASE_URL.replace(/\/+$/, "") + "/s/" + encodeURIComponent(k) + ".json";

async function handleAI(req, env, cors) {
  const J = (o, st) => new Response(JSON.stringify(o), { status: st || 200, headers: Object.assign({ "Content-Type": "application/json" }, cors) });
  if (req.method === "GET") return J({ ok: true, mode: env.ANTHROPIC_API_KEY ? "Claude" : env.AI ? "Workers AI" : "none" });
  if (req.method !== "POST") return J({ error: "POST requis" }, 405);
  if (env.ALLOWED_ORIGIN && req.headers.get("Origin") !== env.ALLOWED_ORIGIN) return J({ error: "origine refusée" }, 403);
  let b;
  try { b = JSON.parse(await req.text()); } catch (e) { return J({ error: "requête invalide" }, 400); }
  const msgs = (Array.isArray(b.messages) ? b.messages : []).slice(-14).map((m) => ({ role: m.role === "assistant" ? "assistant" : "user", content: String(m.content || "").slice(0, 4000) })).filter((m) => m.content);
  if (!msgs.length) return J({ error: "message vide" }, 400);
  const system = String(b.system || "").slice(0, 800);
  try {
    if (env.ANTHROPIC_API_KEY) {
      const r = await fetch("https://api.anthropic.com/v1/messages", { method: "POST", headers: { "content-type": "application/json", "x-api-key": env.ANTHROPIC_API_KEY, "anthropic-version": "2023-06-01" }, body: JSON.stringify({ model: env.ANTHROPIC_MODEL || "claude-haiku-5-5", max_tokens: 700, system: system, messages: msgs }) });
      const d = await r.json();
      if (!r.ok) return J({ error: (d.error && d.error.message) || "erreur API" }, 502);
      return J({ reply: (d.content || []).map((c) => c.text || "").join("") });
    }
    if (env.AI) {
      const out = await env.AI.run("@cf/meta/llama-3.1-8b-instruct", { messages: [{ role: "system", content: system }].concat(msgs), max_tokens: 600 });
      return J({ reply: (out && out.response) || "" });
    }
    return J({ error: "IA non configurée sur le Worker" }, 501);
  } catch (e) { return J({ error: String((e && e.message) || e) }, 500); }
}

const worker = {
  async fetch(req, env0) {
    const env = Object.assign({}, DEFAULTS, env0);
    if (!env.VAPID_PRIVATE_JWK && req.method === "POST" && !new URL(req.url).pathname.replace(/\/+$/, "").endsWith("/ai")) return new Response(JSON.stringify({ error: "VAPID_PRIVATE_JWK manquant dans les secrets Cloudflare" }), { status: 500, headers: { "Access-Control-Allow-Origin": "*", "Content-Type": "application/json" } });
    const cors = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "POST, OPTIONS", "Access-Control-Allow-Headers": "Content-Type" };
    if (req.method === "OPTIONS") return new Response(null, { headers: cors });
    if (new URL(req.url).pathname.replace(/\/+$/, "").endsWith("/ai")) return handleAI(req, env, cors);
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

export default {
  async fetch(req, env) {
    try { return await worker.fetch(req, env); }
    catch (e) { return new Response(JSON.stringify({ error: String((e && e.message) || e) }), { status: 500, headers: { "Access-Control-Allow-Origin": "*", "Content-Type": "application/json" } }); }
  },
};
