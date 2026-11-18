#include "hcs/agent/tool_audit.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace hcs::agent {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------------------------
// Minimal Python lexer: enough to tell code from strings/comments and to read string literals.
// ---------------------------------------------------------------------------------------------------------
enum class Tok { Name, Op, Str, Number, Newline };

struct Token {
    Tok kind = Tok::Op;
    std::string text;  // identifier / operator char / decoded string value
    bool fstring = false;
    bool bytes = false;
};

bool is_ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}
bool is_ident_char(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

bool string_prefix(const std::string& id, bool& raw, bool& f, bool& b) {
    if (id.empty() || id.size() > 2)
        return false;
    raw = f = b = false;
    for (const char ch : id) {
        switch (std::tolower(static_cast<unsigned char>(ch))) {
            case 'r':
                raw = true;
                break;
            case 'f':
                f = true;
                break;
            case 'b':
                b = true;
                break;
            case 'u':
                break;
            default:
                return false;
        }
    }
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    return (std::tolower(static_cast<unsigned char>(c)) - 'a') + 10;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Text of the `{expression}` parts of an f-string body (`{{` and `}}` are literal braces). Python evaluates
// these, so they are audited like code.
std::string fstring_expressions(std::string_view body) {
    std::string out;
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] == '{') {
            if (i + 1 < body.size() && body[i + 1] == '{') {
                i += 2;
                continue;
            }
            int depth = 1;
            std::size_t j = i + 1;
            while (j < body.size() && depth > 0) {
                if (body[j] == '{')
                    ++depth;
                else if (body[j] == '}')
                    --depth;
                if (depth > 0)
                    out.push_back(body[j]);
                ++j;
            }
            out.push_back('\n');
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

std::vector<Token> lex(std::string_view s) {
    std::vector<Token> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    int depth = 0;
    auto push_op = [&](char c) {
        Token t;
        t.kind = Tok::Op;
        t.text.assign(1, c);
        out.push_back(std::move(t));
    };
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f') {
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {  // line continuation
            i += 2;
            if (i < n && s[i - 1] == '\r' && s[i] == '\n')
                ++i;
            continue;
        }
        if (c == '#') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == '\n') {
            if (depth <= 0) {
                Token t;
                t.kind = Tok::Newline;
                out.push_back(std::move(t));
            }
            ++i;
            continue;
        }
        bool raw = false, fs_ = false, by = false;
        bool is_string = false;
        std::size_t qpos = i;
        if (c == '\'' || c == '"') {
            is_string = true;
        } else if (is_ident_start(c)) {
            std::size_t j = i;
            while (j < n && is_ident_char(static_cast<unsigned char>(s[j]))) ++j;
            const std::string id(s.substr(i, j - i));
            if (j < n && (s[j] == '\'' || s[j] == '"') && string_prefix(id, raw, fs_, by)) {
                is_string = true;
                qpos = j;
            } else {
                Token t;
                t.kind = Tok::Name;
                t.text = id;
                out.push_back(std::move(t));
                i = j;
                continue;
            }
        }
        if (is_string) {
            const char q = s[qpos];
            const bool triple = qpos + 2 < n && s[qpos + 1] == q && s[qpos + 2] == q;
            std::size_t j = qpos + (triple ? 3 : 1);
            std::string value;
            bool closed = false;
            while (j < n) {
                const char ch = s[j];
                if (ch == '\\' && j + 1 < n) {
                    const char e = s[j + 1];
                    const char kind = raw ? '\0' : e;  // a raw string keeps every escape verbatim
                    if (kind == '\\' || kind == '\'' || kind == '"') {
                        value.push_back(e);
                    } else if (kind == 'n') {
                        value.push_back('\n');
                    } else if (kind == 't') {
                        value.push_back('\t');
                    } else if (kind == 'r') {
                        value.push_back('\r');
                    } else if (kind == '\n') {
                        // escaped newline inside a string: dropped
                    } else if (kind == 'x' || kind == 'u' || kind == 'U') {
                        const std::size_t digits = e == 'x' ? 2 : (e == 'u' ? 4 : 8);
                        std::uint32_t cp = 0;
                        std::size_t k = 0;
                        while (k < digits && j + 2 + k < n &&
                               std::isxdigit(static_cast<unsigned char>(s[j + 2 + k]))) {
                            cp = cp * 16 + static_cast<std::uint32_t>(hex_value(s[j + 2 + k]));
                            ++k;
                        }
                        if (k == digits) {
                            append_utf8(value, cp);
                            j += 2 + digits;
                            continue;
                        }
                        value.push_back('\\');
                        value.push_back(e);
                    } else if (kind >= '0' && kind <= '7') {
                        std::uint32_t cp = 0;
                        std::size_t k = 0;
                        while (k < 3 && j + 1 + k < n && s[j + 1 + k] >= '0' && s[j + 1 + k] <= '7') {
                            cp = cp * 8 + static_cast<std::uint32_t>(s[j + 1 + k] - '0');
                            ++k;
                        }
                        append_utf8(value, cp);
                        j += 1 + k;
                        continue;
                    } else {
                        value.push_back('\\');
                        value.push_back(e);
                    }
                    j += 2;
                    continue;
                }
                if (triple) {
                    if (ch == q && j + 2 < n + 0 && s.substr(j, 3) == std::string(3, q)) {
                        j += 3;
                        closed = true;
                        break;
                    }
                } else {
                    if (ch == q) {
                        ++j;
                        closed = true;
                        break;
                    }
                    if (ch == '\n')
                        break;  // unterminated single-line string
                }
                value.push_back(ch);
                ++j;
            }
            (void)closed;
            Token t;
            t.kind = Tok::Str;
            t.text = std::move(value);
            t.fstring = fs_;
            t.bytes = by;
            out.push_back(std::move(t));
            i = j;
            continue;
        }
        if (std::isdigit(c)) {
            std::size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '_'))
                ++j;
            Token t;
            t.kind = Tok::Number;
            t.text.assign(s.substr(i, j - i));
            out.push_back(std::move(t));
            i = j;
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        if ((c == ')' || c == ']' || c == '}') && depth > 0)
            --depth;
        push_op(static_cast<char>(c));
        ++i;
    }
    return out;
}

bool is_op(const std::vector<Token>& t, std::size_t i, char c) {
    return i < t.size() && t[i].kind == Tok::Op && t[i].text.size() == 1 && t[i].text[0] == c;
}
bool is_name(const std::vector<Token>& t, std::size_t i, std::string_view name) {
    return i < t.size() && t[i].kind == Tok::Name && t[i].text == name;
}

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

AuditVerdict deny(std::string rule, std::string detail) {
    AuditVerdict v;
    v.allowed = false;
    v.rule = std::move(rule);
    v.detail = std::move(detail);
    return v;
}
AuditVerdict ok() {
    AuditVerdict v;
    v.rule = "ok";
    return v;
}

const std::set<std::string>& network_modules() {
    static const std::set<std::string> m = {
        "socket",   "ssl",      "http",       "urllib",    "urllib3",  "requests",     "httpx",
        "httpx2",   "httpcore", "ftplib",     "smtplib",   "imaplib",  "poplib",       "telnetlib",
        "xmlrpc",   "aiohttp",  "websockets", "websocket", "paramiko", "socketserver", "asyncore",
        "asynchat", "smtpd",    "nntplib",    "mcp"};
    return m;
}
const std::set<std::string>& dynamic_modules() {
    static const std::set<std::string> m = {"ctypes",  "cffi",     "importlib", "runpy",  "code",
                                            "codeop",  "imp",      "marshal",   "pickle", "dill",
                                            "_ctypes", "builtins", "pty"};
    return m;
}
const std::set<std::string>& process_modules() {
    static const std::set<std::string> m = {"multiprocessing", "pexpect", "winreg",     "_winapi",
                                            "webbrowser",      "msvcrt",  "concurrent", "psutil",
                                            "sched",           "signal"};
    return m;
}

// Returns the "kind" rule for an imported module root, or empty when allowed.
std::string denied_import_rule(const std::string& root, const AuditPolicy& policy) {
    if (std::find(policy.denied_imports.begin(), policy.denied_imports.end(), root) !=
        policy.denied_imports.end())
        return "network";
    if (network_modules().count(root))
        return "network";
    if (dynamic_modules().count(root))
        return "dynamic_code";
    if (process_modules().count(root))
        return "process_spawn";
    return {};
}

// Names used to reach the interpreter internals; a lexical scan cannot follow them, so they are refused
// outright.
bool is_dunder_escape(const std::string& n) {
    static const std::set<std::string> k = {"__builtins__",     "__import__", "__subclasses__",
                                            "__globals__",      "__code__",   "__loader__",
                                            "__getattribute__", "__bases__",  "__mro__"};
    return k.count(n) != 0;
}

bool has_non_ascii(const std::string& n) {
    return std::any_of(n.begin(), n.end(), [](unsigned char c) { return c >= 0x80; });
}

bool looks_like_url(std::string_view s) {
    return s.find("://") != std::string_view::npos;
}

bool has_dotdot_segment(std::string_view s) {
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find_first_of("/\\", start);
        if (end == std::string_view::npos)
            end = s.size();
        if (s.substr(start, end - start) == "..")
            return true;
        start = end + 1;
    }
    return false;
}

bool is_drive_absolute(std::string_view s) {
    return s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' &&
           (s[2] == '\\' || s[2] == '/');
}

// "/etc/passwd", "/usr", "\\Windows\\..." - a rooted path, not a lone slash or a regex/URL path piece with
// spaces.
bool is_rooted_path(std::string_view s) {
    if (s.size() < 2 || s.size() > 1024 || (s[0] != '/' && s[0] != '\\'))
        return false;
    if (s[0] == '\\' && s[1] != '\\') {
        // `\d+` or `\w\s` are regex escapes; only `\\server` (UNC) or `\Windows\...` (a word of 4+ letters
        // followed by a separator) are treated as root-relative Windows paths.
        std::size_t k = 1;
        while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k]))) ++k;
        if (k < 5 || k >= s.size() || (s[k] != '\\' && s[k] != '/'))
            return false;
    }
    if (s.find_first_of(" \t\n\r") != std::string_view::npos)
        return false;
    const unsigned char c = static_cast<unsigned char>(s[1]);
    return std::isalnum(c) || c == '.' || c == '_' || c == '~' || c == '-' || c == '\\' || c == '/';
}

// Splits a command line into argv. Rejects shell metacharacters and unbalanced quotes.
bool split_command(std::string_view cmd, std::vector<std::string>& argv, std::string& why) {
    argv.clear();
    std::string cur;
    bool in_tok = false;
    char quote = 0;
    for (const char ch : cmd) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (ch == '\0' || ch == '\n' || ch == '\r') {
            why = "line break or NUL in a command";
            return false;
        }
        if (quote) {
            if (ch == quote) {
                quote = 0;
            } else {
                cur.push_back(ch);
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            in_tok = true;
            continue;
        }
        if (std::string_view("|&;<>`$%^(){}!*?~").find(ch) != std::string_view::npos && ch != '~') {
            why = std::string("shell metacharacter '") + ch + "' is not allowed";
            return false;
        }
        if (ch == '~' && !in_tok) {
            why = "home expansion is not allowed";
            return false;
        }
        if (std::isspace(u)) {
            if (in_tok) {
                argv.push_back(cur);
                cur.clear();
                in_tok = false;
            }
            continue;
        }
        cur.push_back(ch);
        in_tok = true;
    }
    if (quote) {
        why = "unbalanced quote";
        return false;
    }
    if (in_tok)
        argv.push_back(cur);
    return true;
}

std::string program_stem(std::string_view arg0) {
    std::string s = lower(arg0);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0)
        s.resize(s.size() - 4);
    return s;
}

}  // namespace

AuditPolicy default_audit_policy() {
    AuditPolicy p;
    p.extra_commands = {
        {"gcc", {}, {}},          {"cc", {}, {}},     {"clang", {}, {}}, {"make", {}, {}},
        {"mingw32-make", {}, {}}, {"pytest", {}, {}}, {"ls", {}, {}},    {"cat", {}, {}},
        {"head", {}, {}},         {"tail", {}, {}},   {"wc", {}, {}},    {"diff", {}, {}},
    };
    return p;
}

Result<ToolAudit> ToolAudit::create(const fs::path& root, AuditPolicy policy) {
    PathGuard guard;
    if (auto r = guard.add_root(root); !r)
        return r.error();
    SandboxPolicy sp = default_policy();
    for (const auto& extra : policy.extra_commands) sp.commands.push_back(extra);
    auto sandbox = ToolSandbox::create(std::move(guard), std::move(sp));
    if (!sandbox)
        return sandbox.error();
    std::error_code ec;
    auto canon = fs::weakly_canonical(root, ec);
    return ToolAudit(ec ? root : canon, std::move(sandbox).value(), std::move(policy));
}

AuditVerdict ToolAudit::audit_path_literal(std::string_view lit) const {
    if (lit.empty() || lit.size() > 1024 || looks_like_url(lit))
        return ok();
    if (lit[0] == '~')
        return deny("path_escape", "home directory access is not allowed");
    const bool abs = is_drive_absolute(lit) || is_rooted_path(lit);
    const bool dd = has_dotdot_segment(lit) && lit.find_first_of(" \t\n") == std::string_view::npos;
    if (!abs && !dd)
        return ok();
    const fs::path p = abs ? fs::path(std::string(lit)) : root_ / fs::path(std::string(lit));
    auto r = sandbox_.authorize_read(p);
    if (!r)
        return deny("path_escape", "a path literal is outside the approved workspace");
    return ok();
}

AuditVerdict ToolAudit::audit_argv(const std::vector<std::string>& argv_in) const {
    if (argv_in.empty())
        return deny("unauditable_command", "empty command");
    const std::string stem = program_stem(argv_in[0]);
    if (stem == "python" || stem == "python3" || stem == "py") {
        std::size_t i = 1;
        while (i < argv_in.size() && (argv_in[i] == "-u" || argv_in[i] == "-B")) ++i;
        if (i >= argv_in.size())
            return deny("command_not_allowed", "interactive python is not allowed");
        const std::string& a = argv_in[i];
        if (a == "-m") {
            if (i + 1 >= argv_in.size())
                return deny("command_not_allowed", "python -m needs a module");
            const std::string& mod = argv_in[i + 1];
            if (std::find(policy_.python_modules.begin(), policy_.python_modules.end(), mod) ==
                policy_.python_modules.end())
                return deny("command_not_allowed", "python -m " + mod + " is not allow-listed");
            ++i;
        } else if (a.size() > 3 && lower(a).compare(a.size() - 3, 3, ".py") == 0 && a[0] != '-') {
            const fs::path script =
                is_drive_absolute(a) || is_rooted_path(a) ? fs::path(a) : root_ / fs::path(a);
            if (!sandbox_.authorize_read(script))
                return deny("path_escape", "script is outside the approved workspace");
        } else {
            return deny("command_not_allowed", "python may only run -m <allowed module> or a .py script");
        }
        for (std::size_t k = i + 1; k < argv_in.size(); ++k) {
            const std::string& arg = argv_in[k];
            if (looks_like_url(arg))
                return deny("network", "URL argument");
            if (auto v = audit_path_literal(arg); !v.allowed)
                return v;
        }
        return ok();
    }
    CommandRequest req;
    req.argv = argv_in;
    req.cwd = root_;
    auto approved = sandbox_.authorize_command(req);
    if (!approved) {
        const auto code = approved.error().code;
        return deny(code == ErrorCode::SecurityViolation ? "command_not_allowed" : "unauditable_command",
                    approved.error().message);
    }
    return ok();
}

AuditVerdict ToolAudit::audit_command_line(std::string_view command) const {
    std::vector<std::string> argv;
    std::string why;
    if (command.size() > 4096)
        return deny("unauditable_command", "command is too long");
    if (!split_command(command, argv, why))
        return deny("unauditable_command", why);
    return audit_argv(argv);
}

AuditVerdict ToolAudit::audit_python(std::string_view code, int depth) const {
    if (depth > 4)
        return deny("malformed", "f-string expressions are nested too deeply to audit");
    if (code.size() > policy_.max_code_bytes)
        return deny("malformed", "code exceeds the audit size cap");
    const std::vector<Token> t = lex(code);

    auto statement_start = [&](std::size_t i) {
        return i == 0 || t[i - 1].kind == Tok::Newline || is_op(t, i - 1, ';') || is_op(t, i - 1, ':');
    };

    // Reads the first argument of a call whose '(' is at `open`. Returns false when it is not auditable.
    auto first_argument = [&](std::size_t open, std::vector<std::string>& argv, bool& is_list) {
        std::size_t j = open + 1;
        if (j < t.size() && t[j].kind == Tok::Str) {
            std::string s;
            while (j < t.size() && t[j].kind == Tok::Str) {
                if (t[j].fstring || t[j].bytes)
                    return false;
                s += t[j].text;
                ++j;
            }
            if (!(is_op(t, j, ')') || is_op(t, j, ',')))
                return false;
            argv.assign(1, s);
            is_list = false;
            return true;
        }
        if (is_op(t, j, '[')) {
            argv.clear();
            ++j;
            while (j < t.size() && !is_op(t, j, ']')) {
                if (t[j].kind != Tok::Str || t[j].fstring || t[j].bytes)
                    return false;
                argv.push_back(t[j].text);
                ++j;
                if (is_op(t, j, ','))
                    ++j;
                else if (!is_op(t, j, ']'))
                    return false;
            }
            if (!is_op(t, j, ']'))
                return false;
            is_list = true;
            return !argv.empty();
        }
        return false;
    };

    auto audit_call = [&](std::size_t open, std::string_view what) -> AuditVerdict {
        std::vector<std::string> argv;
        bool is_list = false;
        if (!first_argument(open, argv, is_list))
            return deny("unauditable_command",
                        std::string(what) + " needs a literal command (string or list of strings)");
        return is_list ? audit_argv(argv) : audit_command_line(argv[0]);
    };

    static const std::set<std::string> kSubprocessCalls = {
        "run", "call", "check_call", "check_output", "Popen", "getoutput", "getstatusoutput"};

    bool in_import = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const Token& tk = t[i];
        if (tk.kind == Tok::Newline || is_op(t, i, ';')) {
            in_import = false;
            continue;
        }
        if (tk.kind == Tok::Str && tk.fstring) {
            if (auto v = audit_python(fstring_expressions(tk.text), depth + 1); !v.allowed)
                return v;
        }
        if (tk.kind == Tok::Str) {
            const std::string_view lit = tk.fstring ? std::string_view(tk.text).substr(0, tk.text.find('{'))
                                                    : std::string_view(tk.text);
            if (auto v = audit_path_literal(lit); !v.allowed)
                return v;
            continue;
        }
        if (tk.kind != Tok::Name)
            continue;
        const bool after_dot = i > 0 && is_op(t, i - 1, '.');
        if (has_non_ascii(tk.text))
            return deny("dynamic_code", "non-ASCII identifiers are not auditable");
        if (is_dunder_escape(tk.text))
            return deny("dynamic_code", "'" + tk.text + "' reaches interpreter internals");

        // --- imports ---
        if (!after_dot && statement_start(i) && (tk.text == "import" || tk.text == "from")) {
            in_import = true;
            std::size_t j = i + 1;
            if (tk.text == "import") {
                while (j < t.size() && t[j].kind != Tok::Newline && !is_op(t, j, ';')) {
                    if (t[j].kind == Tok::Name && t[j].text != "as" && (j == i + 1 || is_op(t, j - 1, ','))) {
                        const std::string root = t[j].text;
                        if (auto rule = denied_import_rule(root, policy_); !rule.empty())
                            return deny(rule, "import of '" + root + "' is not allowed");
                        if ((root == "subprocess" || root == "os") && is_name(t, j + 1, "as"))
                            return deny("unauditable_command", "aliasing '" + root + "' hides process calls");
                    }
                    ++j;
                }
            } else {
                while (j < t.size() && is_op(t, j, '.')) ++j;
                if (j < t.size() && t[j].kind == Tok::Name) {
                    const std::string root = t[j].text;
                    if (auto rule = denied_import_rule(root, policy_); !rule.empty())
                        return deny(rule, "import from '" + root + "' is not allowed");
                    if (root == "subprocess")
                        return deny("unauditable_command", "'from subprocess import' hides process calls");
                    if (root == "os") {
                        for (std::size_t k = j;
                             k < t.size() && t[k].kind != Tok::Newline && !is_op(t, k, ';'); ++k) {
                            if (is_op(t, k, '*'))
                                return deny("process_spawn", "'from os import *' hides process calls");
                        }
                        for (std::size_t k = j;
                             k < t.size() && t[k].kind != Tok::Newline && !is_op(t, k, ';'); ++k) {
                            if (t[k].kind == Tok::Name &&
                                (t[k].text == "system" || t[k].text == "popen" || t[k].text == "kill" ||
                                 t[k].text == "startfile" || t[k].text.rfind("spawn", 0) == 0 ||
                                 t[k].text.rfind("exec", 0) == 0 || t[k].text == "fork"))
                                return deny("process_spawn",
                                            "'from os import " + t[k].text + "' hides process calls");
                        }
                    }
                }
            }
            continue;
        }

        // --- dynamic code ---
        if (tk.text == "__import__" || tk.text == "importlib")
            return deny("dynamic_code", "dynamic import is not allowed");
        if (!after_dot && !in_import &&
            (((tk.text == "compile" || tk.text == "breakpoint") && is_op(t, i + 1, '(')) ||
             tk.text == "exec" || tk.text == "eval"))
            return deny("dynamic_code", tk.text + " runs code that cannot be audited");
        if (!after_dot && !in_import && (tk.text == "subprocess" || tk.text == "os") && !is_op(t, i + 1, '.'))
            return deny("unauditable_command", "'" + tk.text + "' is used without a visible attribute call");
        if (after_dot && (tk.text == "home" || tk.text == "expanduser") && is_op(t, i + 1, '('))
            return deny("path_escape", "home directory access is not allowed");

        // --- process and command calls ---
        if (tk.text == "bash" && is_op(t, i + 1, '(')) {
            if (auto v = audit_call(i + 1, "bash()"); !v.allowed)
                return v;
            continue;
        }
        if (tk.text == "subprocess" && is_op(t, i + 1, '.') && i + 3 < t.size() &&
            t[i + 2].kind == Tok::Name && is_op(t, i + 3, '(')) {
            if (kSubprocessCalls.count(t[i + 2].text) == 0)
                return deny("process_spawn", "subprocess." + t[i + 2].text + " is not allowed");
            if (auto v = audit_call(i + 3, "subprocess." + t[i + 2].text + "()"); !v.allowed)
                return v;
            continue;
        }
        if (tk.text == "os" && is_op(t, i + 1, '.') && i + 3 < t.size() && t[i + 2].kind == Tok::Name &&
            is_op(t, i + 3, '(')) {
            const std::string& fn = t[i + 2].text;
            if (fn == "system" || fn == "popen") {
                if (auto v = audit_call(i + 3, "os." + fn + "()"); !v.allowed)
                    return v;
            } else if (fn.rfind("spawn", 0) == 0 || fn.rfind("exec", 0) == 0 || fn == "fork" ||
                       fn == "forkpty" || fn == "kill" || fn == "killpg" || fn == "startfile" ||
                       fn == "posix_spawn" || fn == "posix_spawnp" || fn == "putenv" || fn == "chroot") {
                return deny("process_spawn", "os." + fn + "() is not allowed");
            }
            continue;
        }
        if (tk.text == "asyncio" && is_op(t, i + 1, '.') && i + 2 < t.size() && t[i + 2].kind == Tok::Name &&
            t[i + 2].text.rfind("create_subprocess", 0) == 0)
            return deny("process_spawn", "use bash() with a literal command instead of asyncio subprocesses");
    }
    return ok();
}

AuditVerdict ToolAudit::audit_tool_call(std::string_view tool, std::string_view payload) const {
    if (tool != "python_repl")
        return deny("unknown_tool",
                    "tool '" + std::string(tool.substr(0, 64)) + "' is not available to HCS Agent");
    return audit_python(payload);
}

}  // namespace hcs::agent
