"""Robustness check: feeds truncated and mangled shaders to shaderlab-ls and queries features at random positions.

Usage: python tests/fuzz.py path/to/shaderlab-ls.exe [iterations] [extra files...]
Set FUZZ_COMPILE=1 to compile HLSL (FXC or DXC) concurrently. Fails if the server crashes, hangs, or returns an error.
"""

import os
import pathlib
import random
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from run_tests import Client  # noqa: E402

FIXTURES = pathlib.Path(__file__).parent / "fixtures"


def mutate(text, rng):
    choice = rng.randrange(5)
    if not text:
        return text
    a = rng.randrange(len(text))
    b = min(len(text), a + rng.randrange(1, 40))
    if choice == 0:
        return text[:a]  # truncation, as while typing
    if choice == 1:
        return text[:a] + text[b:]  # deletion
    if choice == 2:
        return text[:a] + rng.choice(['{', '}', '"', '[', ']', '(', ')', '#', '\n', '=', ',', '/*', '//', 'ENDHLSL', 'HLSLPROGRAM', 'é', '\\']) + text[a:]
    if choice == 3:
        return text[:a] + text[b:a + 2 * (b - a)] + text[a:]  # duplication
    return "".join(c for c in text if rng.random() > 0.02)


def main():
    exe = sys.argv[1]
    iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    files = sorted(FIXTURES.glob("*.*")) + [pathlib.Path(p) for p in sys.argv[3:]]
    rng = random.Random(1234)
    client = Client(exe)
    client.request("initialize", {"processId": None, "rootUri": None,
                                  "capabilities": {"general": {"positionEncodings": ["utf-16"]}},
                                  "initializationOptions": {"diagnostics": {"compiler": "auto" if os.environ.get("FUZZ_COMPILE") == "1" else "none", "delay": 0}}})
    client.notify("initialized", {})
    start = time.time()
    for path in files:
        base = path.read_text(encoding="utf-8")
        uri = path.resolve().as_uri()
        client.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "shaderlab", "version": 0, "text": base}})
        text = base
        for version in range(1, iterations + 1):
            text = mutate(text if rng.random() < 0.7 else base, rng)
            client.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]})
            lines = text.split("\n")
            line = rng.randrange(len(lines))
            position = {"line": line, "character": rng.randrange(len(lines[line]) + 2)}
            params = {"textDocument": {"uri": uri}, "position": position}
            client.request("textDocument/completion", params, timeout=10)
            client.request("textDocument/hover", params, timeout=10)
            client.request("textDocument/definition", params, timeout=10)
            client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}}, timeout=10)
            try:
                client.request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}}, timeout=10)
            except AssertionError as error:
                if "can't format" not in str(error):  # refusing unbalanced input is expected
                    raise
            client.pending.clear()
            if client.proc.poll() is not None:
                print(f"server exited with {client.proc.returncode} on {path.name} v{version}")
                return 1
        client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        print(f"{path.name}: {iterations} mutations ok")
    client.request("shutdown", None)
    client.notify("exit", None)
    code = client.proc.wait(timeout=10)
    print(f"done in {time.time() - start:.1f}s, exit {code}")
    return code


if __name__ == "__main__":
    sys.exit(main())
