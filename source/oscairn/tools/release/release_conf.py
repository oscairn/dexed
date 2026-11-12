#!/usr/bin/env python3
"""Shared plumbing for the release scripts: read release.conf, hash files."""
import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def _repo_root():
    d = HERE
    while d != os.path.dirname(d) and not os.path.exists(os.path.join(d, '.git')):
        d = os.path.dirname(d)
    return d


REPO = _repo_root()


def conf():
    """Parse release.conf's KEY=VALUE lines (the same file the sh scripts source)."""
    out = {}
    with open(os.path.join(HERE, 'release.conf'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            key, _, value = line.partition('=')
            out[key] = value.strip().strip('"')
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()
