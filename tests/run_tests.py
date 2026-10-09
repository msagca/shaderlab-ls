"""End-to-end tests: drives shaderlab-ls over stdio like an editor would.

Usage: python tests/run_tests.py path/to/shaderlab-ls.exe
"""

import json
import os
import pathlib
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

FIXTURES = pathlib.Path(__file__).parent / "fixtures"


class Client:
    def __init__(self, exe):
        self.proc = subprocess.Popen([exe, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.next_id = 1
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        stream = self.proc.stdout
        while True:
            length = None
            while True:
                line = stream.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                name, _, value = line.decode().partition(":")
                if name.lower() == "content-length":
                    length = int(value)
            self.messages.put(json.loads(stream.read(length)))

    def send(self, message):
        body = json.dumps(message).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params, timeout=10):
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = time.time() + timeout
        while time.time() < deadline:
            message = self.messages.get(timeout=deadline - time.time())
            if message.get("id") == request_id:
                if "error" in message:
                    raise AssertionError(f"{method} failed: {message['error']}")
                return message["result"]
            self.pending.append(message)
        raise AssertionError(f"{method} timed out")

    pending = []

    def diagnostics(self, uri, count, timeout=15):
        """Returns the `count`-th publishDiagnostics for uri (the first is syntax-only, the second includes FXC)."""
        seen = 0
        deadline = time.time() + timeout
        backlog = list(self.pending)
        self.pending.clear()
        while time.time() < deadline:
            message = backlog.pop(0) if backlog else self.messages.get(timeout=deadline - time.time())
            if message.get("method") == "textDocument/publishDiagnostics" and message["params"]["uri"] == uri:
                seen += 1
                if seen == count:
                    return message["params"]["diagnostics"]
        raise AssertionError(f"diagnostics for {uri} timed out")


def uri_for(path):
    return path.resolve().as_uri()


failures = []
# Set from the server's startup log. Shaders compile with FXC where it exists (Windows) and DXC elsewhere; with
# neither, HLSL is not compiled, there is one publishDiagnostics per document instead of two, and the compile checks
# below do not apply.
compiler = None  # "fxc" or "dxc": what a shader without #pragma use_dxc compiles with
has_dxc = False


def find_dxc():
    """A DXC to test with: $SHADERLAB_LS_TEST_DXC, else the newest Unity editor's on Windows. None leaves it to the
    server, which then looks on the system's library path."""
    if os.environ.get("SHADERLAB_LS_TEST_DXC"):
        return os.environ["SHADERLAB_LS_TEST_DXC"]
    if sys.platform == "win32":
        hub = pathlib.Path(os.environ.get("ProgramFiles", r"C:\Program Files"), "Unity", "Hub", "Editor")
        editors = sorted(hub.glob("*/Editor/Data/Tools/dxcompiler.dll"))
        if editors:
            return str(editors[-1])
    return None


def diags(client, uri, timeout=15):
    """The diagnostics of the last pass: the compiler's where there is one, the syntax ones where there is not."""
    return client.diagnostics(uri, 2 if compiler else 1, timeout=timeout)


def skip(description, why="no HLSL compiler"):
    print(f"  [skip] {description} ({why})")


def check(condition, description):
    status = "ok  " if condition else "FAIL"
    print(f"  [{status}] {description}")
    if not condition:
        failures.append(description)


def has(diagnostics, line, fragment, severity=None, source=None):
    for d in diagnostics:
        if d["range"]["start"]["line"] == line - 1 and fragment in d["message"]:
            if severity is not None and d["severity"] != severity:
                continue
            if source is not None and d.get("source") != source:
                continue
            return True
    return False


def labels(result):
    items = result["items"] if isinstance(result, dict) else result
    return {item["label"] for item in items}


def open_doc(client, path):
    uri = uri_for(path)
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": uri, "languageId": "shaderlab", "version": 1, "text": path.read_text(encoding="utf-8")}
    })
    return uri


def main():
    exe = sys.argv[1]
    client = Client(exe)
    init = client.request("initialize", {
        "processId": None,
        "rootUri": None,
        "capabilities": {
            "general": {"positionEncodings": ["utf-16"]},
            "textDocument": {"completion": {"completionItem": {"snippetSupport": True}}},
        },
        "initializationOptions": {"diagnostics": {"delay": 0}, "indexCache": False, **({"dxcPath": find_dxc()} if find_dxc() else {})},
    })
    client.notify("initialized", {})
    global compiler, has_dxc
    log = " ".join(m["params"]["message"] for m in client.pending if m.get("method") == "window/logMessage")
    has_dxc = "DXC (" in log
    compiler = "fxc" if "FXC (" in log else "dxc" if has_dxc else None

    print("initialize")
    caps = init["capabilities"]
    check(caps["positionEncoding"] == "utf-16", "negotiates utf-16")
    check(caps["hoverProvider"] and caps["definitionProvider"] and caps["documentSymbolProvider"] and caps["referencesProvider"]
          and caps["documentHighlightProvider"] and caps["renameProvider"]["prepareProvider"], "advertises features")

    print("errors.shader")
    errors = FIXTURES / "errors.shader"
    uri = open_doc(client, errors)
    d = diags(client, uri)
    check(has(d, 7, "Unknown property type 'Float2'", 1), "unknown property type")
    check(has(d, 8, "default value of a Color property", 1), "property default value shape")
    check(has(d, 13, "Invalid PreviewType value 'Cube'", 2), "closed tag values")
    check(has(d, 14, "Invalid value 'Sideways' for Cull", 1), "command values")
    check(has(d, 16, "'_Missing' is not declared", 4), "undeclared property references are hints")
    check(has(d, 21, "Invalid ColorMask channels 'RGBX'", 1), "ColorMask channels")
    check(has(d, 22, "Offset <factor>, <units>", 1), "Offset arity")
    check(has(d, 25, "from 0 through 255", 1), "Stencil Ref range")
    check(has(d, 26, "Invalid comparison operation 'Sometimes'", 1), "Stencil comparison values")
    check(has(d, 32, "Unknown #pragma target value", 2), "pragma target values")
    if compiler:
        # FXC says "undeclared identifier", DXC "use of undeclared identifier".
        check(has(d, 35, "undeclared identifier 'undefinedThing'", 1, compiler), f"{compiler} error mapped into the Pass")
        compiled = [x for x in d if "undefinedThing" in x["message"]]
        # "            float4 frag() : SV_Target { /* é */ return _Color * " is 64 UTF-16 units.
        check(bool(compiled) and compiled[0]["range"]["start"]["character"] == 64,
              f"{compiler} byte columns converted to UTF-16")
    else:
        skip("compile errors mapped into the Pass")
    check(not any(x["range"]["start"]["line"] == 14 for x in d), "valid Blend with a property reference")
    missing = next((x for x in d if "'_Missing' is not declared" in x["message"]), None)
    fixes = client.request("textDocument/codeAction", {"textDocument": {"uri": uri}, "range": missing["range"] if missing else {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
                                                       "context": {"diagnostics": [missing] if missing else []}})
    fix = next((a for a in fixes if a["title"] == "Declare _Missing in Properties"), None)
    edit = fix["edit"]["changes"][uri][0] if fix else {}
    check(fix is not None and fix["kind"] == "quickfix" and '_Missing ("_Missing", Float) = 0' in edit.get("newText", ""),
          "a quick fix declares an undeclared property")

    print("valid.shader")
    valid_uri = open_doc(client, FIXTURES / "valid.shader")
    d = diags(client, valid_uri)
    faded = [x for x in d if x.get("tags") == [1]]
    d = [x for x in d if x.get("tags") != [1]]
    check(d == [], "no diagnostics for a valid shader: " + "; ".join(x["message"] for x in d))
    check([(x["range"]["start"]["line"], x["severity"]) for x in faded] == [(67, 4)],
          "the #ifdef branch the default variant leaves out is faded, and nothing else")

    print("variants")
    variant_uri = uri_for(FIXTURES / "unsaved_variant.shader")
    # An error only the _EMISSION variant compiles.
    variant_text = (FIXTURES / "valid.shader").read_text(encoding="utf-8").replace("_BaseColor * 2.0", "_BaseColor * missingInEmission")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": variant_uri, "languageId": "shaderlab", "version": 1, "text": variant_text}})
    diags(client, variant_uri)

    def actions_on(line, diagnostics=()):
        return client.request("textDocument/codeAction", {"textDocument": {"uri": variant_uri},
                              "range": {"start": {"line": line, "character": 0}, "end": {"line": line, "character": 0}},
                              "context": {"diagnostics": list(diagnostics)}})

    compiled = []

    def faded_lines():
        found = diags(client, variant_uri)
        compiled[:] = [x["message"] for x in found if x.get("source") in ("fxc", "dxc")]
        return sorted(x["range"]["start"]["line"] for x in found if x.get("tags") == [1])

    titles = [a["title"] for a in actions_on(46)]
    check(titles == ["Check the variant with _EMISSION"], "a keyword set offers its other variants: " + str(titles))
    check([a["title"] for a in actions_on(47)] == ["Check the variant with _ALPHATEST_ON"], "so does a lone shader_feature")
    emission = actions_on(46)[0]["command"]
    client.request("workspace/executeCommand", {"command": emission["command"], "arguments": emission["arguments"]})
    check(faded_lines() == [69], "the chosen variant decides which branch is faded")
    if compiler:
        check(any("missingInEmission" in m and "[variant: _EMISSION]" in m for m in compiled), "and is the one compiled, named on what it reports")
    else:
        skip("the chosen variant is compiled")
    titles = [a["title"] for a in actions_on(46)]
    check(titles == ["Check the variant without _EMISSION", "Check the default variant again"], "and can be undone: " + str(titles))
    client.request("workspace/executeCommand", {"command": emission["command"], "arguments": [variant_uri]})
    check(faded_lines() == [67] and not compiled, "back to the default variant, which has no such error")

    include_uri = uri_for(FIXTURES / "unsaved_conditions.hlsl")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": include_uri, "languageId": "hlsl", "version": 1, "text": "\n".join([
        "#define ON 1",
        "#if ON == 2",
        "float never;",
        "#endif",
        "#ifdef DEFINED_BY_THE_INCLUDER",
        "float maybe;",
        "#endif",
        "#if !defined(ON) || defined(SHADER_API_D3D11) && 0",
        "float neither;",
        "#elif 1",
        "float always;",
        "#else",
        "float after;",
        "#endif",
    ])}})
    hints = [x["range"]["start"]["line"] for x in client.diagnostics(include_uri, 1) if x.get("tags") == [1]]
    check(hints == [2, 8, 12], "an include file fades what is false for certain, not what its includer may define: " + str(hints))

    variant_kernel = "\n".join([
        "#pragma kernel K",
        "#pragma multi_compile _ _FANCY",
        "struct S { float3 a;",
        "#ifdef _FANCY",
        "    float4 fancy;",
        "#endif",
        "};",
        "#ifdef _FANCY",
        "half4 v;",
        "#else",
        "float2 v;",
        "#endif",
        "[numthreads(1, 1, 1)] void K() { S s; s.; v.; }",
    ])
    kernel_uri = uri_for(FIXTURES / "unsaved_variant_kernel.compute")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": kernel_uri, "languageId": "hlsl", "version": 1, "text": variant_kernel}})
    last = variant_kernel.split("\n")[-1]

    def kernel_members(needle):
        position = {"line": 12, "character": last.index(needle) + len(needle)}
        return labels(client.request("textDocument/completion", {"textDocument": {"uri": kernel_uri}, "position": position}))

    check(kernel_members("s.") == {"a"}, "a field an #if leaves out of the variant is not offered")
    check("y" in kernel_members("v.") and "z" not in kernel_members("v."), "of two declarations, the one the variant compiles is used")

    # Declarations in #if branches the variant leaves out: a use goes to the one its own #if guarantees.
    guarded_kernel = "\n".join([
        "#pragma kernel K",
        "#pragma multi_compile _ _A _B",
        "#ifdef _A",
        "float Pick() { return 1; }",
        "#endif",
        "#if defined(_B)",
        "float Pick() { return 2; }",
        "#endif",
        "[numthreads(1, 1, 1)] void K() {",
        "#ifdef _B",
        "    Pick();",
        "#elif defined(_A)",
        "    Pick();",
        "#endif",
        "#if defined(_B) && defined(_C)",
        "    Pick();",
        "#endif",
        "}",
    ])
    guarded_uri = uri_for(FIXTURES / "unsaved_guarded_kernel.compute")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": guarded_uri, "languageId": "hlsl", "version": 1, "text": guarded_kernel}})
    for line, expected, branch in [(10, 6, "#ifdef _B"), (12, 3, "#elif after it"), (15, 6, "#if that ands _B with more")]:
        target = client.request("textDocument/definition", {"textDocument": {"uri": guarded_uri}, "position": {"line": line, "character": 5}})
        check(target is not None and target["range"]["start"]["line"] == expected, f"a call under {branch} goes to the definition that branch compiles")
    client.notify("textDocument/didClose", {"textDocument": {"uri": guarded_uri}})

    # A use whose own #if guarantees none of them: the variant decides, however deep each definition is nested.
    nested_kernel = "\n".join([
        "#pragma kernel K",
        "#if defined(_A)",
        "float Pick() { return 1; }",
        "#else",
        "#if defined(_B)",
        "float Pick() { return 2; }",
        "#else",
        "float Pick() { return 3; }",
        "#endif",
        "#endif",
        "[numthreads(1, 1, 1)] void K() {",
        "    Pick();",
        "}",
    ])
    nested_uri = uri_for(FIXTURES / "unsaved_nested_kernel.compute")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": nested_uri, "languageId": "hlsl", "version": 1, "text": nested_kernel}})
    target = client.request("textDocument/definition", {"textDocument": {"uri": nested_uri}, "position": {"line": 11, "character": 5}})
    check(target is not None and target["range"]["start"]["line"] == 7,
          "a call no #if guards goes to the definition the variant compiles, though it is nested deeper than another")
    client.notify("textDocument/didClose", {"textDocument": {"uri": nested_uri}})

    # Two headers of one function in #if branches, each opening its body: the declarations after it are still found.
    split_kernel = "\n".join([
        "#pragma kernel K",
        "#if defined(_A)",
        "float4 Pre(float4 p) {",
        "#elif defined(_B)",
        "float4 Pre(float4 p, uint i) {",
        "#endif",
        "    return p;",
        "}",
        "#ifdef _A",
        "float After() { return 1; }",
        "#endif",
        "[numthreads(1, 1, 1)] void K() {",
        "#ifdef _A",
        "    After();",
        "#endif",
        "}",
    ])
    split_uri = uri_for(FIXTURES / "unsaved_split_kernel.compute")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": split_uri, "languageId": "hlsl", "version": 1, "text": split_kernel}})
    target = client.request("textDocument/definition", {"textDocument": {"uri": split_uri}, "position": {"line": 13, "character": 5}})
    check(target is not None and target["range"]["start"]["line"] == 9, "a definition after a function with a header in each #if branch is found")
    outline = client.request("textDocument/documentSymbol", {"textDocument": {"uri": split_uri}})
    names = [s["name"] for s in outline]
    check(names.count("Pre") == 2 and "After" in names and "K" in names, "and each branch's header is a symbol, once: " + str(names))
    client.notify("textDocument/didClose", {"textDocument": {"uri": split_uri}})


    print("unity_compat.shader")
    compat_uri = open_doc(client, FIXTURES / "unity_compat.shader")
    d = diags(client, compat_uri)
    unexpected = [x for x in d if x["severity"] < 4 and "Sideways" not in x["message"]]
    check(unexpected == [], "undocumented forms used by Unity's own shaders are accepted: " + "; ".join(
        f"{x['range']['start']['line'] + 1}: {x['message']}" for x in unexpected))
    check(has(d, 45, "Invalid value 'Sideways' for Cull", 1), "commands after a legacy command are still validated")
    check(has(d, 21, "'_CullMode' is not declared", 4), "undeclared property reference hint")

    print("byte order mark in an include")
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp)
        (folder / "bom.hlsl").write_bytes(b"\xef\xbb\xbf#define BOM_VALUE 1\n")
        compute = folder / "bom.compute"
        compute.write_text('#pragma kernel CSMain\n#include "bom.hlsl"\n[numthreads(1, 1, 1)]\nvoid CSMain() { float x = BOM_VALUE; }\n', encoding="utf-8")
        bom_uri = open_doc(client, compute)
        d = diags(client, bom_uri)
        check(d == [], "included files with a UTF-8 BOM compile: " + "; ".join(x["message"] for x in d))
        client.notify("textDocument/didClose", {"textDocument": {"uri": bom_uri}})
        client.diagnostics(bom_uri, 1)

    print("formatting")
    check(shutil.which("clang-format") is not None, "clang-format is on PATH (the expected output is laid out with it)")
    # Fixtures may be checked out with CRLF; the formatter keeps whatever it is given, so test with LF explicitly.
    source = (FIXTURES / "format_input.shader").read_bytes().replace(b"\r\n", b"\n")
    expected = (FIXTURES / "format_expected.shader").read_bytes().replace(b"\r\n", b"\n")
    # Unity's default layout is used only when no .clang-format is found from the file up to the root. The checks
    # that expect it therefore name a path in an empty temp folder rather than tests/fixtures, which a
    # .clang-format put anywhere in this repository - or above it - would reach.
    unstyled_dir = tempfile.TemporaryDirectory()
    unstyled = pathlib.Path(unstyled_dir.name)
    above = next((d for d in [unstyled, *unstyled.parents]
                  if (d / ".clang-format").exists() or (d / "_clang-format").exists()), None)
    check(above is None, "nothing above the temp folder configures clang-format"
          + (f", but {above} does: the default-layout checks below cannot hold" if above else ""))
    where = ["--assume-filename", str(unstyled / "format_input.shader")]
    run = subprocess.run([exe, "--format", "-"] + where, input=source, capture_output=True)
    check(run.returncode == 0 and run.stdout == expected, "output matches format_expected.shader")
    again = subprocess.run([exe, "--format", "-"] + where, input=expected, capture_output=True)
    check(again.stdout == expected, "formatting is idempotent")
    check(b"\n\n" not in expected and expected.endswith(b"}\n"),
          "MaxEmptyLinesToKeep is 0 by default, so the file has no empty lines")
    check(b"            void main()\n            {\n" in expected, "GLSLPROGRAM blocks are laid out as well")
    crlf = subprocess.run([exe, "--format", "-"] + where, input=source.replace(b"\n", b"\r\n"), capture_output=True)
    check(crlf.stdout == expected.replace(b"\n", b"\r\n"), "CRLF line endings are preserved")

    with tempfile.TemporaryDirectory() as temp:
        # A .clang-format anywhere above the file decides the layout of both the ShaderLab and the HLSL.
        project = pathlib.Path(temp)
        (project / ".clang-format").write_text("BasedOnStyle: LLVM\nUseTab: Always\nIndentWidth: 4\nTabWidth: 4\n")
        shader = project / "Configured.shader"
        shader.write_bytes(source)
        styled = subprocess.run([exe, "--format", str(shader)], capture_output=True).stdout
        check(b"\n\tProperties" in styled and b"\n\t\t\tHLSLPROGRAM" in styled, "UseTab indents with tabs")
        check(b'Shader "Tests/Format" {' in styled, "BreakBeforeBraces puts the '{' where clang-format would")
        check(b"\n\t\t\tfloat4 frag() : SV_Target { return 1; }" in styled, "clang-format lays out the HLSL")
        # LLVM keeps one empty line in a row: one left in the Properties, one in the HLSL, none at a block's edge.
        check(styled.count(b"\n\n") == 2 and b"\n\n\n" not in styled and styled.endswith(b"}\n"),
              "MaxEmptyLinesToKeep decides how many empty lines in a row survive")
        shader.write_bytes(styled)
        check(subprocess.run([exe, "--format", str(shader)], capture_output=True).stdout == styled,
              "formatting with a .clang-format is idempotent")

    broken = subprocess.run([exe, "--format", str(FIXTURES / "syntax.shader")], capture_output=True)
    check(broken.returncode == 1 and broken.stdout == b"" and b"missing '}'" in broken.stderr, "unbalanced files are refused")

    # .compute, .hlsl and .cginc files are HLSL from end to end: all of the file goes to clang-format, with the same
    # two HLSL fixes the code blocks of a .shader get - entry point attributes keep their own line and includes keep
    # their order. The fixtures folder has no .clang-format, so this is Unity's layout again.
    hlsl = (b'#pragma kernel CSMain\n#include "b.hlsl"\n#include "a.hlsl"\n\n\n'
            b'RWStructuredBuffer<float>  _Values;\n\n'
            b'[numthreads(8, 1, 1)]\nvoid CSMain(uint3 id : SV_DispatchThreadID)\n{\n  _Values[ id.x ] = 1;\n}\n')
    hlsl_expected = (b'#pragma kernel CSMain\n#include "b.hlsl"\n#include "a.hlsl"\n'
                     b'RWStructuredBuffer<float> _Values;\n'
                     b'[numthreads(8, 1, 1)]\nvoid CSMain(uint3 id : SV_DispatchThreadID)\n{\n    _Values[id.x] = 1;\n}\n')
    for extension in (".compute", ".hlsl", ".cginc"):
        hlsl_where = ["--assume-filename", str(unstyled / ("unsaved" + extension))]
        run = subprocess.run([exe, "--format", "-"] + hlsl_where, input=hlsl, capture_output=True)
        check(run.returncode == 0 and run.stdout == hlsl_expected, f"a {extension} file is laid out by clang-format")
        again = subprocess.run([exe, "--format", "-"] + hlsl_where, input=run.stdout, capture_output=True)
        check(again.stdout == hlsl_expected, f"formatting a {extension} file is idempotent")

    hlsl_where = ["--assume-filename", str(unstyled / "unsaved.compute")]
    crlf = subprocess.run([exe, "--format", "-"] + hlsl_where, input=hlsl.replace(b"\n", b"\r\n"), capture_output=True)
    check(crlf.stdout == hlsl_expected.replace(b"\n", b"\r\n"), "an HLSL file keeps its CRLF line endings")
    bom = subprocess.run([exe, "--format", "-"] + hlsl_where, input=b"\xef\xbb\xbf" + hlsl, capture_output=True)
    check(bom.stdout == b"\xef\xbb\xbf" + hlsl_expected, "an HLSL file keeps its UTF-8 BOM")
    without = subprocess.run([exe, "--format", "-", "--clang-format", "no-such-clang-format"] + hlsl_where,
                             input=hlsl, capture_output=True)
    check(without.returncode == 1 and without.stdout == b"" and b"clang-format" in without.stderr,
          "an HLSL file is left alone when clang-format cannot be run")

    # GLSL files are laid out the same way - clang-format reads them as C++, which they are close enough to - but
    # they are never analyzed, here as in a GLSLPROGRAM block.
    glsl = (b'#version 300 es\nlayout(location = 0) in  vec3 aPos;\n\n\n'
            b'void main()\n{\n  gl_Position = vec4( aPos, 1.0 );\n}\n')
    glsl_expected = (b'#version 300 es\nlayout(location = 0) in vec3 aPos;\n'
                     b'void main()\n{\n    gl_Position = vec4(aPos, 1.0);\n}\n')
    for extension in (".glsl", ".glslinc"):
        run = subprocess.run([exe, "--format", "-", "--assume-filename", str(unstyled / ("unsaved" + extension))],
                             input=glsl, capture_output=True)
        check(run.returncode == 0 and run.stdout == glsl_expected, f"a {extension} file is laid out by clang-format")

    format_uri = uri_for(unstyled / "format_input.shader")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": format_uri, "languageId": "shaderlab", "version": 1, "text": source.decode("utf-8")}})
    diags(client, format_uri)
    edits = client.request("textDocument/formatting", {"textDocument": {"uri": format_uri}, "options": {"tabSize": 4, "insertSpaces": True}})
    lines = source.decode("utf-8").split("\n")
    check(len(edits) == 1 and edits[0]["newText"] == expected.decode("utf-8")
          and edits[0]["range"]["end"]["line"] == len(lines) - 1, "LSP formatting returns the whole formatted document")
    # Range formatting: the lines of a range come out as whole-file formatting would leave them, and no others.
    source_text = source.decode("utf-8")
    expected_text = expected.decode("utf-8")

    def apply(text, edits):
        lines = text.split("\n")
        offsets = [0]
        for line in lines:
            offsets.append(offsets[-1] + len(line) + 1)
        def offset(position):
            return offsets[position["line"]] + position["character"] if position["line"] < len(lines) else len(text)
        for edit in sorted(edits, key=lambda e: offset(e["range"]["start"]), reverse=True):
            text = text[:offset(edit["range"]["start"])] + edit["newText"] + text[offset(edit["range"]["end"]):]
        return text

    def range_format(first, last):
        return client.request("textDocument/rangeFormatting", {
            "textDocument": {"uri": format_uri}, "options": {"tabSize": 4, "insertSpaces": True},
            "range": {"start": {"line": first, "character": 0}, "end": {"line": last, "character": 1}}})

    source_lines = source_text.split("\n")
    total = len(source_lines)
    whole = apply(source_text, range_format(0, total - 1))
    check(whole == expected_text, "range formatting over every line formats the whole file")
    pieces = source_text
    for first in range(0, total, 7):
        client.notify("textDocument/didChange", {"textDocument": {"uri": format_uri, "version": 2 + first},
                                                 "contentChanges": [{"text": pieces}]})
        pieces = apply(pieces, range_format(first, min(first + 6, total - 1)) if first < len(pieces.split("\n")) else [])
    client.notify("textDocument/didChange", {"textDocument": {"uri": format_uri, "version": 1000}, "contentChanges": [{"text": pieces}]})
    rest = range_format(0, len(pieces.split("\n")) - 1)
    check(apply(pieces, rest) == expected_text, "formatting a file range by range ends where formatting it whole does")
    client.notify("textDocument/didChange", {"textDocument": {"uri": format_uri, "version": 1001}, "contentChanges": [{"text": source_text}]})
    one = range_format(3, 3)
    touched = {line for e in one for line in range(e["range"]["start"]["line"], max(e["range"]["end"]["line"], e["range"]["start"]["line"] + 1))}
    alone = bool(one) and touched <= {3}
    check(alone, "a one-line range changes that line alone" + ("" if alone else ": " + json.dumps(one)))
    hlsl_uri = uri_for(unstyled / "unsaved_kernel.compute")
    client.notify("textDocument/didOpen",
                  {"textDocument": {"uri": hlsl_uri, "languageId": "hlsl", "version": 1, "text": hlsl.decode("utf-8")}})
    diags(client, hlsl_uri)
    edits = client.request("textDocument/formatting", {"textDocument": {"uri": hlsl_uri}, "options": {"tabSize": 4, "insertSpaces": True}})
    check(len(edits) == 1 and edits[0]["newText"] == hlsl_expected.decode("utf-8"),
          "LSP formatting lays out HLSL documents as well")
    glsl_uri = uri_for(unstyled / "unsaved.glsl")
    client.notify("textDocument/didOpen",
                  {"textDocument": {"uri": glsl_uri, "languageId": "glsl", "version": 1, "text": glsl.decode("utf-8")}})
    check(diags(client, glsl_uri) == [], "GLSL documents are never analyzed")
    edits = client.request("textDocument/formatting", {"textDocument": {"uri": glsl_uri}, "options": {"tabSize": 4, "insertSpaces": True}})
    check(len(edits) == 1 and edits[0]["newText"] == glsl_expected.decode("utf-8"),
          "LSP formatting lays out GLSL documents")
    where_gl = {"textDocument": {"uri": glsl_uri}, "position": {"line": 6, "character": 10}}
    check(labels(client.request("textDocument/completion", where_gl)) == set(), "no HLSL completion in a GLSL file")
    check(client.request("textDocument/hover", where_gl) is None, "no HLSL hover in a GLSL file")
    broken_uri = uri_for(FIXTURES / "unsaved_broken.shader")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": broken_uri, "languageId": "shaderlab", "version": 1, "text": 'Shader "X"\n{\n'}})
    client.diagnostics(broken_uri, 1)
    try:
        client.request("textDocument/formatting", {"textDocument": {"uri": broken_uri}, "options": {"tabSize": 4, "insertSpaces": True}})
        check(False, "LSP formatting reports unbalanced files as errors")
    except AssertionError as e:
        check("missing '}'" in str(e), "LSP formatting reports unbalanced files as errors")

    unstyled_dir.cleanup()

    print("syntax.shader")
    syntax_uri = open_doc(client, FIXTURES / "syntax.shader")
    d = diags(client, syntax_uri)
    check(has(d, 5, "'Name' is only valid inside a Pass"), "Name outside a Pass")
    check(has(d, 1, "Missing '}'"), "unclosed block")
    check(has(d, 10, "Unknown #pragma 'banana'", 2), "unknown pragma")
    check(has(d, 8, "Missing #pragma fragment", 2), "missing entry point pragma")

    print("kernel.compute")
    compute_uri = open_doc(client, FIXTURES / "kernel.compute")
    d = diags(client, compute_uri)
    if compiler:
        check(has(d, 9, "undeclared identifier 'notDeclared'", 1, compiler), f"compute kernels compiled with {compiler}")
    else:
        skip("compute kernels compiled")

    print("use_dxc.shader")
    use_dxc_uri = open_doc(client, FIXTURES / "use_dxc.shader")
    d = diags(client, use_dxc_uri)
    if has_dxc:
        check(has(d, 12, "undeclared identifier 'missingInDxc'", 1, "dxc"), "#pragma use_dxc compiles with DXC")
        check(len([x for x in d if x.get("source") in ("fxc", "dxc")]) == 1, "and with DXC alone")
    else:
        skip("#pragma use_dxc compiles with DXC", "no DXC")

    print("completion.shader")
    completion = FIXTURES / "completion.shader"
    comp_uri = uri_for(completion)
    # Completion positions sit after a trailing space; add them here since editors strip them from files.
    lines = completion.read_text(encoding="utf-8").split("\n")
    for index in (12, 15, 16):
        lines[index] = lines[index].rstrip() + " "
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": comp_uri, "languageId": "shaderlab", "version": 1, "text": "\n".join(lines)}
    })
    doc = {"uri": comp_uri}

    def complete(line, character):
        return labels(client.request("textDocument/completion", {"textDocument": doc, "position": {"line": line, "character": character}}))

    items = complete(11, 12)
    check({"Cull", "ZWrite", "Stencil", "Tags", "Name", "HLSLPROGRAM"} <= items, "Pass-level keywords and commands")
    check("LOD" not in items, "SubShader-only keywords excluded from Pass")
    check({"Back", "Front", "Off"} <= complete(12, 17), "Cull values")
    check({"_Tint", "_Mode"} <= complete(13, 19), "property names after '['")
    check({"vertex", "fragment", "multi_compile", "target"} <= complete(15, 20), "pragma names")
    check("vert" in complete(16, 27), "entry point functions after #pragma vertex")
    items = complete(18, 47)
    check({"_Tint", "vert", "saturate", "float4"} <= items, "HLSL identifiers, properties and intrinsics")
    items = complete(4, 8)
    check(len(items) == 0 or "Cull" not in items, "no commands inside Properties")

    print("hover / definition / symbols")
    hover = client.request("textDocument/hover", {"textDocument": {"uri": valid_uri}, "position": {"line": 25, "character": 10}})
    check(hover is not None and "depth buffer" in hover["contents"]["value"], "hover on ZWrite shows the reference text")
    hover = client.request("textDocument/hover", {"textDocument": {"uri": valid_uri}, "position": {"line": 37, "character": 22}})
    check(hover is not None and "**Equal**" in hover["contents"]["value"], "hover on a Stencil comparison value")
    hover = client.request("textDocument/hover", {"textDocument": {"uri": valid_uri}, "position": {"line": 43, "character": 23}})
    check(hover is not None and "vertex shader" in hover["contents"]["value"], "hover on #pragma vertex")
    intrinsic_uri = uri_for(FIXTURES / "unsaved_intrinsics.hlsl")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": intrinsic_uri, "languageId": "hlsl", "version": 1,
                                                            "text": "float f(float x) { return sqrt(x) + SQRT(x); }\n"}})
    hover = client.request("textDocument/hover", {"textDocument": {"uri": intrinsic_uri}, "position": {"line": 0, "character": 27}})
    check(hover is not None and "T sqrt(T x)" in hover["contents"]["value"] and "square root" in hover["contents"]["value"],
          "hover on an intrinsic shows its signature and description")
    hover = client.request("textDocument/hover", {"textDocument": {"uri": intrinsic_uri}, "position": {"line": 0, "character": 37}})
    check(hover is None, "intrinsics are matched case-sensitively")
    items = client.request("textDocument/completion", {"textDocument": {"uri": intrinsic_uri}, "position": {"line": 0, "character": 26}})
    items = items["items"] if isinstance(items, dict) else items
    lerp = next((item for item in items if item["label"] == "lerp"), None)
    check(lerp is not None and "detail" not in lerp and "labelDetails" not in lerp
          and "T lerp(T x, T y, T s)" in lerp.get("documentation", {}).get("value", ""),
          "intrinsic completions keep the list to the name and carry the signature and description as documentation")
    groupshared = next((item for item in items if item["label"] == "groupshared"), None)
    check(groupshared is not None and "thread group" in groupshared.get("documentation", {}).get("value", ""),
          "keyword completions carry their description as documentation")

    # One word of each kind, and a phrase its hover must contain.
    hlsl_source = "\n".join([
        "#define SCALE 2",
        "groupshared float cache[64];",
        "Texture2D _Tex; SamplerState sampler_Tex;",
        "struct V { float3 n : NORMAL; // object-space normal",
        "};",
        "// Scales the input.",
        "float3x3 Scale(half3 v) { return 0; }",
        "[numthreads(8, 8, 1)] void Kernel(uint3 id : SV_DispatchThreadID) {",
        "  float4 c = _Tex.SampleLevel(sampler_Tex, float2(0, 0), 0) + WaveActiveSum(1);",
        "  [unroll] for (int i = 0; i < 2; ++i) {}",
        "}",
        "float4 frag(float2 uv : TEXCOORD0) : SV_Target { return 0; }",
    ])
    hlsl_uri = uri_for(FIXTURES / "unsaved_hover.hlsl")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": hlsl_uri, "languageId": "hlsl", "version": 1, "text": hlsl_source}})
    hlsl_lines = hlsl_source.split("\n")

    def hover_text(uri, lines, needle, skip=0):
        line = next(i for i, text in enumerate(lines) if needle in text)
        position = {"line": line, "character": lines[line].index(needle) + skip + 1}
        result = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": position})
        return result["contents"]["value"] if result else ""

    for needle, expected, description in [
        ("define", "Defines a macro", "a # directive"),
        ("groupshared", "thread group", "an HLSL keyword"),
        ("half3", "A vector of 3 `half` components", "a vector type"),
        ("float3x3", "3 rows and 3 columns", "a matrix type"),
        ("Texture2D", "A 2D texture", "a resource type"),
        ("NORMAL", "vertex normal", "a semantic"),
        ("TEXCOORD0", "UV channel", "a numbered semantic"),
        ("SV_DispatchThreadID", "whole dispatch", "a system value"),
        ("numthreads", "thread group", "an attribute"),
        ("unroll", "Unrolls the loop", "a loop attribute"),
        ("SampleLevel", "mip level `lod`", "a texture method"),
        ("WaveActiveSum", "Shader Model 6.0", "a Shader Model 6 intrinsic"),
        ("Scale(", "Scales the input.", "the comment above a function"),
        ("n : NORMAL", "object-space normal", "the comment after a field"),
    ]:
        check(expected in hover_text(hlsl_uri, hlsl_lines, needle), f"hover describes {description}")

    shaderlab_source = "\n".join([
        'Shader "Legacy"',
        "{",
        '    Properties { _MainTex ("Texture", 2D) = "bump" {} }',
        "    SubShader",
        "    {",
        "        Pass",
        "        {",
        "            Lighting Off",
        "            Fog { Mode Off }",
        "        }",
        "    }",
        "}",
    ])
    legacy_uri = uri_for(FIXTURES / "unsaved_legacy.shader")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": legacy_uri, "languageId": "shaderlab", "version": 1, "text": shaderlab_source}})
    legacy_lines = shaderlab_source.split("\n")
    for needle, skip, expected, description in [
        ('"bump"', 1, "flat normal map", "a built-in texture default"),
        ("Lighting", 0, "per-vertex lighting", "a legacy command"),
        ("Mode", 0, "fog mode", "a command inside a legacy block"),
    ]:
        check(expected in hover_text(legacy_uri, legacy_lines, needle, skip), f"hover describes {description}")

    definition = client.request("textDocument/definition", {"textDocument": {"uri": valid_uri}, "position": {"line": 22, "character": 16}})
    check(definition is not None and definition["range"]["start"]["line"] == 8, "definition of [_Cull] goes to the property")
    definition = client.request("textDocument/definition", {"textDocument": {"uri": valid_uri}, "position": {"line": 43, "character": 29}})
    check(definition is not None and definition["range"]["start"]["line"] == 59, "definition of #pragma vertex target")
    definition = client.request("textDocument/definition", {"textDocument": {"uri": valid_uri}, "position": {"line": 67, "character": 24}})
    check(definition is not None and definition["range"]["start"]["line"] == 15, "definition into an HLSLINCLUDE block")

    symbols = client.request("textDocument/documentSymbol", {"textDocument": {"uri": valid_uri}})
    shader = symbols[0] if symbols else {}
    names = [child["name"] for child in shader.get("children", [])]
    check(shader.get("name") == "Tests/Valid" and names[:3] == ["Properties", "HLSLINCLUDE", "SubShader"], "outline structure")
    passes = shader["children"][2]["children"] if len(names) >= 3 else []
    check(any(p["name"] == 'Pass "ForwardLit"' for p in passes), "pass names in outline")

    print("members")
    members_source = "\n".join([
        "struct Light { float3 direction; half4 color; };",
        "struct Surface { float3 normal; Light light; };",
        "StructuredBuffer<Light> _Lights;",
        "Texture2D<float> _Mask; Texture2D _Albedo; SamplerState sampler_Albedo;",
        "TEXTURE2D(_BaseMap);",
        "RWByteAddressBuffer _Raw;",
        "float4x4 _Matrix;",
        "Surface GetSurface() { Surface s = (Surface)0; return s; }",
        "float4 frag(Surface surface, float2 uv : TEXCOORD0) : SV_Target",
        "{",
        "    float3 n = surface.normal, m = n;",
        "    Light first = _Lights[0];",
        "    float k = surface.light.color.x + _Lights[1].color.r + first.direction.y + m.z + GetSurface().light.color.a;",
        "    return _Albedo.Sample(sampler_Albedo, uv) + _BaseMap.Sample(sampler_Albedo, uv) + _Mask.Load(0).x + _Matrix._m00 + 1.5;",
        "}",
    ])
    members_uri = uri_for(FIXTURES / "unsaved_members.hlsl")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": members_uri, "languageId": "hlsl", "version": 1, "text": members_source}})
    members_lines = members_source.split("\n")

    def members_at(needle, occurrence=0):
        """Completion just after the '.' that ends `needle`."""
        line = next(i for i, text in enumerate(members_lines) if needle in text)
        index = -1
        for _ in range(occurrence + 1):
            index = members_lines[line].index(needle, index + 1)
        position = {"line": line, "character": index + len(needle)}
        return labels(client.request("textDocument/completion", {"textDocument": {"uri": members_uri}, "position": position}))

    check(members_at("surface.") == {"normal", "light"}, "a parameter's struct fields")
    check(members_at("surface.light.") == {"direction", "color"}, "a field's struct fields")
    check({"x", "y", "z", "w", "r", "g", "b", "a", "xyzw", "rgb"} <= members_at("surface.light.color."), "a vector's components")
    check("w" not in members_at("first.direction."), "only as many components as the vector has")
    check(members_at("_Lights[1].") == {"direction", "color"}, "an element of a StructuredBuffer of structs")
    check(members_at("first.") == {"direction", "color"}, "a local declared from an indexed buffer")
    check("z" in members_at("m.") and "w" not in members_at("m."), "the second name of a declaration with two")
    check(members_at("GetSurface().light.") == {"direction", "color"}, "through a function's return type")
    albedo = members_at("_Albedo.")
    check({"Sample", "SampleLevel", "Load", "Gather", "GetDimensions"} <= albedo and "Store" not in albedo, "a texture's methods")
    check("Sample" in members_at("_BaseMap."), "a texture declared with Unity's TEXTURE2D macro")
    check(members_at("_Mask.Load(0).") == {"x", "r"}, "what a texture's Load returns, from its <T>")
    check({"_m00", "_m33"} <= members_at("_Matrix."), "a matrix's elements")
    check(members_at("1.") == set(), "nothing after the '.' of a number")
    members_lines.append("void f() { _Raw. }")
    client.notify("textDocument/didChange", {"textDocument": {"uri": members_uri, "version": 2},
                                             "contentChanges": [{"text": "\n".join(members_lines)}]})
    check({"Store4", "Load2"} <= members_at("_Raw."), "a raw buffer's methods")

    def member_line(needle, skip):
        line = next(i for i, text in enumerate(members_lines) if needle in text)
        return {"textDocument": {"uri": members_uri}, "position": {"line": line, "character": members_lines[line].index(needle) + skip}}

    definition = client.request("textDocument/definition", member_line("first.direction", 7))
    check(definition is not None and definition["range"]["start"]["line"] == 0, "definition of a member goes to its struct's field")
    hover = client.request("textDocument/hover", member_line("surface.light.color", 14))
    check(hover is not None and "half4 color" in hover["contents"]["value"], "hover on a member shows its field")

    print("signature help")

    def signature_at(uri, lines, needle, skip):
        line = next(i for i, text in enumerate(lines) if needle in text)
        position = {"line": line, "character": lines[line].index(needle) + skip}
        return client.request("textDocument/signatureHelp", {"textDocument": {"uri": uri}, "position": position})

    sample = signature_at(members_uri, members_lines, "_Albedo.Sample(sampler_Albedo, uv)", len("_Albedo.Sample(sampler_Albedo, "))
    shown = sample["signatures"][sample["activeSignature"]] if sample else {}
    label = shown.get("label", "")
    params = [label[b:e] for b, e in (p["label"] for p in shown.get("parameters", []))]
    check("Sample(" in label and sample["activeParameter"] == 1 and params[:2] == ["SamplerState s", "location"],
          "a method's signature, on its second parameter: " + str(params[:3]))
    nested = signature_at(members_uri, members_lines, "GetSurface().light", len("GetSurface("))
    check(nested is not None and nested["signatures"][0]["label"].startswith("Surface GetSurface()") and nested["activeParameter"] == 0,
          "a function of the document")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": intrinsic_uri, "languageId": "hlsl", "version": 2,
                                                            "text": "float f(float x) { return lerp(x, sqrt(x), 0.5); }\n"}})
    inner = client.request("textDocument/signatureHelp", {"textDocument": {"uri": intrinsic_uri}, "position": {"line": 0, "character": 39}})
    outer = client.request("textDocument/signatureHelp", {"textDocument": {"uri": intrinsic_uri}, "position": {"line": 0, "character": 44}})
    check(inner is not None and inner["signatures"][0]["label"].startswith("T sqrt") and inner["activeParameter"] == 0, "an intrinsic, inside another call")
    check(outer is not None and outer["signatures"][0]["label"].startswith("T lerp") and outer["activeParameter"] == 2, "and the call around it, on its third parameter")

    print("references / highlights / rename")
    refs_source = "\n".join([
        'Shader "Tests/References"',
        "{",
        "    Properties",
        "    {",
        "        [HDR] _Color (\"Color\", Color) = (1, 1, 1, 1)",
        "        _MainTex (\"Texture\", 2D) = \"white\" {}",
        "        _Flag (\"Flag\", Float) = 0",
        "    }",
        "    HLSLINCLUDE",
        "    float4 _Color;",
        "    float Shared(float x) { return x * 2; }",
        "    ENDHLSL",
        "    SubShader",
        "    {",
        "        Cull [_Flag]",
        "        Pass",
        "        {",
        "            SetTexture [_MainTex] { combine texture }",
        "            HLSLPROGRAM",
        "            #pragma vertex vert",
        "            #pragma fragment frag",
        "            struct Attributes { float4 position : POSITION; float2 uv : TEXCOORD0; };",
        "            struct Varyings { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };",
        "            Texture2D _MainTex;",
        "            SamplerState sampler_MainTex;",
        "            float4 _MainTex_ST;",
        "            Varyings vert(Attributes input)",
        "            {",
        "                Varyings output;",
        "                output.position = input.position;",
        "                output.uv = input.uv * _MainTex_ST.xy + _MainTex_ST.zw;",
        "                return output;",
        "            }",
        "            float4 frag(Varyings input) : SV_Target",
        "            {",
        "                float4 color = _MainTex.Sample(sampler_MainTex, input.uv) * _Color;",
        "                return color * Shared(1); // _Color in a comment",
        "            }",
        "            ENDHLSL",
        "        }",
        "        Pass",
        "        {",
        "            HLSLPROGRAM",
        "            #pragma vertex vert",
        "            #pragma fragment frag",
        "            float4 vert() : SV_POSITION { float4 output = 0; return output * Shared(2); }",
        "            float4 frag() : SV_Target { return _Color; }",
        "            ENDHLSL",
        "        }",
        "    }",
        "}",
    ])
    refs_uri = uri_for(FIXTURES / "unsaved_references.shader")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": refs_uri, "languageId": "shaderlab", "version": 1, "text": refs_source}})
    refs_lines = refs_source.split("\n")

    def at(needle, skip=0, line_has=None):
        line = next(i for i, text in enumerate(refs_lines) if needle in text and (line_has is None or line_has in text))
        return {"textDocument": {"uri": refs_uri}, "position": {"line": line, "character": refs_lines[line].index(needle) + skip}}

    def found(result):
        return sorted((r["range"]["start"]["line"], r["range"]["start"]["character"]) for r in result or [])

    def lines_of(result):
        return sorted({r["range"]["start"]["line"] for r in result or []})

    def references(position, include=True):
        return client.request("textDocument/references", {**position, "context": {"includeDeclaration": include}})

    def refused(method, params):
        try:
            client.request(method, params)
        except AssertionError as error:
            return str(error)
        return ""

    color = references(at("_Color", line_has="(\"Color\""))
    check(lines_of(color) == [4, 9, 35, 46], "a property's references reach its variable in every pass, not a comment")
    check(len(references(at("_Color", line_has="(\"Color\""), include=False)) == 3, "and leave its declaration out when asked")
    check(lines_of(references(at("_MainTex", 1, "SetTexture"))) == [5, 17, 23, 35],
          "from a [_MainTex] the parser skips, a texture's are found by its exact name")
    check(lines_of(references(at("vertex vert", 7))) == [19, 26], "an entry point stays within its pass")
    check(lines_of(references(at("Shared", line_has="Shared(2)"))) == [10, 36, 45], "an HLSLINCLUDE function is seen from both passes")
    output = references(at("output", line_has="Varyings output"))
    check(lines_of(output) == [28, 29, 30, 31] and len(output) == 4, "a local is found only in the function it is used in")
    check(lines_of(references(at("uv", line_has="input.uv *"))) == [21, 22, 30, 35], "a member is found by name, in every struct")

    scoped_source = "\n".join([
        "float shade;",
        "float f(float x)",
        "{",
        "    float shade = x;",
        "    for (int i = 0; i < 2; ++i) { float t = i; shade += t; }",
        "    for (int i = 0; i < 3; ++i) shade += i;",
        "    { float t = 2, u = t; shade *= u; }",
        "    return shade;",
        "}",
        "float g() { return shade; }",
    ])
    scoped_uri = uri_for(FIXTURES / "unsaved_scopes.hlsl")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": scoped_uri, "languageId": "hlsl", "version": 1, "text": scoped_source}})
    scoped_lines = scoped_source.split("\n")

    def scoped(line, needle, occurrence=0):
        index = -1
        for _ in range(occurrence + 1):
            index = scoped_lines[line].index(needle, index + 1)
        return {"textDocument": {"uri": scoped_uri}, "position": {"line": line, "character": index}}

    def spots(result):
        return sorted((r["range"]["start"]["line"], r["range"]["start"]["character"]) for r in result or [])

    first_i = spots(references(scoped(4, "i ")))
    check(len(first_i) == 4 and {line for line, _ in first_i} == {4}, "a for loop's variable is its own, not the next loop's: " + str(first_i))
    check({line for line, _ in spots(references(scoped(5, "i ")))} == {5}, "and the next loop's is its own")
    check(spots(references(scoped(4, "t ="))) == [(4, 40), (4, 56)], "a block's local is not its sibling block's")
    check(lines_of(references(scoped(6, "u ="))) == [6], "the second name of a declaration is a local too")
    check(lines_of(references(scoped(3, "shade"))) == [3, 4, 5, 6, 7], "a local that shadows a global has uses of its own")
    check(lines_of(references(scoped(0, "shade"))) == [0, 9], "and the global keeps the rest")
    global_edits = client.request("textDocument/rename", {**scoped(9, "shade"), "newName": "tone"})["changes"][scoped_uri]
    check(sorted(e["range"]["start"]["line"] for e in global_edits) == [0, 9], "renaming the global leaves the local that shadows it alone")

    highlights = client.request("textDocument/documentHighlight", at("_Flag", 1, "Cull"))
    check(sorted((h["range"]["start"]["line"], h["kind"]) for h in highlights or []) == [(6, 3), (14, 2)],
          "highlights mark the property's declaration as written and its uses as read")
    check(client.request("textDocument/documentHighlight", at("float4", line_has="_Color;")) is None, "a type has no references")

    prepared = client.request("textDocument/prepareRename", at("_MainTex", 1, "Texture2D"))
    check(prepared is not None and prepared.get("placeholder") == "_MainTex", "prepareRename offers the name")
    check("Struct members" in refused("textDocument/prepareRename", at("uv", line_has="input.uv *")), "a member can't be renamed")

    def rename(position, new_name):
        result = client.request("textDocument/rename", {**position, "newName": new_name})
        return result["changes"][refs_uri]

    def renamed(position, new_name):
        source = refs_lines[:]
        for edit in sorted(rename(position, new_name), key=lambda e: (-e["range"]["start"]["line"], -e["range"]["start"]["character"])):
            start, end = edit["range"]["start"], edit["range"]["end"]
            assert start["line"] == end["line"]
            text = source[start["line"]]
            source[start["line"]] = text[:start["character"]] + edit["newText"] + text[end["character"]:]
        return source

    texture = renamed(at("_MainTex", line_has="(\"Texture\""), "_BaseMap")
    check(not any("_MainTex" in line for line in texture), "renaming a texture leaves no _MainTex behind")
    check("SamplerState sampler_BaseMap;" in texture[24] and "_BaseMap_ST.zw" in texture[30] and "SetTexture [_BaseMap]" in texture[17],
          "and takes its sampler, scale/offset and [references] with it")
    local = renamed(at("output", line_has="output.position"), "result")
    check(local[31].strip() == "return result;" and "float4 output = 0" in local[45], "renaming a local leaves the other pass alone")
    check("return color * Twice(1);" in renamed(at("Shared", line_has="return x"), "Twice")[36], "renaming an HLSLINCLUDE function")
    for new_name, fragment, description in [
        ("_Flag", "already in use", "a name already in use"),
        ("float3", "keyword, type or intrinsic", "a type name"),
        ("2fast", "not a valid identifier", "an invalid identifier"),
    ]:
        check(fragment in refused("textDocument/rename", {**at("_Color", line_has="(\"Color\""), "newName": new_name}), f"rename refuses {description}")

    print("semantic tokens")
    legend = caps.get("semanticTokensProvider", {}).get("legend", {})
    check(bool(legend.get("tokenTypes")), "semantic tokens are offered by default")
    data = client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": refs_uri}})["data"]
    tokens = {}
    line = character = 0
    for i in range(0, len(data), 5):
        delta_line, delta_character, length, kind, modifiers = data[i:i + 5]
        line += delta_line
        character = character + delta_character if delta_line == 0 else delta_character
        names = {name for bit, name in enumerate(legend["tokenModifiers"]) if modifiers & (1 << bit)}
        tokens[(line, character)] = (length, legend["tokenTypes"][kind], names)

    def token(needle, skip=0, line_has=None):
        position = at(needle, skip, line_has)["position"]
        return tokens.get((position["line"], position["character"]), (0, None, set()))

    for case in [
        ("Shader", None, "keyword", set(), "a ShaderLab keyword"),
        ("_Color", "(\"Color\"", "property", {"declaration"}, "a material property's declaration"),
        ("Color)", None, "type", set(), "a property type"),
        ("HDR", None, "decorator", set(), "a property attribute"),
        ("Cull", None, "keyword", set(), "a render state command"),
        ("_Flag", "Cull", "property", set(), "a [property] reference"),
        ("HLSLPROGRAM", None, "keyword", set(), "a code block keyword"),
        ("#pragma", "vertex", "keyword", set(), "a directive"),
        ("vertex vert", "#pragma", "function", set(), "an entry point in a pragma", 7),
        ("struct", "Attributes {", "keyword", set(), "an HLSL keyword"),
        ("Attributes", "struct", "struct", {"declaration"}, "a struct's declaration"),
        ("float4", "Attributes {", "type", {"defaultLibrary"}, "a built-in type"),
        ("POSITION", "Attributes {", "decorator", set(), "a semantic"),
        ("position", "Attributes {", "property", {"declaration"}, "a field's declaration"),
        ("vert", "Varyings vert", "function", {"declaration"}, "a function's declaration"),
        ("input", "Varyings vert", "parameter", {"declaration"}, "a parameter's declaration"),
        ("input", "output.position = input", "parameter", set(), "a parameter"),
        ("output", "Varyings output", "variable", {"declaration"}, "a local's declaration"),
        ("position", "output.position = input", "property", set(), "a member"),
        ("Sample", "_MainTex.Sample", "method", set(), "a method"),
        ("Shared", "Shared(1)", "function", set(), "a function from HLSLINCLUDE"),
        ("// _Color", None, "comment", set(), "a comment"),
        ("0", "Shared(2)", "number", set(), "a number"),
    ]:
        needle, line_has, kind, modifiers, description, *skip = case
        length, kind_found, modifiers_found = token(needle, skip[0] if skip else 0, line_has)
        check(kind_found == kind and modifiers <= modifiers_found, f"semantic token for {description}"
              + ("" if kind_found == kind else f": {kind_found}"))
    quiet = Client(exe)
    quiet_init = quiet.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}, "initializationOptions": {"semanticTokens": False}})
    check("semanticTokensProvider" not in quiet_init["capabilities"], "semanticTokens = false leaves them out, for an editor with a grammar")
    quiet.request("shutdown", None)
    quiet.notify("exit", None)
    quiet.proc.wait(timeout=10)

    print("workspace references / rename")
    workspace_dir = tempfile.TemporaryDirectory()
    top = pathlib.Path(workspace_dir.name).resolve()
    project = top / "Project"
    files = {
        "Shaders/Common.hlsl": '#include "../../External/Ext.hlsl"\nfloat4 _Tint;\nfloat3 Brighten(float3 c) { return c * _Tint.rgb; }\n',
        "Shaders/A.shader": "\n".join([
            'Shader "A"',
            "{",
            '    Properties { _Tint ("Tint", Color) = (1, 1, 1, 1) }',
            "    SubShader { Pass {",
            "        HLSLPROGRAM",
            "        #pragma vertex vert",
            "        #pragma fragment frag",
            '        #include "Common.hlsl"',
            "        float4 vert(float4 p : POSITION) : SV_POSITION { return p; }",
            "        float4 frag() : SV_Target { return float4(Brighten(_Tint.rgb) + Outside(1), 1); }",
            "        ENDHLSL",
            "    } }",
            "}",
        ]),
        "Shaders/B.compute": '#pragma kernel K\n#include "Common.hlsl"\nRWTexture2D<float4> _Out;\n[numthreads(1, 1, 1)] void K() { _Out[uint2(0, 0)] = float4(Brighten(1), 1); }\n',
        "Shaders/C.shader": 'Shader "C" { SubShader { Pass { HLSLPROGRAM\nfloat3 Brighten(float3 c) { return c; }\nENDHLSL } } }\n',
        "Shaders/E.shader": 'Shader "E" { Properties { _Tint ("Tint", Color) = (1, 1, 1, 1) } SubShader { Pass { HLSLPROGRAM\n#include "Common.hlsl"\nfloat4 f() { return _Tint; }\nENDHLSL } } }\n',
        "Library/D.hlsl": '#include "../Shaders/Common.hlsl"\nfloat3 g() { return Brighten(1); }\n',
    }
    for name, text in files.items():
        (project / name).parent.mkdir(parents=True, exist_ok=True)
        (project / name).write_text(text, encoding="utf-8", newline="\n")
    (top / "External").mkdir()
    (top / "External" / "Ext.hlsl").write_text("float Outside(float x) { return x; }\n", encoding="utf-8", newline="\n")
    client.notify("workspace/didChangeWorkspaceFolders", {"event": {"added": [{"uri": uri_for(project), "name": "Project"}], "removed": []}})

    def path_of(uri):
        from urllib.parse import unquote, urlparse
        path = unquote(urlparse(uri).path)
        return str(pathlib.Path(path.lstrip("/") if sys.platform == "win32" else path).resolve()).lower()

    def where(name):
        return str((project / name).resolve()).lower()

    a_lines = files["Shaders/A.shader"].split("\n")
    a_uri = open_doc(client, project / "Shaders/A.shader")

    def in_a(needle, line_has=None):
        line = next(i for i, text in enumerate(a_lines) if needle in text and (line_has is None or line_has in text))
        return {"textDocument": {"uri": a_uri}, "position": {"line": line, "character": a_lines[line].index(needle) + 1}}

    brighten = references(in_a("Brighten"))
    reached = {path_of(r["uri"]) for r in brighten or []}
    expected_files = {where("Shaders/Common.hlsl"), where("Shaders/A.shader"), where("Shaders/B.compute")}
    check(reached == expected_files, "references reach the include file and every file including it, not a namesake nor Library"
          + ("" if reached == expected_files else ": " + str(sorted(reached))))
    check(len(brighten) == 3 and len(references(in_a("Brighten"), include=False)) == 2, "and know which is the declaration")

    renamed_files = client.request("textDocument/rename", {**in_a("Brighten"), "newName": "Lighten"})["changes"]
    check({path_of(u) for u in renamed_files} == reached and all(e["newText"] == "Lighten" for edits in renamed_files.values() for e in edits),
          "a function from an include file in the workspace is renamed in all of them")
    tint = client.request("textDocument/rename", {**in_a("_Tint", "Properties"), "newName": "_Color"})["changes"]
    by_file = {path_of(u): edits for u, edits in tint.items()}
    counts = {path: len(edits) for path, edits in by_file.items()}
    expected_counts = {where("Shaders/Common.hlsl"): 2, where("Shaders/A.shader"): 2, where("Shaders/E.shader"): 2}
    check(counts == expected_counts, "a property whose variable an include declares is renamed with it, in every shader binding it"
          + ("" if counts == expected_counts else ": " + str(counts)))
    check("outside the workspace" in refused("textDocument/prepareRename", in_a("Outside")), "a symbol declared outside the workspace can't be renamed")
    check(lines_of(references(in_a("Outside"))) == [0, 9], "but its references are found, its declaration among them")
    check("already in use" in refused("textDocument/rename", {**in_a("Brighten"), "newName": "_Out"}), "a new name in use in another file is refused")

    symbols = client.request("workspace/symbol", {"query": "brtn"})
    found = sorted((s["name"], pathlib.Path(path_of(s["location"]["uri"])).name) for s in symbols)
    check(found == [("Brighten", "c.shader"), ("Brighten", "common.hlsl")], "workspace symbols match a name's letters in order, open or not: " + str(found))
    shaders = {s["name"] for s in client.request("workspace/symbol", {"query": ""}) if s["kind"] == 5}
    check({"A", "C", "E"} <= shaders, "and name the shaders")
    help_line = next(i for i, text in enumerate(a_lines) if "Brighten(" in text)
    included = client.request("textDocument/signatureHelp", {"textDocument": {"uri": a_uri}, "position": {"line": help_line, "character": a_lines[help_line].index("Brighten(") + 9}})
    check(included is not None and "float3 Brighten(float3 c)" in included["signatures"][0]["label"], "signature help for a function from an include file")
    common_uri = open_doc(client, project / "Shaders/Common.hlsl")
    from_common = client.request("textDocument/rename", {"textDocument": {"uri": common_uri}, "position": {"line": 2, "character": 9}, "newName": "Lighten"})["changes"]
    check({path_of(u) for u in from_common} == reached, "renaming in an include file reaches the files including it")
    check(common_uri in from_common and a_uri in from_common, "open documents are addressed by the URIs the client opened them with")
    print("index")
    # An include file uses what a sibling declares: G.shader includes Input.hlsl, then Pass.hlsl, which uses its
    # declarations without including it.
    (project / "Shaders/Input.hlsl").write_text("float4 _Gloss;\nstruct Surf { float3 albedo; };\n", encoding="utf-8", newline="\n")
    pass_text = "float4 Shade(Surf s) { return _Gloss * s.albedo.x; }\n"
    (project / "Shaders/Pass.hlsl").write_text(pass_text, encoding="utf-8", newline="\n")
    (project / "Shaders/G.shader").write_text('Shader "G" { SubShader { Pass { HLSLPROGRAM\n#include "Input.hlsl"\n#include "Pass.hlsl"\nENDHLSL } } }\n',
                                              encoding="utf-8", newline="\n")
    for name in ("Input.hlsl", "Pass.hlsl", "G.shader"):
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri_for(project / "Shaders" / name), "type": 1}]})
    client.request("workspace/symbol", {"query": "Shade"})  # waits for the index to take the changes in
    pass_uri = open_doc(client, project / "Shaders/Pass.hlsl")

    def in_pass(needle, skip=0):
        return {"textDocument": {"uri": pass_uri}, "position": {"line": 0, "character": pass_text.index(needle) + skip}}

    hover = client.request("textDocument/hover", in_pass("_Gloss"))
    check(hover is not None and "float4 _Gloss" in hover["contents"]["value"], "an include file sees what its includer declares before it")
    definition = client.request("textDocument/definition", in_pass("Surf"))
    check(definition is not None and path_of(definition["uri"]) == where("Shaders/Input.hlsl"), "and goes to it")
    check(labels(client.request("textDocument/completion", in_pass("s.albedo", 2))) == {"albedo"}, "and completes its members")
    gloss = client.request("textDocument/rename", {**in_pass("_Gloss"), "newName": "_Shine"})["changes"]
    check({path_of(u) for u in gloss} == {where("Shaders/Input.hlsl"), where("Shaders/Pass.hlsl")},
          "a sibling's declaration is renamed with its uses: " + str(sorted(pathlib.Path(path_of(u)).name for u in gloss)))
    client.notify("textDocument/didClose", {"textDocument": {"uri": pass_uri}})

    # A file the index learns of, and one it learns is gone.
    (project / "Shaders/H.shader").write_text('Shader "H" { SubShader { Pass { HLSLPROGRAM\n#include "Common.hlsl"\nfloat3 h() { return Brighten(1); }\nENDHLSL } } }\n',
                                              encoding="utf-8", newline="\n")
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri_for(project / "Shaders/H.shader"), "type": 1}]})
    check(where("Shaders/H.shader") in {path_of(r["uri"]) for r in references(in_a("Brighten"))}, "a new file is found once it is reported")
    (project / "Shaders/H.shader").unlink()
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri_for(project / "Shaders/H.shader"), "type": 3}]})
    check(where("Shaders/H.shader") not in {path_of(r["uri"]) for r in references(in_a("Brighten"))}, "and not once it is reported deleted")

    # An include file's conditions, worked out in the programs that include it.
    cond_text = "\n".join([
        "#ifdef _FEATURE",
        "float featureOnly;",
        "#endif",
        "#if LOCAL_ON",
        "float localOn;",
        "#else",
        "float localOff;",
        "#endif",
        "#ifdef NOTHING_DEFINES_THIS",
        "float never;",
        "#endif",
        "",
    ])
    (project / "Shaders/Cond.hlsl").write_text(cond_text, encoding="utf-8", newline="\n")
    (project / "Shaders/K.shader").write_text('Shader "K" { SubShader { Pass { HLSLPROGRAM\n#pragma multi_compile _ _FEATURE\n'
                                              '#define LOCAL_ON 1\n#include "Cond.hlsl"\nENDHLSL } } }\n', encoding="utf-8", newline="\n")
    for name in ("Cond.hlsl", "K.shader"):
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri_for(project / "Shaders" / name), "type": 1}]})
    client.request("workspace/symbol", {"query": "featureOnly"})  # waits for the index
    cond_uri = open_doc(client, project / "Shaders/Cond.hlsl")

    def cond_faded(published):
        return sorted(x["range"]["start"]["line"] for x in published if x.get("tags") == [1])

    check(cond_faded(diags(client, cond_uri)) == [1, 6, 9],
          "an include file's #if is worked out with its includer's variant and the #defines before its #include")
    # A second includer that defines _FEATURE: what either compiles is not faded.
    (project / "Shaders/L.shader").write_text('Shader "L" { SubShader { Pass { HLSLPROGRAM\n#define _FEATURE 1\n'
                                              '#define LOCAL_ON 1\n#include "Cond.hlsl"\nENDHLSL } } }\n', encoding="utf-8", newline="\n")
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri_for(project / "Shaders/L.shader"), "type": 1}]})
    check(cond_faded(diags(client, cond_uri)) == [6, 9], "and faded only where no includer compiles it, again when an includer appears")
    client.notify("textDocument/didClose", {"textDocument": {"uri": cond_uri}})

    for uri in (a_uri, common_uri):
        client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    client.notify("workspace/didChangeWorkspaceFolders", {"event": {"added": [], "removed": [{"uri": uri_for(project), "name": "Project"}]}})

    print("saved index")
    # A server that saved the index on exit; the next one loads it and indexes only what changed meanwhile.
    cache_dir = tempfile.TemporaryDirectory()
    saved_project = tempfile.TemporaryDirectory()
    saved_root = pathlib.Path(saved_project.name).resolve()
    (saved_root / "One.hlsl").write_text("float First(float x) { return x; }\n", encoding="utf-8", newline="\n")
    (saved_root / "Two.hlsl").write_text("float Second(float x) { return x; }\n", encoding="utf-8", newline="\n")
    options = {"diagnostics": {"compiler": "none"}, "indexCache": cache_dir.name}

    def session():
        server = Client(exe)
        server.request("initialize", {"processId": None, "rootUri": uri_for(saved_root), "capabilities": {}, "initializationOptions": options})
        server.notify("initialized", {})
        return server

    def finish(server):
        server.request("shutdown", None)
        server.notify("exit", None)
        server.proc.wait(timeout=10)

    def logged(server):
        return " ".join(m["params"]["message"] for m in server.pending if m.get("method") == "window/logMessage")

    first = session()
    check(len(first.request("workspace/symbol", {"query": "First"})) == 1, "a fresh index answers")
    finish(first)
    check(any(pathlib.Path(cache_dir.name).glob("*.idx")), "and is saved when the server exits")
    time.sleep(0.05)
    (saved_root / "Two.hlsl").write_text("float Third(float x) { return x; }\n", encoding="utf-8", newline="\n")
    second = session()
    found = {s["name"] for s in second.request("workspace/symbol", {"query": ""})}
    check("files loaded from" in logged(second), "the next server loads it")
    check({"First", "Third"} <= found and "Second" not in found, "and indexes again only the file that changed meanwhile: " + str(sorted(found)))
    finish(second)
    cache_dir.cleanup()
    saved_project.cleanup()

    print("edits")
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": valid_uri, "version": 2},
        "contentChanges": [{"text": (FIXTURES / "valid.shader").read_text(encoding="utf-8").replace("ZWrite Off", "ZWrite Maybe")}],
    })
    d = client.diagnostics(valid_uri, 1)
    check(has(d, 26, "Invalid value 'Maybe' for ZWrite", 1), "diagnostics follow didChange")

    client.request("shutdown", None)
    client.notify("exit", None)
    code = client.proc.wait(timeout=10)
    check(code == 0, "clean shutdown")

    print()
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
