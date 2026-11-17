import test from "node:test";
import assert from "node:assert/strict";
import { normaliseSettings, readSettings, writeSettings, resetSettings, DEFAULT_SETTINGS, SETTINGS_VERSION } from "../js/settings.js";
import { STORAGE_KEY } from "../js/config.js";

function fakeStore() {
  const map = new Map();
  return {
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: (k) => map.delete(k),
    _map: map,
  };
}

test("normaliseSettings fills defaults and rejects bad values", () => {
  const s = normaliseSettings({});
  assert.equal(s.version, SETTINGS_VERSION);
  assert.equal(s.theme, DEFAULT_SETTINGS.theme);
  assert.equal(s.advanced, false);
  assert.equal(s.maxNewTokens, DEFAULT_SETTINGS.maxNewTokens);
  const weird = normaliseSettings({ theme: "neon", maxNewTokens: 99999, temperature: -3, topP: 9, seed: -1 });
  assert.equal(weird.theme, "system");
  assert.equal(weird.maxNewTokens, 2048);
  assert.equal(weird.temperature, 0);
  assert.equal(weird.topP, 1);
  assert.equal(weird.seed, null);
});

test("normaliseSettings keeps valid values", () => {
  const s = normaliseSettings({
    theme: "dark",
    advanced: true,
    chatModel: "org/name",
    maxNewTokens: 128,
    temperature: 0.2,
    topP: 0.5,
    seed: 42,
    keepHistory: false,
  });
  assert.equal(s.theme, "dark");
  assert.equal(s.advanced, true);
  assert.equal(s.chatModel, "org/name");
  assert.equal(s.maxNewTokens, 128);
  assert.equal(s.temperature, 0.2);
  assert.equal(s.topP, 0.5);
  assert.equal(s.seed, 42);
  assert.equal(s.keepHistory, false);
});

test("readSettings returns defaults when absent or corrupt", () => {
  const store = fakeStore();
  assert.deepEqual(readSettings(store), normaliseSettings(null));
  store.setItem(STORAGE_KEY, "{ not json");
  assert.deepEqual(readSettings(store), normaliseSettings(null));
  assert.deepEqual(readSettings(null), normaliseSettings(null));
});

test("writeSettings round-trips through normalisation", () => {
  const store = fakeStore();
  assert.equal(writeSettings(store, { theme: "dark", maxNewTokens: 64 }), true);
  const back = readSettings(store);
  assert.equal(back.theme, "dark");
  assert.equal(back.maxNewTokens, 64);
  assert.equal(writeSettings(null, {}), false);
});

test("resetSettings restores defaults", () => {
  const store = fakeStore();
  writeSettings(store, { theme: "dark" });
  const fresh = resetSettings(store);
  assert.equal(fresh.theme, DEFAULT_SETTINGS.theme);
  assert.equal(readSettings(store).theme, DEFAULT_SETTINGS.theme);
});
