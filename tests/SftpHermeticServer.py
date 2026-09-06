#!/usr/bin/env python3
"""Test-only loopback SSH/SFTP server; no shell, forwarding, or real credentials.

Requires Paramiko. ROOT must be an explicit temporary test directory. The first
stdout JSON line supplies an ephemeral port and generated RSA host fingerprint.
Subsequent JSON lines record authentication/deletion for pre-auth safety tests.
All remote operations are rooted under ROOT; only password fixture auth exists.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import sys
import threading

import paramiko

ROOT = Path(sys.argv[1]).resolve(strict=True)
if not ROOT.is_dir() or ROOT == Path(ROOT.anchor):
    raise SystemExit("An explicit test directory is required")
PRINT_LOCK = threading.Lock()


def report(**values):
    with PRINT_LOCK:
        print(json.dumps(values), flush=True)


class Authentication(paramiko.ServerInterface):
    def get_allowed_auths(self, username):
        return "password"

    def check_auth_password(self, username, password):
        # Report an attempt, never the submitted password.
        report(event="authentication", username=username)
        if username == "apm-test" and password == "apm-test-password":
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, channel_id):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED


class Files(paramiko.SFTPServerInterface):
    @staticmethod
    def path(remote):
        if not remote.startswith("/") or "\x00" in remote or "\\" in remote:
            raise PermissionError("invalid path")
        parts = remote.split("/")
        if any(part in (".", "..") for part in parts):
            raise PermissionError("invalid path segment")
        candidate = ROOT.joinpath(*[part for part in parts if part])
        # Parent links may not escape the fixture root. Keep the leaf itself
        # unresolved so lstat can expose symlinks for client rejection tests.
        parent = candidate.parent.resolve(strict=True)
        if candidate != ROOT and parent != ROOT and ROOT not in parent.parents:
            raise PermissionError("path escaped fixture")
        return candidate

    def list_folder(self, path):
        try:
            folder = self.path(path)
            entries = []
            for item in folder.iterdir():
                attributes = paramiko.SFTPAttributes.from_stat(item.lstat())
                attributes.filename = item.name
                entries.append(attributes)
            return entries
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno or 13)

    def stat(self, path):
        return self.lstat(path)

    def lstat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(self.path(path).lstat())
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno or 13)

    def open(self, path, flags, attr):
        try:
            target = self.path(path)
            if flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC):
                return paramiko.SFTP_PERMISSION_DENIED
            if not stat.S_ISREG(target.lstat().st_mode):
                return paramiko.SFTP_PERMISSION_DENIED
            handle = paramiko.SFTPHandle(flags)
            handle.readfile = target.open("rb")
            handle.stat = lambda: paramiko.SFTPAttributes.from_stat(os.fstat(handle.readfile.fileno()))
            return handle
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno or 13)

    def remove(self, path):
        try:
            target = self.path(path)
            if not stat.S_ISREG(target.lstat().st_mode):
                return paramiko.SFTP_PERMISSION_DENIED
            target.unlink()
            report(event="delete", path=path)
            return paramiko.SFTP_OK
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno or 13)


key = paramiko.RSAKey.generate(2048)
listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.bind(("127.0.0.1", 0))
listener.listen(16)
fingerprint = "SHA256:" + base64.b64encode(hashlib.sha256(key.asbytes()).digest()).decode().rstrip("=")
report(port=listener.getsockname()[1], fingerprint=fingerprint)


def connection(client):
    transport = paramiko.Transport(client)
    try:
        transport.add_server_key(key)
        transport.set_subsystem_handler("sftp", paramiko.SFTPServer, Files)
        transport.start_server(server=Authentication())
        channels = []
        while transport.is_active():
            channel = transport.accept(0.2)
            if channel is not None:
                channels.append(channel)
    except (EOFError, OSError, paramiko.SSHException):
        pass
    finally:
        transport.close()


while True:
    client, _ = listener.accept()
    threading.Thread(target=connection, args=(client,), daemon=True).start()
