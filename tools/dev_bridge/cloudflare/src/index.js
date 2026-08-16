const MAX_LOG_BYTES = 2 * 1024 * 1024;

function json(body, status = 200, headers = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      ...headers,
    },
  });
}

function safeLabel(value, fallback) {
  const normalized = (value || "").trim().replace(/[^A-Za-z0-9._-]/g, "_");
  return normalized.slice(0, 80) || fallback;
}

async function authorized(request, expected) {
  if (!expected) return false;
  const encoder = new TextEncoder();
  const [providedHash, expectedHash] = await Promise.all([
    crypto.subtle.digest("SHA-256", encoder.encode(request.headers.get("authorization") || "")),
    crypto.subtle.digest("SHA-256", encoder.encode(`Bearer ${expected}`)),
  ]);
  const providedBytes = new Uint8Array(providedHash);
  const expectedBytes = new Uint8Array(expectedHash);
  let difference = 0;
  for (let index = 0; index < expectedBytes.length; index += 1) {
    difference |= providedBytes[index] ^ expectedBytes[index];
  }
  return difference === 0;
}

async function serveObject(namespace, key, cacheControl, contentType, headers = {}) {
  const object = await namespace.getWithMetadata(key, "arrayBuffer");
  if (!object.value) return json({ error: "not_found" }, 404);

  return new Response(object.value, {
    headers: {
      "content-type": object.metadata?.contentType || contentType,
      "cache-control": cacheControl,
      ...headers,
    },
  });
}

async function buildDownloadHeaders(namespace, name) {
  const sha256 = name.slice(0, 64);
  let version = null;
  try {
    const indexText = await namespace.get("builds/index.json");
    const index = indexText ? JSON.parse(indexText) : null;
    const release = Array.isArray(index?.versions)
      ? index.versions.find((item) => item?.sha256 === sha256)
      : null;
    if (/^[A-Za-z0-9._-]{1,64}$/.test(release?.version || "")) {
      version = release.version;
    }
  } catch {
    // The immutable build remains downloadable if its optional index is bad.
  }
  const label = version || `build-${sha256.slice(0, 12)}`;
  return {
    "content-disposition": `attachment; filename="LunarNX-${label}.nro"`,
  };
}

async function uploadLog(request, env) {
  if (!(await authorized(request, env.DEVICE_UPLOAD_TOKEN))) {
    return json({ error: "unauthorized" }, 401);
  }

  const contentLength = Number(request.headers.get("content-length") || 0);
  if (contentLength > MAX_LOG_BYTES) {
    return json({ error: "log_too_large", max_bytes: MAX_LOG_BYTES }, 413);
  }

  const bytes = await request.arrayBuffer();
  if (bytes.byteLength === 0) return json({ error: "empty_log" }, 400);
  if (bytes.byteLength > MAX_LOG_BYTES) {
    return json({ error: "log_too_large", max_bytes: MAX_LOG_BYTES }, 413);
  }

  const now = new Date();
  const day = now.toISOString().slice(0, 10);
  const timestamp = now.toISOString().replace(/[:.]/g, "-");
  const device = safeLabel(request.headers.get("x-lunarnx-device"), "switch");
  const build = safeLabel(request.headers.get("x-lunarnx-build"), "unknown");
  const commit = safeLabel(request.headers.get("x-lunarnx-commit"), "unknown");
  const nonce = crypto.randomUUID();
  const key = `logs/${day}/${timestamp}-${device}-${build}-${nonce}.log`;

  await env.ARTIFACTS.put(key, bytes, {
    metadata: { contentType: "text/plain; charset=utf-8", device, build, commit },
  });
  await env.ARTIFACTS.put("logs/latest.json", JSON.stringify({
    log_id: key,
    size: bytes.byteLength,
    device,
    build,
    commit,
    uploaded_at: now.toISOString(),
  }), { metadata: { contentType: "application/json; charset=utf-8" } });
  return json({ ok: true, log_id: key, size: bytes.byteLength }, 201);
}

async function downloadLatestLog(request, env) {
  if (!(await authorized(request, env.ADMIN_TOKEN))) {
    return json({ error: "unauthorized" }, 401);
  }
  const pointer = await env.ARTIFACTS.get("logs/latest.json");
  if (!pointer) return json({ error: "not_found" }, 404);
  let metadata;
  try {
    metadata = JSON.parse(pointer);
  } catch {
    return json({ error: "invalid_log_pointer" }, 500);
  }
  if (!metadata.log_id || !String(metadata.log_id).startsWith("logs/")) {
    return json({ error: "invalid_log_pointer" }, 500);
  }
  const object = await env.ARTIFACTS.get(metadata.log_id, "arrayBuffer");
  if (!object) return json({ error: "not_found" }, 404);
  return new Response(object, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "content-disposition": `attachment; filename="${metadata.log_id.split("/").pop()}"`,
      "x-lunarnx-log-id": metadata.log_id,
      "x-lunarnx-device": metadata.device || "unknown",
      "x-lunarnx-build": metadata.build || "unknown",
      "x-lunarnx-commit": metadata.commit || "unknown",
      "cache-control": "no-store",
    },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/health") {
      return json({ ok: true, service: "lunarnx-dev-bridge" });
    }
    if (request.method === "GET" && url.pathname === "/dev/latest.json") {
      return serveObject(env.ARTIFACTS, "builds/latest.json", "no-store", "application/json; charset=utf-8");
    }
    if (request.method === "GET" && url.pathname === "/dev/versions.json") {
      return serveObject(env.ARTIFACTS, "builds/index.json", "no-store", "application/json; charset=utf-8");
    }
    if (request.method === "GET" && url.pathname.startsWith("/dev/versions/")) {
      const version = url.pathname.slice("/dev/versions/".length);
      if (!/^[A-Za-z0-9._-]{1,64}$/.test(version)) {
        return json({ error: "invalid_version" }, 400);
      }
      return serveObject(env.ARTIFACTS, `builds/versions/${version}.json`, "no-store", "application/json; charset=utf-8");
    }
    if (request.method === "GET" && url.pathname.startsWith("/dev/builds/")) {
      const name = url.pathname.slice("/dev/builds/".length);
      if (!/^[a-f0-9]{64}\.nro(?:\.gz)?$/.test(name)) {
        return json({ error: "invalid_build_name" }, 400);
      }
      const compressed = name.endsWith(".gz");
      const downloadHeaders = await buildDownloadHeaders(env.ARTIFACTS, name);
      return serveObject(
        env.ARTIFACTS,
        `builds/${name}`,
        "public, max-age=31536000, immutable",
        "application/octet-stream",
        compressed
          ? { ...downloadHeaders, "content-encoding": "gzip", "vary": "Accept-Encoding" }
          : downloadHeaders,
      );
    }
    if (request.method === "POST" && url.pathname === "/dev/logs") {
      return uploadLog(request, env);
    }
    if (request.method === "GET" && url.pathname === "/admin/logs/latest") {
      return downloadLatestLog(request, env);
    }
    return json({ error: "not_found" }, 404);
  },
};
