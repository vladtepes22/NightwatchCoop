// Nightwatch mod downloads: serves the R2 bucket's files read-only (GET / HEAD), nothing else.
// The launcher reads <worker>/nocturne/manifest.json and downloads the zip it names (checked by SHA-256).
// Deploy: cd tools/mods-cdn && npx wrangler deploy (the bucket is bound in wrangler.toml).
export default {
  async fetch(request, env) {
    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405, headers: { Allow: "GET, HEAD" } });
    }
    const key = decodeURIComponent(new URL(request.url).pathname.slice(1));
    if (!key || key.includes("..") || key.endsWith("/")) return new Response("Not found", { status: 404 });

    const object = request.method === "HEAD"
      ? await env.MODS.head(key)
      : await env.MODS.get(key, { range: request.headers, onlyIf: request.headers });
    if (!object) return new Response("Not found", { status: 404 });

    const headers = new Headers();
    object.writeHttpMetadata(headers);
    headers.set("ETag", object.httpEtag);
    headers.set("Accept-Ranges", "bytes");
    // A manifest changes with every release; a zip never does (a new version gets a new name).
    headers.set("Cache-Control", key.endsWith(".json") ? "public, max-age=300" : "public, max-age=31536000, immutable");

    if (request.method === "HEAD") {
      headers.set("Content-Length", String(object.size));
      return new Response(null, { headers });
    }
    if (!("body" in object)) return new Response(null, { status: 304, headers }); // onlyIf matched: not modified
    let status = 200;
    // R2 reports a range on every read: only a request that asked for one gets 206.
    if (request.headers.has("range") && object.range && "offset" in object.range) {
      const start = object.range.offset, end = start + object.range.length - 1;
      headers.set("Content-Range", `bytes ${start}-${end}/${object.size}`);
      status = 206;
    }
    return new Response(object.body, { status, headers });
  },
};
