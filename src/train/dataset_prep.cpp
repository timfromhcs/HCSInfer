#include "hcs/train/dataset_prep.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "hcs/storage/sha256.hpp"
#include "hcs/train/chat_template.hpp"

namespace hcs::train {

namespace fs = std::filesystem;

namespace {

std::string str_member(const OJson& o, const char* key) {
    if (!o.is_object())
        return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string normalise(std::string_view s) {
    std::string out;
    bool space = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty())
            out += ' ';
        space = false;
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}

Result<std::string> read_file_bounded(const fs::path& p, std::size_t max_bytes) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
        return Error(ErrorCode::NotFound, "not a file: " + p.string());
    const auto size = fs::file_size(p, ec);
    if (ec || size > max_bytes)
        return Error(ErrorCode::InvalidArgument, "file is larger than the allowed " +
                                                     std::to_string(max_bytes) + " bytes: " + p.string());
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return Error(ErrorCode::IoError, "cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string sha256_hex(const std::string& data) {
    return storage::to_hex(
        storage::sha256({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}));
}

// FNV-1a 64 over seed + id: stable across platforms (std::hash is not).
std::uint64_t split_hash(std::uint64_t seed, const std::string& id) {
    std::uint64_t h = 1469598103934665603ULL ^ seed;
    for (unsigned char c : id) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// Special tokens inside message text would let a training record forge turn boundaries.
bool has_control_tokens(const OJson& rec) {
    const std::string text = rec.contains("messages") ? rec["messages"].dump() : std::string();
    for (const char* tok : {"<|im_start|>", "<|im_end|>", "<|startoftext|>"})
        if (text.find(tok) != std::string::npos)
            return true;
    return false;
}

bool overlaps_eval(const OJson& rec, const std::vector<std::string>& eval_prompts) {
    if (eval_prompts.empty() || !rec.contains("messages") || !rec["messages"].is_array())
        return false;
    for (const auto& m : rec["messages"]) {
        if (!m.is_object() || str_member(m, "role") != "user" || !m.contains("content") ||
            !m["content"].is_string())
            continue;
        const std::string u = normalise(m["content"].get<std::string>());
        for (const auto& e : eval_prompts) {
            if (u == e || (e.size() >= 24 && u.find(e) != std::string::npos))
                return true;
        }
    }
    return false;
}

}  // namespace

Result<std::vector<std::string>> load_eval_prompts(const std::vector<fs::path>& files) {
    std::vector<std::string> out;
    for (const auto& f : files) {
        auto text = read_file_bounded(f, 64ULL * 1024 * 1024);
        if (!text)
            return text.error();
        std::istringstream in(text.value());
        std::string line;
        while (std::getline(in, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos)
                continue;
            const OJson t = OJson::parse(line, nullptr, false);
            if (t.is_discarded() || !t.is_object() || !t.contains("messages") || !t["messages"].is_array())
                return Error(ErrorCode::InvalidArgument,
                             "eval file has a line that is not a task object: " + f.string());
            for (const auto& m : t["messages"])
                if (m.is_object() && str_member(m, "role") == "user" && m.contains("content") &&
                    m["content"].is_string()) {
                    const std::string n = normalise(m["content"].get<std::string>());
                    if (!n.empty())
                        out.push_back(n);
                }
        }
    }
    return out;
}

Result<PrepareReport> prepare_dataset(const PrepareOptions& opt) {
    if (opt.inputs.empty())
        return Error(ErrorCode::InvalidArgument, "at least one input file is required");
    if (opt.dev_fraction < 0.0 || opt.dev_fraction > 0.5)
        return Error(ErrorCode::InvalidArgument, "dev fraction must be within [0, 0.5]");
    if (opt.max_records_per_input == 0)
        return Error(ErrorCode::InvalidArgument, "max records per input must be positive");
    if (opt.out_dir.empty())
        return Error(ErrorCode::InvalidArgument, "an output directory is required");
    std::error_code ec;
    if (fs::exists(opt.out_dir / "manifest.json", ec))
        return Error(ErrorCode::AlreadyExists,
                     "output directory already holds a manifest.json: " + opt.out_dir.string());
    for (const char* name : {"train.jsonl", "dev.jsonl"})
        if (fs::exists(opt.out_dir / name, ec))
            return Error(ErrorCode::AlreadyExists,
                         std::string("output directory already holds ") + name + " (refusing to overwrite)");

    auto eval_prompts = load_eval_prompts(opt.eval_files);
    if (!eval_prompts)
        return eval_prompts.error();

    DatasetPolicy policy;
    for (const auto& l : opt.extra_allowed_licences) policy.allowed_licences.push_back(l);
    const DatasetValidator validator(opt.limits, policy);

    PrepareReport rep;
    std::string train_text, dev_text;
    std::set<std::string> seen_across_inputs;  // dedupe across files by prompt+completion
    nlohmann::json attribution = nlohmann::json::array();

    for (const auto& in_path : opt.inputs) {
        auto text = read_file_bounded(in_path, opt.limits.max_total_bytes);
        if (!text)
            return text.error();
        PrepareInputReport ir;
        ir.path = in_path.filename().string();
        ir.sha256 = sha256_hex(text.value());
        ir.validation = validator.validate_string(text.value());
        const std::set<std::size_t> accepted(ir.validation.accepted_lines.begin(),
                                             ir.validation.accepted_lines.end());
        std::istringstream in(text.value());
        std::string line;
        std::size_t line_no = 0, taken = 0;
        std::set<std::string> licences, sources;
        while (std::getline(in, line)) {
            ++line_no;
            if (!accepted.count(line_no))
                continue;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const OJson rec = OJson::parse(line, nullptr, false);
            if (rec.is_discarded() || !rec.is_object()) {
                ++ir.render_failed;
                continue;
            }
            if (overlaps_eval(rec, eval_prompts.value())) {
                ++ir.eval_overlap_dropped;
                continue;
            }
            if (has_control_tokens(rec)) {
                ++ir.render_failed;
                continue;
            }
            if (taken >= opt.max_records_per_input) {
                ++ir.capped;
                continue;
            }
            auto exs = to_sft_examples(rec);
            if (!exs) {
                ++ir.render_failed;
                continue;
            }
            ++taken;
            const std::string licence = str_member(rec, "licence");
            const std::string source = str_member(rec, "source");
            const std::string generator = str_member(rec, "generator");
            licences.insert(licence);
            sources.insert(source);
            for (auto& ex : exs.value()) {
                if (!seen_across_inputs.insert(ex.prompt + '\x1f' + ex.completion).second)
                    continue;
                nlohmann::json o = {
                    {"id", ex.id},        {"prompt", ex.prompt}, {"completion", ex.completion},
                    {"licence", licence}, {"source", source},    {"generator", generator}};
                const bool dev = opt.dev_fraction > 0.0 &&
                                 static_cast<double>(split_hash(opt.seed, str_member(rec, "id")) % 10000) <
                                     opt.dev_fraction * 10000.0;
                (dev ? dev_text : train_text) += o.dump() + "\n";
                ++(dev ? rep.dev_examples : rep.train_examples);
                ++ir.examples;
            }
        }
        if (ir.examples == 0)
            return Error(ErrorCode::InvalidArgument,
                         "input '" + ir.path + "' produced no usable examples (accepted " +
                             std::to_string(ir.validation.accepted) + ", rejected " +
                             std::to_string(ir.validation.rejected) + ", eval overlap " +
                             std::to_string(ir.eval_overlap_dropped) + ", render failures " +
                             std::to_string(ir.render_failed) + ")");
        attribution.push_back({{"input", ir.path}, {"licences", licences}, {"sources", sources}});
        rep.inputs.push_back(std::move(ir));
    }
    if (rep.train_examples == 0)
        return Error(ErrorCode::InvalidArgument, "no training examples left after the dev split");

    fs::create_directories(opt.out_dir, ec);
    if (ec)
        return Error(ErrorCode::IoError, "cannot create " + opt.out_dir.string());
    const auto write = [&](const char* name, const std::string& body) -> Result<void> {
        std::ofstream o(opt.out_dir / name, std::ios::binary | std::ios::trunc);
        o << body;
        o.flush();
        if (!o)
            return Error(ErrorCode::IoError, std::string("cannot write ") + name);
        return {};
    };
    if (auto r = write("train.jsonl", train_text); !r)
        return r.error();
    if (auto r = write("dev.jsonl", dev_text); !r)
        return r.error();
    rep.train_sha256 = sha256_hex(train_text);
    rep.dev_sha256 = sha256_hex(dev_text);

    nlohmann::json inputs = nlohmann::json::array();
    for (const auto& ir : rep.inputs)
        inputs.push_back({{"file", ir.path},
                          {"sha256", ir.sha256},
                          {"lines_read", ir.validation.lines_read},
                          {"accepted", ir.validation.accepted},
                          {"rejected", ir.validation.rejected},
                          {"duplicates", ir.validation.duplicates},
                          {"eval_overlap_dropped", ir.eval_overlap_dropped},
                          {"capped", ir.capped},
                          {"render_failed", ir.render_failed},
                          {"examples", ir.examples},
                          {"accepted_by_licence", ir.validation.accepted_by_licence}});
    nlohmann::json man = {
        {"schema_version", 1},
        {"template", "lfm3-chat-v1"},
        {"inputs", inputs},
        {"policy",
         {{"allowed_licences", policy.allowed_licences},
          {"forbidden_generator_substrings", policy.forbidden_generator_substrings},
          {"max_records_per_input", opt.max_records_per_input},
          {"dev_fraction", opt.dev_fraction},
          {"seed", opt.seed},
          {"eval_files_checked", opt.eval_files.size()},
          {"eval_prompts_checked", eval_prompts.value().size()}}},
        {"outputs",
         {{"train",
           {{"file", "train.jsonl"}, {"sha256", rep.train_sha256}, {"examples", rep.train_examples}}},
          {"dev", {{"file", "dev.jsonl"}, {"sha256", rep.dev_sha256}, {"examples", rep.dev_examples}}}}},
        {"attribution", attribution}};
    rep.manifest_path = opt.out_dir / "manifest.json";
    if (auto r = write("manifest.json", man.dump(2) + "\n"); !r)
        return r.error();
    return rep;
}

}  // namespace hcs::train
