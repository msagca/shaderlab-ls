"""A stand-in GLSL language server for tests/run_plugin_tests.py: enough of LSP to show what a GLSL server attached
to a .shader is sent and what comes back from it.

It records every document it is sent to the file named on its command line, as one JSON object per line, and
answers the way a real server would, with the one liberty that makes the answers checkable: every line with `bad`
in it gets a diagnostic, and so does the first line of every document, which in a .shader is outside any GLSL block
and must never reach the buffer. Hover says where it was asked, and definition points back into the document at
the same place. It advertises incremental sync and formatting, both of which the plugin is expected to take away.

Usage: python tests/fake_glsl_server.py path/to/record
"""

import json
import sys

record = open(sys.argv[1], "a", encoding="utf-8")
stdin, stdout = sys.stdin.buffer, sys.stdout.buffer


def note(entry):
    record.write(json.dumps(entry) + "\n")
    record.flush()


def read():
    length = None
    while True:
        line = stdin.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        name, _, number = line.partition(b":")
        if name.lower() == b"content-length":
            length = int(number)
    return json.loads(stdin.read(length))


def send(message):
    body = json.dumps(dict(message, jsonrpc="2.0")).encode()
    stdout.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    stdout.flush()


def publish(uri, text):
    lines = text.split("\n")
    marked = [0] + [number for number, line in enumerate(lines) if "bad" in line]
    diagnostics = [
        {"range": {"start": {"line": n, "character": 0}, "end": {"line": n, "character": 1}}, "message": f"line {n}"}
        for n in marked
    ]
    send({"method": "textDocument/publishDiagnostics", "params": {"uri": uri, "diagnostics": diagnostics}})


while True:
    message = read()
    if message is None:
        break
    method, params = message.get("method"), message.get("params") or {}
    if method == "initialize":
        capabilities = {"textDocumentSync": 2, "hoverProvider": True, "definitionProvider": True,
                        "documentFormattingProvider": True}
        send({"id": message["id"], "result": {"capabilities": capabilities}})
    elif method == "textDocument/didOpen":
        document = params["textDocument"]
        note({"event": "open", "uri": document["uri"], "languageId": document["languageId"], "text": document["text"]})
        publish(document["uri"], document["text"])
    elif method == "textDocument/didChange":
        uri = params["textDocument"]["uri"]
        change = params["contentChanges"][-1]
        note({"event": "change", "uri": uri, "full": "range" not in change, "text": change["text"]})
        publish(uri, change["text"])
    elif method == "textDocument/hover":
        position = params["position"]
        note({"event": "hover", "position": position})
        send({"id": message["id"], "result": {"contents": f"hover {position['line']}:{position['character']}"}})
    elif method == "textDocument/definition":
        position = params["position"]
        location = {"uri": params["textDocument"]["uri"], "range": {"start": position, "end": position}}
        send({"id": message["id"], "result": [location]})
    elif method == "shutdown":
        send({"id": message["id"], "result": None})
    elif method == "exit":
        break
    elif "id" in message:
        send({"id": message["id"], "result": None})
