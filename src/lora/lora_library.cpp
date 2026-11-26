#include "hcs/lora.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

#include "hcs/fs_util.hpp"
#include "hcs/image_profiles.hpp"
#include "hcs/safetensors.hpp"
#include "hcs/storage/sha256.hpp"

namespace hcs {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (unsigned char c : id)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-'))
            return false;
    return id != "." && id != "..";
}

bool bare_safetensors_name(const std::string& f) {
    if (f.empty() || f.size() > 200)
        return false;
    for (unsigned char c : f)
        if (c == '/' || c == '\\' || c == ':' || c == 0 || c < 32)
            return false;
    if (f == "." || f == "..")
        return false;
    return lower(f).size() > 12 && lower(f).substr(lower(f).size() - 12) == ".safetensors";
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64)
        return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c))
            return false;
    return true;
}

struct Family {
    std::string_view name;
    std::string_view group;
};
constexpr Family kFamilies[] = {{"sd15", "sd1"}, {"sd2", "sd1"},    {"sdxl", "sdxl"},
                                {"sd3", "sd3"},  {"flux1", "flux"}, {"flux3", "flux"}};

}  // namespace

bool is_known_lora_family(std::string_view f) noexcept {
    for (const auto& k : kFamilies)
        if (k.name == f)
            return true;
    return false;
}

std::string_view lora_family_group(std::string_view f) noexcept {
    for (const auto& k : kFamilies)
        if (k.name == f)
            return k.group;
    return {};
}

LoraCompat lora_compatibility(std::string_view lora_family, std::string_view model_family) noexcept {
    if (model_family.empty() || lora_family.empty())
        return LoraCompat::Unknown;
    // Exact family match: a FLUX.1 LoRA does not fit FLUX.3 Klein and an SD2 LoRA does not fit SD1.5, even
    // though both pairs share a tensor-naming group.
    // Turbo checkpoints are SD2 / SDXL derivatives: compare by the architecture the LoRA was trained for.
    return lora_family == image_base_family(model_family) ? LoraCompat::Compatible : LoraCompat::Incompatible;
}

std::string sniff_lora_group(const std::vector<std::string>& names) {
    bool te1 = false, te = false, flux = false, sd3 = false;
    for (const auto& n : names) {
        const std::string l = lower(n);
        if (l.find("lora_te1_") != std::string::npos || l.find("lora_te2_") != std::string::npos ||
            l.find("text_encoder_2") != std::string::npos)
            te1 = true;
        else if (l.find("lora_te_") != std::string::npos)
            te = true;
        if (l.find("double_blocks") != std::string::npos || l.find("single_blocks") != std::string::npos ||
            l.find("single_transformer_blocks") != std::string::npos)
            flux = true;
        if (l.find("joint_blocks") != std::string::npos)
            sd3 = true;
    }
    if (flux)
        return "flux";
    if (sd3)
        return "sd3";
    if (te1)
        return "sdxl";
    if (te)
        return "sd1";
    return {};
}

Result<void> validate_lora_weight(float w) {
    if (!std::isfinite(w))
        return make_error(ErrorCode::InvalidArgument, "lora weight must be a finite number");
    if (w == 0.0f)
        return make_error(ErrorCode::InvalidArgument, "lora weight 0 has no effect; omit the LoRA instead");
    if (std::fabs(w) > kMaxLoraWeight)
        return make_error(ErrorCode::InvalidArgument,
                          "lora weight must be within -" + std::to_string(static_cast<int>(kMaxLoraWeight)) +
                              ".." + std::to_string(static_cast<int>(kMaxLoraWeight)));
    return {};
}

Result<ParsedLoraManifest> parse_lora_manifest(std::string_view text) {
    nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return make_error(ErrorCode::ModelError, "lora manifest.json is not a JSON object");
    if (j.value("schema_version", 0) != 1)
        return make_error(ErrorCode::ModelError, "lora manifest.json: schema_version must be 1");
    if (!j.contains("loras") || !j["loras"].is_array())
        return make_error(ErrorCode::ModelError, "lora manifest.json: 'loras' must be an array");
    if (j["loras"].size() > 4096)
        return make_error(ErrorCode::ModelError, "lora manifest.json: too many entries");
    ParsedLoraManifest out;
    std::set<std::string> seen;
    for (const auto& e : j["loras"]) {
        LoraEntry le;
        auto str = [&](const char* k) -> std::string {
            if (e.is_object() && e.contains(k) && e[k].is_string())
                return e[k].get<std::string>();
            return {};
        };
        le.id = str("id");
        le.file = str("file");
        le.name = str("name");
        le.base_family = str("base_family");
        le.licence = str("licence");
        le.source = str("source");
        le.sha256 = lower(str("sha256"));
        if (!e.is_object()) {
            le.status = "invalid";
            le.problems.push_back("entry is not an object");
            out.entries.push_back(std::move(le));
            continue;
        }
        if (le.name.empty())
            le.name = le.id;
        if (!valid_id(le.id))
            le.problems.push_back("id must be 1..64 characters of [A-Za-z0-9._-]");
        else if (!seen.insert(le.id).second)
            le.problems.push_back("duplicate id");
        if (!bare_safetensors_name(le.file))
            le.problems.push_back("file must be a bare .safetensors file name inside the library folder");
        if (!is_known_lora_family(le.base_family))
            le.problems.push_back("base_family must be one of sd15, sd2, sdxl, sd3, flux1, flux3");
        if (le.licence.empty())
            le.problems.push_back("licence is required");
        if (!le.sha256.empty() && !is_hex64(le.sha256))
            le.problems.push_back("sha256 must be 64 hex characters");
        if (e.contains("recommended_weight")) {
            if (!e["recommended_weight"].is_number())
                le.problems.push_back("recommended_weight must be a number");
            else {
                le.recommended_weight = e["recommended_weight"].get<float>();
                if (auto v = validate_lora_weight(le.recommended_weight); !v)
                    le.problems.push_back("recommended_weight: " + v.error().message);
            }
        }
        if (e.contains("trigger_words")) {
            if (!e["trigger_words"].is_array() || e["trigger_words"].size() > 32)
                le.problems.push_back("trigger_words must be an array of at most 32 strings");
            else
                for (const auto& w : e["trigger_words"]) {
                    if (!w.is_string() || w.get<std::string>().size() > 64)
                        le.problems.push_back(
                            "trigger_words entries must be strings of at most 64 characters");
                    else
                        le.trigger_words.push_back(w.get<std::string>());
                }
        }
        if (!le.problems.empty())
            le.status = "invalid";
        out.entries.push_back(std::move(le));
    }
    return out;
}

Result<void> LoraLibrary::scan(const PathGuard& guard, const std::filesystem::path& dir,
                               LoraScanOptions opt) {
    entries_.clear();
    note_.clear();
    dir_.clear();
    auto rd = guard.resolve(dir);
    if (!rd)
        return rd.error();
    dir_ = rd.value();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir_, ec)) {
        note_ = "LoRA folder '" + dir_.string() + "' does not exist; the library is empty";
        return {};
    }
    std::set<std::string> referenced;
    const auto manifest = dir_ / "manifest.json";
    if (std::filesystem::exists(manifest, ec)) {
        auto text = read_file_limited(manifest, 4u << 20);
        if (!text)
            return text.error();
        auto parsed = parse_lora_manifest(text.value());
        if (!parsed)
            return parsed.error();
        entries_ = std::move(parsed.value().entries);
    } else {
        note_ = "no manifest.json in '" + dir_.string() + "'; files without an entry are unusable";
    }
    for (auto& e : entries_) {
        referenced.insert(lower(e.file));
        if (e.status == "invalid")
            continue;
        const auto fp = guard.resolve(dir_ / e.file);
        if (!fp) {
            e.status = "invalid";
            e.problems.push_back("file path rejected: " + fp.error().message);
            continue;
        }
        e.path = fp.value();
        std::error_code fec;
        if (!std::filesystem::is_regular_file(e.path, fec)) {
            e.status = "missing";
            e.problems.push_back("file does not exist");
            e.path.clear();
            continue;
        }
        e.size_bytes = std::filesystem::file_size(e.path, fec);
        if (e.size_bytes > kMaxLoraFileBytes) {
            e.status = "invalid";
            e.problems.push_back("file is larger than the " + std::to_string(kMaxLoraFileBytes >> 30) +
                                 " GiB LoRA limit");
            continue;
        }
        auto hdr = read_safetensors_header(e.path);
        if (!hdr) {
            e.status = "invalid";
            e.problems.push_back(hdr.error().message);
            continue;
        }
        e.tensor_count = hdr.value().tensors.size();
        std::vector<std::string> names;
        names.reserve(hdr.value().tensors.size());
        bool lora_like = false;
        for (const auto& t : hdr.value().tensors) {
            names.push_back(t.name);
            const std::string l = lower(t.name);
            if (l.find("lora") != std::string::npos || l.find("lokr") != std::string::npos ||
                l.find("loha") != std::string::npos)
                lora_like = true;
        }
        if (!lora_like) {
            e.status = "invalid";
            e.problems.push_back("no tensor name contains 'lora': this does not look like a LoRA file");
        }
        e.sniffed_group = sniff_lora_group(names);
        if (!e.sniffed_group.empty() && e.sniffed_group != lora_family_group(e.base_family)) {
            e.status = "invalid";
            e.problems.push_back("manifest declares base_family '" + e.base_family +
                                 "' but the tensor names look like a '" + e.sniffed_group + "' LoRA");
        }
        if (opt.verify_hashes && !e.sha256.empty()) {
            std::ifstream in(e.path, std::ios::binary);
            storage::Sha256 h;
            std::vector<char> buf(1u << 20);
            while (in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                if (in.gcount() > 0)
                    h.update(buf.data(), static_cast<std::size_t>(in.gcount()));
            }
            const std::string got = storage::to_hex(h.finalize());
            e.sha256_status = got == e.sha256 ? "ok" : "mismatch";
            if (e.sha256_status == "mismatch") {
                e.status = "invalid";
                e.problems.push_back("sha256 mismatch: manifest " + e.sha256 + ", file " + got);
            }
        }
    }
    for (const auto& de : std::filesystem::directory_iterator(dir_, ec)) {
        if (!de.is_regular_file(ec))
            continue;
        const std::string name = de.path().filename().string();
        if (lower(name).size() < 12 || lower(name).substr(lower(name).size() - 12) != ".safetensors")
            continue;
        if (referenced.count(lower(name)))
            continue;
        LoraEntry u;
        u.id = name;
        u.name = name;
        u.file = name;
        u.status = "unmanifested";
        u.problems.push_back("not listed in manifest.json (base family, licence and weight are unknown)");
        entries_.push_back(std::move(u));
    }
    return {};
}

const LoraEntry* LoraLibrary::find(std::string_view id) const noexcept {
    for (const auto& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

Result<std::vector<LoraSpec>> LoraLibrary::resolve(const std::vector<Ref>& refs,
                                                   std::string_view model_family,
                                                   bool allow_unknown_family) const {
    if (refs.size() > kMaxLorasPerRequest)
        return make_error(ErrorCode::InvalidArgument,
                          "at most " + std::to_string(kMaxLorasPerRequest) + " LoRAs per request");
    std::vector<LoraSpec> out;
    std::set<std::string> seen;
    for (const auto& r : refs) {
        if (!valid_id(r.id))
            return make_error(ErrorCode::InvalidArgument, "invalid lora id");
        if (!seen.insert(r.id).second)
            return make_error(ErrorCode::InvalidArgument, "lora '" + r.id + "' is listed twice");
        const LoraEntry* e = find(r.id);
        if (!e)
            return make_error(ErrorCode::NotFound, "lora '" + r.id + "' is not in the library");
        if (!e->usable()) {
            std::string why;
            for (const auto& p : e->problems) why += (why.empty() ? "" : "; ") + p;
            return make_error(ErrorCode::ModelError, "lora '" + r.id + "' is " + e->status + ": " + why);
        }
        const float w = r.weight_given ? r.weight : e->recommended_weight;
        if (auto v = validate_lora_weight(w); !v)
            return make_error(ErrorCode::InvalidArgument, "lora '" + r.id + "': " + v.error().message);
        switch (lora_compatibility(e->base_family, model_family)) {
            case LoraCompat::Compatible:
                break;
            case LoraCompat::Incompatible:
                return make_error(ErrorCode::InvalidArgument, "lora_incompatible: LoRA '" + r.id +
                                                                  "' was trained for '" + e->base_family +
                                                                  "' but the model family is '" +
                                                                  std::string(model_family) + "'");
            case LoraCompat::Unknown:
                if (!allow_unknown_family)
                    return make_error(
                        ErrorCode::InvalidArgument,
                        "lora_incompatible: the loaded model's family is not recognised, so LoRA '" + r.id +
                            "' (" + e->base_family + ") cannot be verified");
                break;
        }
        out.push_back(LoraSpec{e->id, e->path.string(), e->base_family, w});
    }
    return out;
}

}  // namespace hcs


