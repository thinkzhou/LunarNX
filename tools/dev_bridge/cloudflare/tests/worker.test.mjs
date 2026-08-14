import test from "node:test";
import assert from "node:assert/strict";
import worker from "../src/index.js";

class MemoryNamespace {
  constructor() { this.objects = new Map(); }
  async put(key, value, options = {}) {
    const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : value;
    this.objects.set(key, { bytes, options });
  }
  async get(key, type = "text") {
    const item = this.objects.get(key);
    if (!item) return null;
    if (type === "arrayBuffer") {
      if (item.bytes instanceof Uint8Array) return item.bytes.buffer;
      return new TextEncoder().encode(item.bytes).buffer;
    }
    if (typeof item.bytes === "string") return item.bytes;
    return new TextDecoder().decode(item.bytes);
  }
  async getWithMetadata(key, type = "text") {
    const item = this.objects.get(key);
    if (!item) return { value: null, metadata: null };
    return { value: await this.get(key, type), metadata: item.options.metadata || null };
  }
}

function env() {
  return {
    ARTIFACTS: new MemoryNamespace(),
    DEVICE_UPLOAD_TOKEN: "secret",
    ADMIN_TOKEN: "admin-secret",
  };
}

test("health endpoint is public", async () => {
  const response = await worker.fetch(new Request("https://bridge.test/health"), env());
  assert.equal(response.status, 200);
  assert.equal((await response.json()).service, "lunarnx-dev-bridge");
});

test("log upload requires the device token", async () => {
  const response = await worker.fetch(new Request("https://bridge.test/dev/logs", {
    method: "POST",
    body: "hello",
  }), env());
  assert.equal(response.status, 401);
});

test("authorized log upload stores a bounded object", async () => {
  const testEnv = env();
  const response = await worker.fetch(new Request("https://bridge.test/dev/logs", {
    method: "POST",
    headers: {
      authorization: "Bearer secret",
      "x-lunarnx-device": "my switch",
      "x-lunarnx-build": "dev/123",
      "x-lunarnx-commit": "abc1234",
    },
    body: "test log",
  }), testEnv);
  assert.equal(response.status, 201);
  const result = await response.json();
  assert.equal(result.size, 8);
  assert.match(result.log_id, /^logs\/\d{4}-\d{2}-\d{2}\/.+\.log$/);
  assert.ok(testEnv.ARTIFACTS.objects.has(result.log_id));
  assert.ok(testEnv.ARTIFACTS.objects.has("logs/latest.json"));
  const pointer = JSON.parse(await testEnv.ARTIFACTS.get("logs/latest.json"));
  assert.equal(pointer.device, "my_switch");
  assert.equal(pointer.build, "dev_123");
  assert.equal(pointer.commit, "abc1234");
});

test("admin can download the latest uploaded log", async () => {
  const testEnv = env();
  await worker.fetch(new Request("https://bridge.test/dev/logs", {
    method: "POST",
    headers: { authorization: "Bearer secret" },
    body: "latest log",
  }), testEnv);
  const response = await worker.fetch(
    new Request("https://bridge.test/admin/logs/latest", {
      headers: { authorization: "Bearer admin-secret" },
    }), testEnv);
  assert.equal(response.status, 200);
  assert.equal(await response.text(), "latest log");
});

test("build downloads only accept sha256 object names", async () => {
  const response = await worker.fetch(
    new Request("https://bridge.test/dev/builds/latest.nro"), env());
  assert.equal(response.status, 400);
});

test("version index and individual manifests are downloadable", async () => {
  const testEnv = env();
  const release = {
    version: "v0.2.0-dev.1",
    notes: "Adds remote log upload",
    download_url: "https://bridge.test/dev/builds/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nro",
  };
  await testEnv.ARTIFACTS.put("builds/index.json", JSON.stringify({ schema: 1, versions: [release] }));
  await testEnv.ARTIFACTS.put("builds/versions/v0.2.0-dev.1.json", JSON.stringify(release));

  const indexResponse = await worker.fetch(new Request("https://bridge.test/dev/versions.json"), testEnv);
  assert.equal(indexResponse.status, 200);
  assert.deepEqual((await indexResponse.json()).versions, [release]);

  const versionResponse = await worker.fetch(new Request("https://bridge.test/dev/versions/v0.2.0-dev.1"), testEnv);
  assert.equal(versionResponse.status, 200);
  assert.deepEqual(await versionResponse.json(), release);
});

test("version manifests reject unsafe version names", async () => {
  const response = await worker.fetch(new Request("https://bridge.test/dev/versions/%2E%2E%2Fsecret"), env());
  assert.equal(response.status, 400);
});
