// Hugging Face Hub access helpers.
//
// URL builders and the pure response parsers are unit-tested with fixtures; the
// network call itself happens only in the browser. No prompt, image or model
// bytes are ever sent to any server other than the Hub, and only model
// *metadata* is fetched here (the inference runtime fetches the weights).

import { LIMITS } from "./config.js";

const HUB_API = "https://huggingface.co/api";

/** Encode a model id path-segment-wise so "org/name" keeps its slash. */
export function encodeModelId(id) {
  return String(id)
    .split("/")
    .map((segment) => encodeURIComponent(segment))
    .join("/");
}

/** Search URL for the Hub model API. */
export function modelSearchUrl(query, { limit = 20, task } = {}) {
  const params = new URLSearchParams();
  if (query && String(query).trim()) {
    params.set("search", String(query).trim());
  }
  params.set("limit", String(Math.min(Math.max(1, limit), LIMITS.maxSearchResults)));
  params.set("sort", "downloads");
  params.set("direction", "-1");
  if (task) {
    params.set("filter", task);
  }
  return `${HUB_API}/models?${params.toString()}`;
}

/** Info URL (with blob sizes) for one model. */
export function modelInfoUrl(id) {
  return `${HUB_API}/models/${encodeModelId(id)}?blobs=true`;
}

/** Parse a model-info response into a stable shape. */
export function parseModelInfo(json) {
  if (!json || typeof json !== "object") {
    return null;
  }
  const siblings = Array.isArray(json.siblings) ? json.siblings : [];
  let totalBytes = 0;
  let hasOnnx = false;
  const files = [];
  for (const sib of siblings) {
    if (!sib || typeof sib !== "object" || typeof sib.rfilename !== "string") {
      continue;
    }
    const size = Number.isFinite(sib.size) ? sib.size : 0;
    totalBytes += size;
    files.push({ name: sib.rfilename, size });
    if (/\.onnx$/i.test(sib.rfilename)) {
      hasOnnx = true;
    }
  }
  const card = json.cardData && typeof json.cardData === "object" ? json.cardData : {};
  const licence = typeof card.license === "string" ? card.license : Array.isArray(card.license) ? card.license[0] : null;
  return {
    id: typeof json.id === "string" ? json.id : typeof json.modelId === "string" ? json.modelId : "",
    licence: licence || "unknown",
    licenceName: typeof card.license_name === "string" ? card.license_name : null,
    gated: json.gated === true || json.gated === "auto" || json.gated === "manual",
    gatedMode: typeof json.gated === "string" ? json.gated : json.gated === true ? "manual" : null,
    pipelineTag: typeof json.pipeline_tag === "string" ? json.pipeline_tag : null,
    downloads: Number.isFinite(json.downloads) ? json.downloads : null,
    likes: Number.isFinite(json.likes) ? json.likes : null,
    totalBytes,
    hasOnnx,
    files,
  };
}

/** Parse a model-search response into a list of stable entries. */
export function parseModelSearch(json) {
  if (!Array.isArray(json)) {
    return [];
  }
  return json
    .map((item) => {
      if (!item || typeof item !== "object" || typeof item.id !== "string") {
        return null;
      }
      const card = item.cardData && typeof item.cardData === "object" ? item.cardData : {};
      const licence = typeof card.license === "string" ? card.license : null;
      return {
        id: item.id,
        licence: licence || "unknown",
        gated: item.gated === true || item.gated === "auto" || item.gated === "manual",
        pipelineTag: typeof item.pipeline_tag === "string" ? item.pipeline_tag : null,
        downloads: Number.isFinite(item.downloads) ? item.downloads : null,
        likes: Number.isFinite(item.likes) ? item.likes : null,
      };
    })
    .filter(Boolean);
}

/** Human-readable licence line. */
export function describeLicence(name) {
  if (!name || name === "unknown") {
    return "licence: unknown";
  }
  return `licence: ${name}`;
}

/** Human-readable gating line. */
export function describeGating(gated, mode) {
  if (!gated) {
    return "public";
  }
  return mode === "manual" ? "gated (approval required)" : "gated";
}
