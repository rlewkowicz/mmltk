#!/usr/bin/env python3

import argparse
import json
import os
import sys

from vastai_sdk import VastAI


def _normalize_result(result, client: VastAI):
    payload = result
    if payload in (None, ""):
        payload = client.last_output
    if isinstance(payload, str):
        text = payload.strip()
        if not text:
            return []
        return json.loads(text)
    return payload


def search_offers_command(args: argparse.Namespace) -> int:
    api_key = os.getenv("VAST_API_KEY", "")
    if not api_key:
        raise RuntimeError("VAST_API_KEY is required")

    client = VastAI(api_key=api_key)
    result = client.search_offers(
        query=args.query,
        type=args.type,
        limit=args.limit,
        order=args.order,
        no_default=args.no_default,
        disable_bundling=args.disable_bundling,
        storage=args.storage,
    )
    json.dump(_normalize_result(result, client), sys.stdout)
    sys.stdout.write("\n")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="fastloader Vast.ai bridge")
    subparsers = parser.add_subparsers(dest="command", required=True)

    search_parser = subparsers.add_parser("search-offers")
    search_parser.add_argument("--query", required=True)
    search_parser.add_argument("--type", default="on-demand")
    search_parser.add_argument("--limit", type=int, default=64)
    search_parser.add_argument("--order", default="dlperf_usd-")
    search_parser.add_argument("--storage", type=float, default=5.0)
    search_parser.add_argument("--no-default", action="store_true")
    search_parser.add_argument("--disable-bundling", action="store_true")
    search_parser.set_defaults(func=search_offers_command)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
