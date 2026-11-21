// Run the browser upload queue against controllable transfer and preparation responses.
const assert = require("node:assert/strict");
const { File } = require("node:buffer");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { test } = require("node:test");
const vm = require("node:vm");

const source = readFileSync(join(__dirname, "../../web/pages/files.js"), "utf8");
const upload = source.slice(source.indexOf("async function prepareUploadedEpub("), source.indexOf("function showFailedUploadsBanner"));
const reserve = source.slice(source.indexOf("function reserveAvailableUploadFilename"), source.indexOf("async function fetchExistingUploadNames"));
const cancel = source.slice(source.indexOf("function handleCancelUploadModal"), source.indexOf("function restoreAfterCancel"));
const settings = source.slice(source.indexOf("const UPLOAD_SETTINGS_STORAGE_KEY"), source.indexOf("function updateUploadSettingsPersistence"));

async function run(options = {}) {
  const records = [];
  const elements = new Map();
  function element(id) {
    if (!elements.has(id)) elements.set(id, { style: {}, classList: { add() {}, remove() {} } });
    return elements.get(id);
  }
  element("fileInput").files = (options.names ?? ["One.epub"]).map(name => new File(["book"], name));
  const noop = () => {};
  let resolveCapability;
  const capability = new Promise(resolve => { resolveCapability = resolve; });
  function receiveStatus() {
    element("prepareFirstOpenToggle").disabled = !!options.unsupported;
    resolveCapability(!options.unsupported);
  }
  element("prepareFirstOpenToggle").disabled = true;
  if (!options.delayedStatus) receiveStatus();
  const state = {
    File, URLSearchParams, AbortController, clearLog: noop, showLog: noop, log: noop, logError: noop,
    console: { error: noop, warn: noop, log: noop }, exportLogCheckbox: null,
    document: { getElementById: element }, currentPath: "/Books & Comics/",
    currentUploadWs: null, currentUploadXhr: null, currentPreparation: null,
    readerSupportsPreparation: capability,
    DEFAULT_JPEG_QUALITY: 85, X_DEFAULT_REFERENCE_CHARACTERS_PER_PAGE: 1500,
    localStorage: { getItem: () => JSON.stringify(options.saved ?? null) },
    normalizedReferenceCharactersPerPage: value => value, setQualityPreset: noop,
    setDeviceTarget: noop, setHandedness: noop, setOverlap: noop, toggleConvertOptions: noop,
    operationCancelled: false, isUploadInProgress: false, uploadGeneration: 0,
    failedUploadsGlobal: [], closeUploadModal: noop, showFailedUploadsBanner: noop,
    showWebAlert: async message => records.push(["notice", message]),
    window: { location: { reload: () => records.push(["reload"]) } },
    setTimeout: cb => cb(), fetchExistingUploadNames: async () => new Set(options.existing),
    convertEpubFile: async file => {
      if (options.convertFails) throw new Error("unreadable");
      return file;
    },
    uploadFileWebSocket: async file => {
      if (options.http) throw new Error("WebSocket connection failed");
      if (options.uploadFails) throw new Error("disk full");
      records.push(["saved", file.name]);
      if (options.delayedStatus) receiveStatus();
    },
    uploadFileHTTP: async file => records.push(["http-saved", file.name]),
    fetch: async (url, request) => {
      assert.equal(url, "/api/prepare-uploaded-epub");
      assert.equal(request.method, "POST");
      records.push(["prepare", request.body.get("path")]);
      if (options.cancel) {
        state.handleCancelUploadModal();
        assert.equal(request.signal.aborted, true);
        throw new Error("aborted");
      }
      return { ok: !options.skip, json: async () => ({ status: "ready" }) };
    },
  };
  vm.runInNewContext(settings + reserve + cancel + upload, state);
  state.restoreUploadSettingsFromStorage();
  element("convertBeforeUpload").checked = options.optimize ?? true;
  if (options.prepare !== undefined) element("prepareFirstOpenToggle").checked = options.prepare;
  await state.uploadFile();
  // uploadFile starts the queue; drain its promised uploads and follow-ups.
  await new Promise(resolve => setImmediate(resolve));
  return { records, failed: state.failedUploadsGlobal };
}

test("older preferences inherit the default and a remembered opt-out takes precedence", async () => {
  const legacy = await run({ saved: { rememberSettings: true, quality: 65 } });
  assert.ok(legacy.records.some(([type]) => type === "prepare"));
  const optedOut = await run({ saved: { rememberSettings: true, prepareFirstOpen: false } });
  assert.ok(!optedOut.records.some(([type]) => type === "prepare"));
});

test("a slow capability response still prepares the saved optimized EPUB", async () => {
  const { records } = await run({ delayedStatus: true });
  assert.deepEqual(records.map(([type]) => type), ["saved", "prepare", "reload"]);
});

test("both transports prepare the final name after acknowledgement, before the next file", async () => {
  for (const http of [false, true]) {
    const { records } = await run({ http, existing: ["one.epub"], names: ["One.epub", "Two.txt"] });
    const saved = http ? "http-saved" : "saved";
    assert.deepEqual(records, [[saved, "One (2).epub"], ["prepare", "/Books & Comics/One (2).epub"], [saved, "Two.txt"], ["reload"]]);
  }
});

test("plain, unsupported, opted-out, conversion fallback, and failed uploads skip preparation", async () => {
  for (const options of [{ optimize: false }, { unsupported: true }, { prepare: false }, { convertFails: true }, { uploadFails: true }]) {
    const { records, failed } = await run(options);
    assert.ok(!records.some(([type]) => type === "prepare"));
    assert.equal(failed.length, options.uploadFails ? 1 : 0);
    assert.equal(records.some(([type]) => type === "notice"), !!options.convertFails);
  }
});

test("unavailable preparation keeps both uploaded files and explains first-open fallback", async () => {
  const { records, failed } = await run({ skip: true, names: ["One.epub", "Two.epub"] });
  assert.equal(failed.length, 0);
  assert.deepEqual(records.map(([type]) => type), ["saved", "prepare", "saved", "prepare", "notice", "reload"]);
  assert.match(records[4][1], /One\.epub\nTwo\.epub/);
});

test("cancelling during preparation keeps the saved book and stops a batch", async () => {
  const { records, failed } = await run({ cancel: true, names: ["One.epub", "Two.epub"] });
  assert.equal(failed.length, 0);
  assert.deepEqual(records.map(([type]) => type), ["saved", "prepare", "notice", "reload"]);
  assert.match(records[2][1], /One\.epub is saved/);
});
