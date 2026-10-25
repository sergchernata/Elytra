#!/usr/bin/env python3
# A native messaging host for the tests. It echoes what it gets, and some messages ask it for more:
#   {"cmd": "info"}                        -> {"argv": [...], "cwd": "..."}
#   {"cmd": "note_exit", "path": "..."}    -> {"noted": true}; writes "exited" to the path once its stdin closes
#   {"cmd": "burst", "count": n}           -> n messages {"burst": i}, in one write
#   {"cmd": "garbage"}                     -> a frame that isn't JSON
#   {"cmd": "oversized"}                   -> a frame over Chrome's 1 MiB limit
#   {"cmd": "exit"}                        -> exits at once
#   anything else                          -> {"echo": message}
import json
import os
import struct
import sys

stdin = sys.stdin.buffer
stdout = sys.stdout.buffer
exit_note = None


def frame(payload):
    return struct.pack("=I", len(payload)) + payload


def send(message):
    stdout.write(frame(json.dumps(message).encode("utf-8")))
    stdout.flush()


def read_exactly(count):
    data = b""
    while len(data) < count:
        chunk = stdin.read(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def serve():
    global exit_note
    while True:
        header = read_exactly(4)
        if header is None:
            break
        body = read_exactly(struct.unpack("=I", header)[0])
        if body is None:
            break
        message = json.loads(body)
        command = message.get("cmd") if isinstance(message, dict) else None
        if command == "info":
            send({"argv": sys.argv, "cwd": os.getcwd()})
        elif command == "note_exit":
            exit_note = message["path"]
            send({"noted": True})
        elif command == "burst":
            stdout.write(b"".join(frame(json.dumps({"burst": i}).encode("utf-8")) for i in range(message["count"])))
            stdout.flush()
        elif command == "garbage":
            stdout.write(frame(b"this is not json"))
            stdout.flush()
        elif command == "oversized":
            stdout.write(frame(b'"' + b"x" * (1024 * 1024) + b'"'))
            stdout.flush()
        elif command == "exit":
            sys.exit(0)
        else:
            send({"echo": message})


try:
    serve()
except BrokenPipeError:
    # The browser closed the pipe, as it does with a host that misbehaves.
    sys.exit(0)

if exit_note:
    with open(exit_note, "w") as file:
        file.write("exited")
