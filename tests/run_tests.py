"""End-to-end tests: drives shaderlab-ls over stdio like an editor would.

Usage: python tests/run_tests.py path/to/shaderlab-ls.exe
"""

import json
import pathlib
import queue
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
        "initializationOptions": {"diagnostics": {"delay": 0}},
    })
    client.notify("initialized", {})

    print("initialize")
    caps = init["capabilities"]
    check(caps["positionEncoding"] == "utf-16", "negotiates utf-16")
    check(caps["hoverProvider"] and caps["definitionProvider"] and caps["documentSymbolProvider"], "advertises features")

    print("errors.shader")
    errors = FIXTURES / "errors.shader"
    uri = open_doc(client, errors)
    d = client.diagnostics(uri, 2)
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
    check(has(d, 35, "undeclared identifier 'undefinedThing'", 1, "fxc"), "FXC error mapped into the Pass")
    fxc = [x for x in d if "undefinedThing" in x["message"]]
    # "            float4 frag() : SV_Target { /* é */ return _Color * " is 64 UTF-16 units.
    check(bool(fxc) and fxc[0]["range"]["start"]["character"] == 64, "FXC byte columns converted to UTF-16")
    check(not any(x["range"]["start"]["line"] == 14 for x in d), "valid Blend with a property reference")

    print("valid.shader")
    valid_uri = open_doc(client, FIXTURES / "valid.shader")
    d = client.diagnostics(valid_uri, 2)
    check(d == [], "no diagnostics for a valid shader: " + "; ".join(x["message"] for x in d))

    print("unity_compat.shader")
    compat_uri = open_doc(client, FIXTURES / "unity_compat.shader")
    d = client.diagnostics(compat_uri, 2)
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
        d = client.diagnostics(bom_uri, 2)
        check(d == [], "included files with a UTF-8 BOM compile: " + "; ".join(x["message"] for x in d))
        client.notify("textDocument/didClose", {"textDocument": {"uri": bom_uri}})
        client.diagnostics(bom_uri, 1)

    print("formatting")
    # Fixtures may be checked out with CRLF; the formatter keeps whatever it is given, so test with LF explicitly.
    source = (FIXTURES / "format_input.shader").read_bytes().replace(b"\r\n", b"\n")
    expected = (FIXTURES / "format_expected.shader").read_bytes().replace(b"\r\n", b"\n")
    run = subprocess.run([exe, "--format", "-", "--indent", "4"], input=source, capture_output=True)
    check(run.returncode == 0 and run.stdout == expected, "output matches format_expected.shader")
    again = subprocess.run([exe, "--format", "-", "--indent", "4"], input=expected, capture_output=True)
    check(again.stdout == expected, "formatting is idempotent")
    crlf = subprocess.run([exe, "--format", "-", "--indent", "4"], input=source.replace(b"\n", b"\r\n"), capture_output=True)
    check(crlf.stdout == expected.replace(b"\n", b"\r\n"), "CRLF line endings are preserved")
    tabs = subprocess.run([exe, "--format", "-", "--indent", "4", "--tabs"], input=expected, capture_output=True).stdout
    check(b"\n\tProperties" in tabs and b"\n\t\t\tHLSLPROGRAM" in tabs, "--tabs indents with tabs")
    broken = subprocess.run([exe, "--format", str(FIXTURES / "syntax.shader")], capture_output=True)
    check(broken.returncode == 1 and broken.stdout == b"" and b"missing '}'" in broken.stderr, "unbalanced files are refused")

    format_uri = uri_for(FIXTURES / "format_input.shader")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": format_uri, "languageId": "shaderlab", "version": 1, "text": source.decode("utf-8")}})
    client.diagnostics(format_uri, 2)
    edits = client.request("textDocument/formatting", {"textDocument": {"uri": format_uri}, "options": {"tabSize": 4, "insertSpaces": True}})
    lines = source.decode("utf-8").split("\n")
    check(len(edits) == 1 and edits[0]["newText"] == expected.decode("utf-8")
          and edits[0]["range"]["end"]["line"] == len(lines) - 1, "LSP formatting returns the whole formatted document")
    broken_uri = uri_for(FIXTURES / "unsaved_broken.shader")
    client.notify("textDocument/didOpen", {"textDocument": {"uri": broken_uri, "languageId": "shaderlab", "version": 1, "text": 'Shader "X"\n{\n'}})
    client.diagnostics(broken_uri, 1)
    try:
        client.request("textDocument/formatting", {"textDocument": {"uri": broken_uri}, "options": {"tabSize": 4, "insertSpaces": True}})
        check(False, "LSP formatting reports unbalanced files as errors")
    except AssertionError as e:
        check("missing '}'" in str(e), "LSP formatting reports unbalanced files as errors")

    print("syntax.shader")
    syntax_uri = open_doc(client, FIXTURES / "syntax.shader")
    d = client.diagnostics(syntax_uri, 2)
    check(has(d, 5, "'Name' is only valid inside a Pass"), "Name outside a Pass")
    check(has(d, 1, "Missing '}'"), "unclosed block")
    check(has(d, 10, "Unknown #pragma 'banana'", 2), "unknown pragma")
    check(has(d, 8, "Missing #pragma fragment", 2), "missing entry point pragma")

    print("kernel.compute")
    compute_uri = open_doc(client, FIXTURES / "kernel.compute")
    d = client.diagnostics(compute_uri, 2)
    check(has(d, 9, "undeclared identifier 'notDeclared'", 1, "fxc"), "compute kernels compiled with cs_5_0")

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
