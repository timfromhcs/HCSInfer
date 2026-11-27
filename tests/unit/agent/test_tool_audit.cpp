// ToolAudit is a tripwire, not a sandbox (docs/agent.md). These tests pin down what it must catch (including
// the usual obfuscation tricks) and what ordinary, harmless code must NOT trip. Cases that a lexical scan
// cannot catch are listed explicitly as documented limits so nobody mistakes them for coverage.
#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "hcs/agent/tool_audit.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::agent;
using hcs::test::TempDir;
namespace fs = std::filesystem;

namespace {

struct AuditFixture : ::testing::Test {
    TempDir tmp;
    fs::path root = tmp / "ws";
    fs::path outside = tmp / "elsewhere";
    std::optional<ToolAudit> audit;

    void SetUp() override {
        fs::create_directories(root / "src");
        fs::create_directories(outside);
        auto r = ToolAudit::create(root);
        ASSERT_TRUE(r) << r.error().message;
        audit.emplace(std::move(r).value());
    }

    AuditVerdict py(const std::string& code) const { return audit->audit_tool_call("python_repl", code); }

    void expect_denied(const std::string& code, const std::string& rule) const {
        const AuditVerdict v = py(code);
        EXPECT_FALSE(v.allowed) << "should be denied: " << code;
        if (!rule.empty()) {
            EXPECT_EQ(v.rule, rule) << code;
        }
        EXPECT_LE(v.detail.size(), 300u) << "verdict detail must stay short (never the whole code)";
    }
    void expect_allowed(const std::string& code) const {
        const AuditVerdict v = py(code);
        EXPECT_TRUE(v.allowed) << code << " -> " << v.rule << ": " << v.detail;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Tool names and harmless code
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, OnlyPythonReplIsAvailable) {
    for (const char* tool : {"bash", "shell", "read_file", "", "python_repl2", "PYTHON_REPL"}) {
        const auto v = audit->audit_tool_call(tool, "print(1)");
        EXPECT_FALSE(v.allowed) << tool;
        EXPECT_EQ(v.rule, "unknown_tool") << tool;
    }
    EXPECT_TRUE(audit->audit_tool_call("python_repl", "print(1)").allowed);
}

TEST_F(AuditFixture, UnknownToolNameIsCappedInVerdict) {
    const auto v = audit->audit_tool_call(std::string(5000, 'x'), "");
    EXPECT_FALSE(v.allowed);
    EXPECT_LT(v.detail.size(), 200u);
}

TEST_F(AuditFixture, OrdinaryCodeIsAllowed) {
    expect_allowed("");
    expect_allowed("print('hello')");
    expect_allowed("import os\nimport sys, json, re\nprint(os.path.join('a', 'b'), sys.argv)");
    expect_allowed("from pathlib import Path\nprint(Path('src/main.c').read_text())");
    expect_allowed("import re\nm = re.compile(r'\\d+')\nprint(m.findall('a1b22'))");
    expect_allowed("def f(x):\n    return [i * 2 for i in range(x)]\nprint(f(3))");
    expect_allowed("model.eval()\nresult = torch.compile(net)");  // attribute access, not the builtins
    expect_allowed("open('src/main.c', 'w').write('int main(){return 0;}')");
    expect_allowed("x = {'a': 1}\nprint(x['a'])");
    expect_allowed(R"j(import re
re.sub(r'\w+\s*\d', '', 'x 1'))j");  // regex escapes are not Windows paths
    expect_allowed(R"j(print(r'\d+\.\d+'))j");
}

TEST_F(AuditFixture, DangerousWordsInStringsAndCommentsDoNotTrip) {
    expect_allowed("print('exec(open(x)) and eval(y) and import socket and __import__')");
    expect_allowed("# exec('boom')\n# import socket\nprint(1)");
    expect_allowed(
        "\"\"\"docstring mentioning exec(), eval(), subprocess.run(['rm']) and http://example.com\"\"\"\n");
    expect_allowed("s = 'import ctypes'\nprint(s)");
    expect_allowed("print(f'{{exec}} literal braces {1 + 1}')");
}

TEST_F(AuditFixture, OversizedCodeIsRejectedAsMalformed) {
    AuditPolicy p = default_audit_policy();
    p.max_code_bytes = 100;
    auto r = ToolAudit::create(root, p);
    ASSERT_TRUE(r);
    const auto v = r.value().audit_tool_call("python_repl", std::string(101, '#'));
    EXPECT_FALSE(v.allowed);
    EXPECT_EQ(v.rule, "malformed");
    EXPECT_TRUE(r.value().audit_tool_call("python_repl", std::string(100, '#')).allowed);
}

// ---------------------------------------------------------------------------------------------------------------
// Dynamic code
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, DynamicCodeBuiltinsAreDenied) {
    expect_denied("exec('print(1)')", "dynamic_code");
    expect_denied("x = eval('1+1')", "dynamic_code");
    expect_denied("c = compile('1', 'f', 'eval')", "dynamic_code");
    expect_denied("breakpoint()", "dynamic_code");
    expect_denied("__import__('os').system('ls')", "dynamic_code");
    expect_denied("import importlib\nimportlib.import_module('socket')", "");
    expect_denied("importlib.import_module('socket')", "dynamic_code");
    expect_denied("import ctypes", "dynamic_code");
    expect_denied("from ctypes import windll", "dynamic_code");
    expect_denied("import ctypes.wintypes as w", "dynamic_code");
    expect_denied("import runpy\nrunpy.run_path('x.py')", "dynamic_code");
    expect_denied("import pickle", "dynamic_code");
}

TEST_F(AuditFixture, ExecEvalReferencedWithoutDirectCallAreDenied) {
    expect_denied("f = exec\nf('print(1)')", "dynamic_code");
    expect_denied("run = eval; run('1')", "dynamic_code");
    expect_denied("fn = [exec][0]", "dynamic_code");
    expect_denied("exec \\\n('print(1)')", "dynamic_code");
    expect_denied("(exec\n('print(1)'))", "dynamic_code");
}

TEST_F(AuditFixture, InterpreterInternalsEscapeHatchesAreDenied) {
    expect_denied("__builtins__.__dict__['ex' + 'ec']('1')", "dynamic_code");
    expect_denied("getattr(__builtins__, 'ev' 'al')('1')", "dynamic_code");
    expect_denied("().__class__.__bases__[0].__subclasses__()", "dynamic_code");
    expect_denied("f.__globals__['os']", "dynamic_code");
    expect_denied("f.__code__", "dynamic_code");
}

TEST_F(AuditFixture, UnicodeLookalikeIdentifiersAreDenied) {
    // Python NFKC-normalises identifiers, so a full-width spelling of exec is exec.
    expect_denied("\xEF\xBD\x85\xEF\xBD\x98\xEF\xBD\x85\xEF\xBD\x83('print(1)')", "dynamic_code");
    expect_denied("caf\xC3\xA9 = 1", "dynamic_code");  // any non-ASCII identifier is refused: unauditable
}

TEST_F(AuditFixture, FStringExpressionsAreAudited) {
    expect_denied("f\"{exec('print(1)')}\"", "dynamic_code");
    expect_denied("print(f'{__import__(\"os\").system(\"ls\")}')", "dynamic_code");
    expect_denied("x = rf'{eval(\"1\")}'", "dynamic_code");
    expect_denied("f'''{\n__import__('socket')\n}'''", "dynamic_code");
    expect_denied("f\"{f'{exec(1)}'}\"", "dynamic_code");
    expect_denied("f'{os.system(cmd)}'", "unauditable_command");
    expect_denied("f'{open(\"/etc/passwd\").read()}'", "path_escape");
    expect_allowed("name = 'x'\nprint(f'{name!r:>10} {len(name)}')");
}

TEST_F(AuditFixture, FStringsNestedFourDeepWithDistinctQuotesAreStillAudited) {
    // Four distinct delimiters is the deepest nesting the lexer can tell apart; recursion is capped at that
    // depth.
    expect_denied(R"j(f"""{f'''{f"{f'{exec(1)}'}"}'''}""")j", "dynamic_code");
    expect_allowed(R"j(f"""{f'''{f"{f'{1 + 1}'}"}'''}""")j");
}

// ---------------------------------------------------------------------------------------------------------------
// Network and process modules
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, NetworkModulesAreDeniedInEveryImportForm) {
    for (const char* code :
         {"import socket", "import urllib.request", "from urllib.request import urlopen", "import requests",
          "import http.client", "from http import client", "import ssl", "import os, socket",
          "import os; import socket", "if True: import socket", "import os.path as p, requests",
          "import json,\\\n socket", "import smtplib", "from . import x\nimport ftplib", "import httpx as h",
          "import websockets", "from socket import *", "    import socket", "import mcp"}) {
        expect_denied(code, "network");
    }
}

TEST_F(AuditFixture, ProcessModulesAndCallsAreDenied) {
    expect_denied("import multiprocessing", "process_spawn");
    expect_denied("import psutil", "process_spawn");
    expect_denied("import signal", "process_spawn");
    expect_denied("os.kill(1, 9)", "process_spawn");
    expect_denied("os.execv('/bin/sh', ['sh'])", "process_spawn");
    expect_denied("os.spawnl(0, 'x', 'x')", "process_spawn");
    expect_denied("os.startfile('x.exe')", "process_spawn");
    expect_denied("os.fork()", "process_spawn");
    expect_denied("from os import system", "process_spawn");
    expect_denied("from os import path, execv", "process_spawn");
    expect_denied("from os import *", "process_spawn");
    expect_denied("subprocess.Popen(['git', 'status']).kill()\nsubprocess.signal_x(1)", "process_spawn");
    expect_denied("import asyncio\nasyncio.create_subprocess_shell('ls')", "process_spawn");
}

TEST_F(AuditFixture, AliasingOsOrSubprocessIsDenied) {
    expect_denied("import subprocess as sp", "unauditable_command");
    expect_denied("import os as o", "unauditable_command");
    expect_denied("import subprocess\nsp = subprocess\nsp.run('git status')", "unauditable_command");
    expect_denied("import os\ns = getattr(os, 'sys' + 'tem')", "unauditable_command");
    expect_denied("from subprocess import run", "unauditable_command");
    expect_denied("from subprocess import *", "unauditable_command");
}

TEST_F(AuditFixture, CustomDeniedImportListIsHonoured) {
    AuditPolicy p = default_audit_policy();
    p.denied_imports = {"numpy"};
    auto r = ToolAudit::create(root, p);
    ASSERT_TRUE(r);
    EXPECT_FALSE(r.value().audit_tool_call("python_repl", "import numpy as np").allowed);
    EXPECT_TRUE(r.value().audit_tool_call("python_repl", "import math").allowed);
}

// ---------------------------------------------------------------------------------------------------------------
// String / comment / continuation obfuscation
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, CodeAfterStringsAndCommentsIsStillScanned) {
    expect_denied("x = 'harmless'  # comment\nimport socket", "network");
    expect_denied("s = \"\"\"multi\nline\"\"\"; exec('1')", "dynamic_code");
    expect_denied("s = 'it\\'s'\nimport socket", "network");
    expect_denied("s = r'\\'\nimport socket",
                  "network");  // raw string with an escaped quote keeps the string open
    expect_denied("# first\n# second\nexec('1')", "dynamic_code");
    expect_denied("x = b'bytes'; eval(x)", "dynamic_code");
    expect_denied("x = 'a' 'b' 'c'; exec(x)", "dynamic_code");
}

TEST_F(AuditFixture, LineContinuationsDoNotHideCalls) {
    expect_denied("import \\\nsocket", "network");
    expect_denied("os.\\\nsystem('rm x')", "command_not_allowed");
    expect_denied("subprocess.\\\nrun('curl http://x')", "");
    expect_denied("x = (1,\n 2); import socket", "network");
    expect_denied("x = 1\r\nimport socket\r\n", "network");
    expect_denied("x = 1\\\r\n; import socket", "network");
}

TEST_F(AuditFixture, HexUnicodeAndOctalEscapesInPathsAreDecoded) {
    expect_denied("open('\\x2fetc/passwd')", "path_escape");
    expect_denied("open('\\u002fetc/passwd')", "path_escape");
    expect_denied("open('\\U0000002fetc/passwd')", "path_escape");
    expect_denied("open('\\057etc/passwd')", "path_escape");
    expect_denied("open('\\x2e\\x2e/\\x2e\\x2e/secret')", "path_escape");
    expect_allowed("print('\\x41\\u0042\\103')");
}

TEST_F(AuditFixture, EscapedCommandNamesAreDecodedBeforeChecking) {
    expect_denied("bash('\\x63url http://example.com')", "");
    expect_denied("os.system('\\x72m -rf x')", "command_not_allowed");
}

TEST_F(AuditFixture, UnterminatedAndMalformedInputNeverThrowsAndIsNotFreePass) {
    for (const char* code : {"'unterminated", "\"\"\"never closed\nexec('1')", "exec(", "((((((", "))))",
                             "f'{", "f'{{{{", "\\", "\\\n", "'\\", "b'\\x"}) {
        EXPECT_NO_THROW({ (void)py(code); }) << code;
    }
    // an unclosed f-string expression is still audited
    EXPECT_FALSE(py("f'{exec(1)").allowed);
}

TEST_F(AuditFixture, HostileBytesNeverThrow) {
    std::string junk;
    for (int i = 0; i < 4096; ++i) junk.push_back(static_cast<char>(i * 31 + 7));
    EXPECT_NO_THROW({ (void)py(junk); });
    EXPECT_NO_THROW({ (void)py(std::string("exec\0('1')", 10)); });
    EXPECT_NO_THROW({ (void)py(std::string(60000, '(')); });
    EXPECT_NO_THROW({ (void)py(std::string(60000, '\\')); });
}

// ---------------------------------------------------------------------------------------------------------------
// Literal shell commands against sandbox_policy
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, AllowedLiteralCommandsPass) {
    expect_allowed("bash('git status')");
    expect_allowed("bash(\"git log\")");
    expect_allowed("bash('git diff --stat')");
    expect_allowed("os.system('ls src')");
    expect_allowed("subprocess.run(['git', 'status'])");
    expect_allowed("subprocess.run(['git', 'status'], capture_output=True)");
    expect_allowed("subprocess.check_output('git log', text=True)");
    expect_allowed("bash('cmake --version')");
    expect_allowed("bash('python -m unittest')");
    expect_allowed("bash('python -m pytest -q')");
    expect_allowed("bash('python src/tool.py')");
}

TEST_F(AuditFixture, CommandsOutsideTheAllowListAreDenied) {
    for (const char* cmd : {"rm -rf x",
                            "curl http://example.com",
                            "wget x",
                            "powershell -c ls",
                            "cmd /c dir",
                            "bash -c ls",
                            "sh -c ls",
                            "git push",
                            "git clone x",
                            "git config user.name x",
                            "git remote add o x",
                            "ssh host",
                            "nc -l 1",
                            "python -c print(1)",
                            "python",
                            "python -m http.server",
                            "python -m pip install x",
                            "pip install x",
                            "del x",
                            "taskkill /f",
                            "reg add x",
                            "certutil -urlcache"}) {
        const auto v = py(std::string("bash('") + cmd + "')");
        EXPECT_FALSE(v.allowed) << cmd;
        EXPECT_TRUE(v.rule == "command_not_allowed" || v.rule == "unauditable_command" || v.rule == "network")
            << cmd << " " << v.rule;
    }
}

TEST_F(AuditFixture, ShellMetacharactersAreUnauditable) {
    for (const char* cmd :
         {"git status; rm x", "git status && rm x", "git status | cat", "git status > out.txt",
          "git status `id`", "git status $(id)", "git status %PATH%", "git status\nrm x", "git status & calc",
          "git status ^ x", "ls *", "ls ?", "ls {a,b}", "ls ~", "git status !"}) {
        const AuditVerdict v = audit->audit_command_line(cmd);
        EXPECT_FALSE(v.allowed) << cmd;
        EXPECT_EQ(v.rule, "unauditable_command") << cmd;
    }
}

TEST_F(AuditFixture, CommandLineQuotingAndLimits) {
    EXPECT_FALSE(audit->audit_command_line("git 'status").allowed);
    EXPECT_FALSE(audit->audit_command_line("").allowed);
    EXPECT_FALSE(audit->audit_command_line("   ").allowed);
    EXPECT_FALSE(audit->audit_command_line(std::string(5000, 'a')).allowed);
    EXPECT_FALSE(audit->audit_command_line(std::string("git status") + '\0' + "x").allowed);
    EXPECT_TRUE(audit->audit_command_line("git \"status\"").allowed);
    EXPECT_TRUE(audit->audit_command_line("git status").allowed);
}

TEST_F(AuditFixture, ProgramNamesAreCanonicalised) {
    EXPECT_FALSE(audit->audit_command_line("CURL.EXE http://x").allowed);
    EXPECT_FALSE(audit->audit_command_line("Curl x").allowed);
    EXPECT_TRUE(audit->audit_command_line("GIT.exe status").allowed);
    EXPECT_FALSE(audit->audit_command_line("C:\\Windows\\System32\\cmd.exe /c dir").allowed);
    EXPECT_FALSE(audit->audit_command_line("/usr/bin/curl x").allowed);
}

TEST_F(AuditFixture, NonLiteralCommandsAreUnauditable) {
    for (const char* code :
         {"bash(cmd)", "bash(f'git {x}')", "bash('git ' + sub)", "bash('git %s' % sub)", "bash(*args)",
          "bash()", "bash('git status' if x else 'ls')", "bash(b'git status')", "os.system(cmd)",
          "os.system(f'ls {d}')", "os.popen(c)", "subprocess.run(cmd)", "subprocess.run(['git', sub])",
          "subprocess.run([*args])", "subprocess.run([])", "subprocess.run(['git', 'a' + 'b'])",
          "subprocess.Popen(c, shell=True)", "subprocess.check_call(args)", "subprocess.getoutput(f'{c}')"}) {
        const AuditVerdict v = py(code);
        EXPECT_FALSE(v.allowed) << code;
        EXPECT_EQ(v.rule, "unauditable_command") << code;
    }
}

TEST_F(AuditFixture, AdjacentLiteralsAreConcatenatedBeforeAudit) {
    expect_allowed("bash('git ' 'status')");
    expect_denied("bash('cu' 'rl http://x')", "");
    expect_denied("bash('rm' ' -rf x')", "command_not_allowed");
}

TEST_F(AuditFixture, SubprocessWithoutLiteralOrWithUnlistedFunctionIsDenied) {
    expect_denied("subprocess.run(['rm', '-rf', 'x'])", "command_not_allowed");
    expect_denied("subprocess.call('curl http://x')", "");
    expect_denied("subprocess.startupinfo()", "process_spawn");
}

TEST_F(AuditFixture, NetworkArgumentsToAllowedProgramsAreDenied) {
    expect_denied("bash('git status http://example.com/x')", "");
    expect_denied("bash('ls https://example.com')", "");
    expect_denied("subprocess.run(['python', 'tool.py', 'http://x'])", "");
}

TEST_F(AuditFixture, PathsInCommandsMustStayInsideTheRoot) {
    expect_denied("bash('cat /etc/passwd')", "");
    expect_denied("bash('cat ../../outside.txt')", "");
    expect_denied("bash('cat C:\\\\Windows\\\\win.ini')", "");
    expect_denied("bash('ls --path=/etc')", "");
    expect_denied("bash('cat ../elsewhere/x')", "");
    expect_allowed("bash('cat src/main.c')");
    expect_allowed("bash('ls src')");
}

TEST_F(AuditFixture, PythonCommandForms) {
    EXPECT_TRUE(audit->audit_argv({"python", "-m", "unittest", "discover"}).allowed);
    EXPECT_TRUE(audit->audit_argv({"python3", "-u", "-m", "pytest"}).allowed);
    EXPECT_TRUE(audit->audit_argv({"py", "script.py", "arg"}).allowed);
    EXPECT_TRUE(audit->audit_argv({"python.exe", "-B", "tool.PY"}).allowed);
    for (const std::vector<std::string>& argv : std::vector<std::vector<std::string>>{
             {"python"},
             {"python", "-m"},
             {"python", "-m", "pip"},
             {"python", "-m", "http.server"},
             {"python", "-c", "print(1)"},
             {"python", "-", "x"},
             {"python", "-m", "unittest", "http://x"},
             {"python", "-m", "unittest", "/etc/passwd"},
             {"python", "-m", "unittest", "../../x"},
             {"python", "/etc/evil.py"},
             {"python", "../../evil.py"},
             {"python", "evil.txt"},
             {},
         }) {
        EXPECT_FALSE(audit->audit_argv(argv).allowed) << (argv.empty() ? "<empty>" : argv[0]);
    }
}

TEST_F(AuditFixture, ExtraCommandsAreAppliedOnTopOfTheDefaults) {
    AuditPolicy p = default_audit_policy();
    p.extra_commands.push_back({"mytool", {"build"}, {}});
    auto r = ToolAudit::create(root, p);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r.value().audit_command_line("mytool build").allowed);
    EXPECT_FALSE(r.value().audit_command_line("mytool deploy").allowed);
    EXPECT_FALSE(audit->audit_command_line("mytool build").allowed);
}

// ---------------------------------------------------------------------------------------------------------------
// Path literals
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, AbsoluteAndTraversalPathLiteralsOutsideTheRootAreDenied) {
    expect_denied("open('/etc/passwd')", "path_escape");
    expect_denied("open('C:\\\\Windows\\\\win.ini')", "path_escape");
    expect_denied("open('C:/Windows/win.ini')", "path_escape");
    expect_denied("open('\\\\\\\\server\\\\share\\\\x')", "path_escape");
    expect_denied("open('../../x')", "path_escape");
    expect_denied("open('src/../../x')", "path_escape");
    expect_denied("open('src/../notes.txt')",
                  "path_escape");  // PathGuard refuses every `..` segment, even a harmless one
    expect_denied("open('~/.ssh/id_rsa')", "path_escape");
    expect_denied(R"j(open(r'\Windows\win.ini'))j", "path_escape");
    expect_denied("Path.home()", "path_escape");
    expect_denied("os.path.expanduser('x')", "path_escape");
    expect_denied("open(" + std::string("'") + (outside / "f.txt").generic_string() + "')", "path_escape");
}

TEST_F(AuditFixture, PathLiteralsInsideTheRootAreAllowed) {
    expect_allowed("open('src/main.c')");
    expect_allowed("open('./notes.txt')");
    expect_allowed("open(" + std::string("'") + (root / "src" / "x.c").generic_string() + "')");
}

TEST_F(AuditFixture, TextThatOnlyLooksLikeAPathIsNotFlagged) {
    expect_allowed("print('a/b and /c d')");  // contains a space, so it is prose, not a path
    expect_allowed("print('1/2')");
    expect_allowed("print('/')");
    expect_allowed(
        "print('see http://example.com/docs')");  // URL text is data unless it is a command argument
    expect_allowed("print('use ../ to go up')");
    expect_allowed("x = '/'");
}

TEST_F(AuditFixture, FStringPathPrefixIsChecked) {
    expect_denied("open(f'/etc/{name}')", "path_escape");
    expect_allowed("open(f'src/{name}.c')");
}

// ---------------------------------------------------------------------------------------------------------------
// Documented limits: a lexical scan cannot see these. They are NOT coverage; the Job Object and a disposable
// workspace are the actual containment (docs/agent.md). If one of these starts being denied that is an
// improvement, not a regression, and the test should be moved above.
// ---------------------------------------------------------------------------------------------------------------
TEST_F(AuditFixture, DocumentedLimit_RuntimeBuiltStringsAreNotFollowed) {
    // Building a path from pieces: the audit only sees harmless literals.
    EXPECT_TRUE(py("p = chr(47) + 'etc' + chr(47) + 'passwd'\nprint(open(p).read())").allowed);
    EXPECT_TRUE(py("import os\nfor f in os.listdir('.'):\n    os.remove(f)").allowed);
    EXPECT_TRUE(py("import shutil\nshutil.rmtree('src')").allowed);
}

TEST_F(AuditFixture, DocumentedLimit_AllowListedBuildToolsRunProjectCode) {
    // `python -m unittest` or `make` execute whatever the repository contains.
    EXPECT_TRUE(py("bash('python -m unittest')").allowed);
    EXPECT_TRUE(py("bash('make')").allowed);
}
