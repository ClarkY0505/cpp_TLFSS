#!/usr/bin/env python3
"""Process smoke checks for the standalone collector; no database is required."""

import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def read_ready(process):
    deadline = time.monotonic() + 10
    output = b""
    while time.monotonic() < deadline:
        readable, _, _ = select.select([process.stdout], [], [],
                                     max(0, deadline - time.monotonic()))
        if not readable:
            break
        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            break
        output += chunk
        match = re.search(rb"READY metrics=(\d+) alarms=(\d+) cli=(\d+)\n", output)
        if match:
            ports = tuple(int(value) for value in match.groups())
            assert all(0 < port <= 65535 for port in ports), ports
            return ports
    raise AssertionError("collector did not become ready: " + output.decode(errors="replace"))


def read_prompt(connection):
    output = b""
    while not output.endswith(b"> "):
        chunk = connection.recv(4096)
        if not chunk:
            raise AssertionError("CLI closed before response: " + repr(output))
        output += chunk
    return output.decode()


def request(connection, command):
    connection.sendall(command.encode() + b"\n")
    return read_prompt(connection)


def wait_response(connection, command, predicate):
    deadline = time.monotonic() + 5
    output = ""
    while time.monotonic() < deadline:
        output = request(connection, command)
        if predicate(output):
            return output
    raise AssertionError("condition not reached for " + command + ": " + output)


def v2_record(module, level, function, event, description, value=0):
    description = description.encode()
    numeric = isinstance(value, int)
    payload = struct.pack("!II", value, 2) if numeric else value.encode()
    header = struct.pack("!BBHHHIIIIqIHH", 2, 0 if numeric else 1, 0,
                         40 + len(description) + len(payload), 40,
                         module, level, function, event, 1234567890, 999,
                         len(description), len(payload))
    return header + description + payload


def check_metrics(connection, metrics_port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        endpoint = ("127.0.0.1", metrics_port)
        # Malformed traffic must not prevent the following valid zero update.
        sender.sendto(b"invalid-wire-record", endpoint)
        sender.sendto(v2_record(0x1000, 0, 1, 0, "zero-test"), endpoint)
        dump = wait_response(connection, "db_dump", lambda text: "zero-test" in text)
        assert 'mid=4096 lvl=info fid=1 eid=0 num=0' in dump, dump
        assert 'changed_at=1234567890.000000999' in dump, dump

        # Similar keys and nonnumeric heartbeat payloads must not imply liveness.
        sender.sendto(v2_record(0x100a, 1, 1, 0, "wrong-level"), endpoint)
        sender.sendto(v2_record(0x100a, 0, 1, 0, "string-heartbeat", "bad"), endpoint)
        sender.sendto(v2_record(0xdead, 0, 1, 0, "unknown-key"), endpoint)
        sender.sendto(v2_record(0x1100, 1, 1, 0, "udp-alarm"), endpoint)
        sender.sendto(v2_record(0x1000, 0, 1, 0, "invalid-batch-barrier", 1), endpoint)
        dump = wait_response(connection, "db_dump", lambda text: "invalid-batch-barrier" in text)
        for rejected in ("wrong-level", "string-heartbeat", "unknown-key", "udp-alarm"):
            assert rejected not in dump, dump
        assert "never_seen" in request(connection, "service_status")

        heartbeat = v2_record(0x100a, 0, 1, 0, "heartbeat-test")
        sender.sendto(heartbeat, endpoint)
        wait_response(connection, "service_status", lambda text: "service=online" in text)
        stale = wait_response(connection, "service_status",
                              lambda text: "suspected_unreachable" in text)
        assert re.search(r"last_age_ms=\d+", stale), stale
        # An identical value with identical remote changed_at is a fresh receipt.
        sender.sendto(heartbeat, endpoint)
        wait_response(connection, "service_status", lambda text: "service=online" in text)


def run_session(executable, inbox, stop_mode):
    process = subprocess.Popen(
        [executable, "--metrics-port", "0", "--alarms-port", "0", "--cli-port", "0",
         "--inbox", str(inbox), "--heartbeat-timeout-ms", "300"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        metrics_port, _, cli_port = read_ready(process)
        with socket.create_connection(("127.0.0.1", cli_port), timeout=5) as connection:
            connection.settimeout(5)
            read_prompt(connection)
            modules = request(connection, "modules")
            for module in ("service", "auth", "upload", "download", "resources", "storage_alarms"):
                assert module + " mid=" in modules, modules
            status = request(connection, "service_status")
            assert "never_seen" in status and "last_age_ms=" in status, status
            help_text = request(connection, "help")
            for command in ("modules", "use", "db_dump", "alarm_status", "service_status", "stop"):
                assert command in help_text, help_text
            alarms = request(connection, "alarm_status")
            assert "collector" in alarms, alarms
            check_metrics(connection, metrics_port)
            if stop_mode == "cli":
                connection.sendall(b"stop\n")
            else:
                process.send_signal(stop_mode)
        output, _ = process.communicate(timeout=10)
        assert process.returncode == 0, (process.returncode, output.decode(errors="replace"))
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


def main():
    executable = str(Path(sys.argv[1]).resolve())
    assert Path(executable).is_file(), "collector executable is missing: " + executable
    with tempfile.TemporaryDirectory(prefix="tlss-collector-") as temporary:
        inbox = Path(temporary) / "inbox"
        for args in (("--metrics-port", "65536"), ("--alarms-port", "-1"),
                     ("--cli-port", "12junk"), ("--heartbeat-timeout-ms", "0"),
                     ("--heartbeat-timeout-ms", "999999999999999999999999"),
                     ("--unknown", "1"), ("--inbox",)):
            result = subprocess.run([executable, *args], capture_output=True, timeout=5)
            assert result.returncode != 0, args
        result = subprocess.run([executable, "--help"], cwd=temporary,
                                capture_output=True, timeout=5)
        assert result.returncode == 0 and b"--inbox" in result.stdout, result
        assert not list(Path(temporary).iterdir()), "--help initialized resources"
        run_session(executable, inbox, signal.SIGTERM)
        assert inbox.is_dir(), inbox
        run_session(executable, inbox, signal.SIGINT)
        run_session(executable, inbox, "cli")
    print("storage collector process smoke passed")


if __name__ == "__main__":
    main()
