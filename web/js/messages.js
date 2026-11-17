// Chat message construction. Pure and unit-tested.
//
// The browser runtime (transformers.js) applies the model's own chat template
// when the pipeline is given a `messages` array, so this module only builds the
// array, trims history and sanitises model output. It never renders a template
// by hand - that is the model's job and gets it right per architecture.

import { LIMITS } from "./config.js";

const ALLOWED_ROLES = new Set(["system", "user", "assistant"]);

/** Build the messages array for one turn. */
export function buildChatMessages({ system = "", history = [], user = "" } = {}) {
  const messages = [];
  if (typeof system === "string" && system.trim()) {
    messages.push({ role: "system", content: system.trim() });
  }
  if (Array.isArray(history)) {
    for (const message of history) {
      if (!message || typeof message !== "object") {
        continue;
      }
      if (!ALLOWED_ROLES.has(message.role)) {
        continue;
      }
      const content = typeof message.content === "string" ? message.content : "";
      if (!content && message.role !== "system") {
        continue;
      }
      messages.push({ role: message.role, content });
    }
  }
  messages.push({ role: "user", content: typeof user === "string" ? user : String(user ?? "") });
  return messages;
}

/** Keep system messages plus the last `maxTurns` user/assistant messages. */
export function trimHistory(messages, maxTurns = LIMITS.maxHistoryTurns) {
  if (!Array.isArray(messages)) {
    return [];
  }
  const limit = Number.isFinite(maxTurns) && maxTurns > 0 ? Math.floor(maxTurns) * 2 : messages.length;
  const system = messages.filter((m) => m.role === "system");
  const turns = messages.filter((m) => m.role !== "system");
  if (turns.length <= limit) {
    return [...system, ...turns];
  }
  return [...system, ...turns.slice(turns.length - limit)];
}

/**
 * Remove control/special tokens a raw model may leak into text (for example
 * `<|im_end|>` or a stray `<think>` block) so the UI shows clean text.
 */
export function stripControlTokens(text) {
  return String(text ?? "")
    .replace(/<\|[^|>]*\|>/g, "")
    .replace(/<\/?think>/gi, "")
    .replace(/<\|?endoftext\|?>/gi, "");
}

/** Clamp a prompt to the configured character limit. */
export function clampPrompt(text) {
  const s = String(text ?? "");
  return s.length > LIMITS.maxPromptChars ? s.slice(0, LIMITS.maxPromptChars) : s;
}

/** True when the text looks like it ends mid control token (hold it back). */
export function endsWithPartialControlToken(text) {
  const s = String(text ?? "");
  const lastOpen = s.lastIndexOf("<|");
  if (lastOpen === -1) {
    return false;
  }
  return s.indexOf("|>", lastOpen) === -1;
}
