#!/usr/bin/env python3

import argparse
import json
import os
import sys

from vastai_sdk import VastAI


def _require_api_key() -> str:
    api_key = os.getenv("VAST_API_KEY", "")
    if not api_key:
        raise RuntimeError("VAST_API_KEY is required")
    return api_key


def _make_client() -> VastAI:
    return VastAI(api_key=_require_api_key(), raw=True)


def _normalize_json_result(result, client: VastAI):
    payload = _payload_or_last_output(result, client)
    if isinstance(payload, str):
        text = payload.strip()
        if not text:
            return {}
        return json.loads(text)
    return payload


def _normalize_text_result(result, client: VastAI, *, strip_log_poll_lines: bool = False) -> str:
    payload = _payload_or_last_output(result, client)
    if not isinstance(payload, str):
        return json.dumps(payload)

    lines = payload.splitlines()
    if strip_log_poll_lines:
        lines = [
            line
            for line in lines
            if not line.startswith("waiting on logs for instance ")
        ]
    return "\n".join(lines).strip()


def _payload_or_last_output(result, client: VastAI):
    if result in (None, ""):
        return client.last_output
    return result


def _write_json(payload) -> None:
    json.dump(payload, sys.stdout)
    sys.stdout.write("\n")


def _write_json_result(result, client: VastAI) -> None:
    _write_json(_normalize_json_result(result, client))


def search_offers_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.search_offers(
        query=args.query,
        type=args.type,
        limit=args.limit,
        order=args.order,
        no_default=args.no_default,
        disable_bundling=args.disable_bundling,
        storage=args.storage,
    )
    _write_json_result(result, client)
    return 0


def create_instance_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.create_instance(
        id=args.offer_id,
        image=args.image,
        bid_price=args.bid_price,
        disk=args.disk,
        user=args.user,
        login=args.login,
        label=args.label,
        onstart=args.onstart,
        onstart_cmd=args.onstart_cmd,
        entrypoint=args.entrypoint,
        ssh=args.ssh,
        jupyter=args.jupyter,
        direct=args.direct,
        jupyter_dir=args.jupyter_dir,
        jupyter_lab=args.jupyter_lab,
        lang_utf8=args.lang_utf8,
        python_utf8=args.python_utf8,
        extra=args.extra,
        env=args.env,
        args=args.args,
        force=args.force,
        cancel_unavail=args.cancel_unavail,
        template_hash=args.template_hash,
    )
    _write_json_result(result, client)
    return 0


def show_instance_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.show_instance(args.instance_id)
    _write_json_result(result, client)
    return 0


def show_instances_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.show_instances(quiet=False)
    _write_json_result(result, client)
    return 0


def logs_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.logs(
        INSTANCE_ID=args.instance_id,
        tail=str(args.tail) if args.tail is not None else None,
        filter=args.filter,
        daemon_logs=args.daemon_logs,
    )
    _write_json({"instance_id": args.instance_id, "logs": _normalize_text_result(result, client, strip_log_poll_lines=True)})
    return 0


def start_instance_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.start_instance(args.instance_id)
    _write_json_result(result, client)
    return 0


def stop_instance_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.stop_instance(args.instance_id)
    _write_json_result(result, client)
    return 0


def destroy_instance_command(args: argparse.Namespace) -> int:
    client = _make_client()
    result = client.destroy_instance(args.instance_id)
    _write_json_result(result, client)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="mmltk Vast.ai bridge")
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

    create_parser = subparsers.add_parser("create-instance")
    create_parser.add_argument("offer_id", type=int)
    create_parser.add_argument("--image", required=True)
    create_parser.add_argument("--bid-price", dest="bid_price", type=float)
    create_parser.add_argument("--disk", type=float)
    create_parser.add_argument("--user")
    create_parser.add_argument("--login")
    create_parser.add_argument("--label")
    create_parser.add_argument("--onstart")
    create_parser.add_argument("--onstart-cmd")
    create_parser.add_argument("--entrypoint")
    create_parser.add_argument("--ssh", action="store_true")
    create_parser.add_argument("--jupyter", action="store_true")
    create_parser.add_argument("--direct", action="store_true")
    create_parser.add_argument("--jupyter-dir")
    create_parser.add_argument("--jupyter-lab", action="store_true")
    create_parser.add_argument("--lang-utf8", action="store_true")
    create_parser.add_argument("--python-utf8", action="store_true")
    create_parser.add_argument("--extra")
    create_parser.add_argument("--env")
    create_parser.add_argument("--force", action="store_true")
    create_parser.add_argument("--cancel-unavail", action="store_true")
    create_parser.add_argument("--template-hash", dest="template_hash")
    create_parser.add_argument("--args", nargs="*")
    create_parser.set_defaults(func=create_instance_command)

    show_instance_parser = subparsers.add_parser("show-instance")
    show_instance_parser.add_argument("instance_id", type=int)
    show_instance_parser.set_defaults(func=show_instance_command)

    show_instances_parser = subparsers.add_parser("show-instances")
    show_instances_parser.set_defaults(func=show_instances_command)

    logs_parser = subparsers.add_parser("logs")
    logs_parser.add_argument("instance_id", type=int)
    logs_parser.add_argument("--tail", type=int)
    logs_parser.add_argument("--filter")
    logs_parser.add_argument("--daemon-logs", action="store_true")
    logs_parser.set_defaults(func=logs_command)

    start_parser = subparsers.add_parser("start-instance")
    start_parser.add_argument("instance_id", type=int)
    start_parser.set_defaults(func=start_instance_command)

    stop_parser = subparsers.add_parser("stop-instance")
    stop_parser.add_argument("instance_id", type=int)
    stop_parser.set_defaults(func=stop_instance_command)

    destroy_parser = subparsers.add_parser("destroy-instance")
    destroy_parser.add_argument("instance_id", type=int)
    destroy_parser.set_defaults(func=destroy_instance_command)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
