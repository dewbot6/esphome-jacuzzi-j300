#!/usr/bin/env python3
"""Minimal client for Saleae Logic 2's built-in MCP server (127.0.0.1:10530/mcp).

Usage:
  saleae.py capture <name> <seconds> [--digital 0,1,..] [--analog 0,1,..]
                    [--drate 25000000] [--arate 625000]
Saves captures/<name>.sal and exports CSVs to captures/<name>/.
"""
import argparse, json, os, sys, urllib.request

URL = "http://127.0.0.1:10530/mcp"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def call(name, args, timeout=600):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(URL, body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.load(r)
    if "error" in resp:
        sys.exit(f"{name}: {resp['error']}")
    res = resp["result"]
    if res.get("isError"):
        sys.exit(f"{name}: {res['content'][0]['text']}")
    return res.get("structuredContent", {})


def chans(s):
    return [int(c) for c in s.split(",")] if s else []


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("name")
    c.add_argument("seconds", type=float)
    c.add_argument("--digital", default="0,1,2,3,4,5,6")
    c.add_argument("--analog", default="")
    c.add_argument("--drate", type=int, default=25_000_000)
    c.add_argument("--arate", type=int, default=625_000)
    c.add_argument("--downsample", type=int, default=1)
    a = p.parse_args()

    dig, ana = chans(a.digital), chans(a.analog)
    logic = {}
    if dig: logic["digitalChannels"] = dig
    if ana: logic["analogChannels"] = ana
    cfg = {"logicChannels": logic}
    if dig: cfg["digitalSampleRate"] = a.drate
    if ana: cfg["analogSampleRate"] = a.arate

    cid = call("start_capture", {"logicDeviceConfiguration": cfg,
               "captureConfiguration": {"timedCaptureMode": {"durationSeconds": a.seconds}}})["captureId"]
    call("wait_capture", {"captureId": cid})
    out = os.path.join(REPO, "captures", a.name)
    os.makedirs(out, exist_ok=True)
    call("export_raw_data_csv", {"captureId": cid, "directory": out,
         "logicChannels": logic, "analogDownsampleRatio": a.downsample})
    call("save_capture", {"captureId": cid, "filepath": out + ".sal"})
    call("close_capture", {"captureId": cid})
    print(out)


if __name__ == "__main__":
    main()
