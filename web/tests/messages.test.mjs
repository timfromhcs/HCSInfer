import test from "node:test";
import assert from "node:assert/strict";
import { buildChatMessages, trimHistory, stripControlTokens, clampPrompt, endsWithPartialControlToken } from "../js/messages.js";

test("buildChatMessages orders system, history, then the new user turn", () => {
  const messages = buildChatMessages({
    system: "  be brief  ",
    history: [
      { role: "user", content: "hi" },
      { role: "assistant", content: "hello" },
    ],
    user: "and now?",
  });
  assert.deepEqual(messages, [
    { role: "system", content: "be brief" },
    { role: "user", content: "hi" },
    { role: "assistant", content: "hello" },
    { role: "user", content: "and now?" },
  ]);
});

test("buildChatMessages drops unknown roles and empty turns", () => {
  const messages = buildChatMessages({
    history: [
      { role: "tool", content: "x" },
      { role: "assistant", content: "" },
      { role: "user", content: "keep" },
      null,
    ],
    user: "q",
  });
  assert.deepEqual(messages, [
    { role: "user", content: "keep" },
    { role: "user", content: "q" },
  ]);
});

test("buildChatMessages omits an empty system prompt", () => {
  const messages = buildChatMessages({ system: "   ", user: "q" });
  assert.deepEqual(messages, [{ role: "user", content: "q" }]);
});

test("trimHistory keeps system messages and the newest turns", () => {
  const messages = [
    { role: "system", content: "s" },
    { role: "user", content: "1" },
    { role: "assistant", content: "2" },
    { role: "user", content: "3" },
    { role: "assistant", content: "4" },
  ];
  const trimmed = trimHistory(messages, 1);
  assert.deepEqual(trimmed, [
    { role: "system", content: "s" },
    { role: "user", content: "3" },
    { role: "assistant", content: "4" },
  ]);
  assert.equal(trimHistory(messages, 10).length, 5);
  assert.deepEqual(trimHistory(null), []);
});

test("stripControlTokens removes special tokens and think blocks", () => {
  assert.equal(stripControlTokens("<|im_end|>hello<|endoftext|>"), "hello");
  assert.equal(stripControlTokens("<think>reason</think>answer"), "reasonanswer");
  assert.equal(stripControlTokens("plain"), "plain");
  assert.equal(stripControlTokens(null), "");
});

test("clampPrompt bounds the prompt length", () => {
  const long = "x".repeat(50);
  assert.equal(clampPrompt(long).length, 50);
  const huge = "y".repeat(100000);
  assert.equal(clampPrompt(huge).length, 20000);
  assert.equal(clampPrompt(null), "");
});

test("endsWithPartialControlToken detects an unclosed control token", () => {
  assert.equal(endsWithPartialControlToken("abc<|"), true);
  assert.equal(endsWithPartialControlToken("abc"), false);
  assert.equal(endsWithPartialControlToken("a<|x|>"), false);
});
