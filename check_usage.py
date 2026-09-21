#!/usr/bin/env python3
"""Quick standalone check of opencode Zen quota usage, same endpoint the
ESP32 firmware polls. Reads the token from .env (opencode_token=...)."""
import json
import os
import sys
import urllib.request

def load_env(path=".env"):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            values[k.strip()] = v.strip()
    return values

def main():
    env = load_env()
    token = env.get("opencode_token")
    if not token:
        print("No opencode_token found in .env", file=sys.stderr)
        sys.exit(1)

    req = urllib.request.Request(
        "https://opencode.ai/zen/go/v1/usage",
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
            "User-Agent": "opencode/1.0",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode(errors='replace')}", file=sys.stderr)
        sys.exit(1)

    usage = data.get("usage", {})
    for key in ("rolling", "weekly", "monthly"):
        bucket = usage.get(key, {})
        pct = bucket.get("percent")
        status = bucket.get("status")
        resets = bucket.get("resetsAt")
        print(f"{key:8s} {pct}% (status={status}, resets={resets})")

if __name__ == "__main__":
    main()
