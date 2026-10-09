#!/usr/bin/env python3
"""Aggregate per-run harness evidence for a staged acceptance gate."""
import argparse
import json
import pathlib
import re
import sys


def read(path):
    return path.read_text(errors="replace") if path.exists() else ""


def inspect(run):
    meta = json.loads(read(run / "run.json") or "{}")
    stage_text = read(run / "stage.txt").strip()
    stage = int(stage_text) if stage_text.isdigit() else 0
    scan = read(run / "scan.txt")
    return {
        "run": run.name,
        "stage": stage,
        "clean": meta.get("exit_code") == 0 and not re.search(
            r"\[(?:crash|unimplemented|gpu_validation)\] [1-9]", scan
        ),
        "elapsed": float(meta.get("elapsed_seconds", 0)),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", nargs="+", type=pathlib.Path)
    parser.add_argument("--stage", type=int, required=True)
    args = parser.parse_args()
    records = [inspect(run) for run in args.runs]
    passed = all(record["stage"] >= args.stage and record["clean"] for record in records)
    print("[acceptance] stage=%d runs=%d result=%s" %
          (args.stage, len(records), "PASS" if passed else "FAIL"))
    for record in records:
        print("[acceptance] %s stage=%d clean=%s elapsed_s=%.3f" %
              (record["run"], record["stage"], "yes" if record["clean"] else "no",
               record["elapsed"]))
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
