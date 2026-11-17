// Pure formatting helpers. No browser dependency; fully unit-tested.

const BYTE_UNITS = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];

/**
 * Format a byte count with binary units. Returns "unknown" for missing or
 * invalid input rather than a misleading 0 B.
 * @param {number|null|undefined} bytes
 * @param {{decimals?: number}} [opts]
 */
export function formatBytes(bytes, opts = {}) {
  if (bytes == null || typeof bytes !== "number" || !Number.isFinite(bytes) || bytes < 0) {
    return "unknown";
  }
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < BYTE_UNITS.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const decimals =
    typeof opts.decimals === "number" ? opts.decimals : unit === 0 ? 0 : value < 10 ? 2 : value < 100 ? 1 : 0;
  return `${value.toFixed(decimals)} ${BYTE_UNITS[unit]}`;
}

/** Format a millisecond duration compactly. */
export function formatDuration(ms) {
  if (ms == null || typeof ms !== "number" || !Number.isFinite(ms) || ms < 0) {
    return "unknown";
  }
  if (ms < 1000) {
    return `${Math.round(ms)} ms`;
  }
  const seconds = ms / 1000;
  if (seconds < 60) {
    return `${seconds.toFixed(seconds < 10 ? 2 : 1)} s`;
  }
  const minutes = Math.floor(seconds / 60);
  const remSeconds = Math.round(seconds - minutes * 60);
  if (minutes < 60) {
    return `${minutes} m ${remSeconds} s`;
  }
  const hours = Math.floor(minutes / 60);
  return `${hours} h ${minutes - hours * 60} m`;
}

/** Tokens per second, or null when it cannot be computed. */
export function tokensPerSecond(tokens, ms) {
  if (!Number.isFinite(tokens) || !Number.isFinite(ms) || tokens <= 0 || ms <= 0) {
    return null;
  }
  return tokens / (ms / 1000);
}

/** Format a throughput value, or "n/a". */
export function formatRate(value, unit = "tok/s") {
  if (value == null || !Number.isFinite(value) || value <= 0) {
    return "n/a";
  }
  return `${value.toFixed(value < 10 ? 2 : 1)} ${unit}`;
}

/** Clamp a number into [lo, hi]; NaN falls back to lo. */
export function clamp(value, lo, hi) {
  const n = Number(value);
  if (!Number.isFinite(n)) {
    return lo;
  }
  return Math.min(hi, Math.max(lo, n));
}

/** Format a 0..1 ratio as a percentage. */
export function formatPercent(ratio, decimals = 0) {
  if (!Number.isFinite(ratio)) {
    return "unknown";
  }
  return `${(ratio * 100).toFixed(decimals)}%`;
}

/** Count of logical CPU threads, or "unknown". */
export function formatCount(n) {
  if (!Number.isFinite(n) || n <= 0) {
    return "unknown";
  }
  return String(Math.trunc(n));
}

/** Normalise an unknown string value to a trimmed string or a fallback. */
export function asLabel(value, fallback = "unknown") {
  if (typeof value === "string" && value.trim()) {
    return value.trim();
  }
  return fallback;
}
