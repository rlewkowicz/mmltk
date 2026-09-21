"""Read retained benchmark image and annotation geometry through ./mmltk."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tarfile


def report(**fields):
    print(json.dumps(fields), flush=True)


def annotation_rows(path, image_id):
    # Version-3 normalized source cache; independent of the compiled .bin format.
    with path.open("rb") as stream:
        header = stream.read(256)
        if len(header) != 256 or struct.unpack_from("<QI", header) != (0x4D4D4C544B4E4931, 3):
            raise ValueError(f"unsupported normalized index: {path}")
        count = struct.unpack_from("<I", header, 16)[0]
        offset = struct.unpack_from("<Q", header, 36)[0]
        if offset != 256 or count * 32 > path.stat().st_size - offset:
            raise ValueError(f"invalid normalized image table: {path}")
        stream.seek(offset)
        for start in range(0, count, 4096):
            rows = min(4096, count - start)
            payload = stream.read(rows * 32)
            if len(payload) != rows * 32:
                raise ValueError(f"truncated normalized image table: {path}")
            for index, row in enumerate(struct.iter_unpack("<QQIIIHH", payload)):
                identity, first_box, boxes, width, height, shard, _ = row
                if identity == image_id:
                    report(kind="annotation", path=str(path), row=start + index, image_id=identity,
                           width=width, height=height, shard=shard, first_box=first_box, boxes=boxes)


def exif_orientation(payload):
    if not payload.startswith(b"Exif\0\0"):
        return None
    data = payload[6:]
    if len(data) < 8 or data[:2] not in (b"II", b"MM"):
        raise ValueError("invalid EXIF TIFF header")
    order = "<" if data[:2] == b"II" else ">"
    if struct.unpack_from(order + "H", data, 2)[0] != 42:
        raise ValueError("invalid EXIF TIFF magic")
    offset = struct.unpack_from(order + "I", data, 4)[0]
    if offset > len(data) - 2:
        raise ValueError("invalid EXIF directory offset")
    count = struct.unpack_from(order + "H", data, offset)[0]
    if count > (len(data) - offset - 2) // 12:
        raise ValueError("invalid EXIF directory size")
    for index in range(count):
        entry = offset + 2 + index * 12
        tag, kind, length = struct.unpack_from(order + "HHI", data, entry)
        if tag == 0x112 and kind == 3 and length == 1:
            return struct.unpack_from(order + "H", data, entry + 8)[0]
    return None


def image_geometry(data):
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        width, height = struct.unpack_from(">II", data, 16)
        return dict(encoding="PNG", width=width, height=height)
    if not data.startswith(b"\xff\xd8"):
        raise ValueError("image is neither JPEG nor PNG")
    result = dict(encoding="JPEG", orientation=None)
    offset = 2
    while offset < len(data):
        if data[offset] != 255:
            raise ValueError("invalid JPEG marker")
        while offset < len(data) and data[offset] == 255:
            offset += 1
        if offset >= len(data):
            raise ValueError("truncated JPEG marker")
        marker = data[offset]
        offset += 1
        if marker in (0xDA, 0xD9):
            break
        if marker == 1 or 0xD0 <= marker <= 0xD7:
            continue
        length = struct.unpack_from(">H", data, offset)[0]
        if length < 2 or length > len(data) - offset:
            raise ValueError("invalid JPEG segment extent")
        payload = data[offset + 2:offset + length]
        if marker == 0xE1:
            orientation = exif_orientation(payload)
            if orientation is not None:
                result["orientation"] = orientation
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            height, width = struct.unpack_from(">HH", payload, 1)
            result.update(width=width, height=height)
        offset += length
    if "width" not in result:
        raise ValueError("JPEG has no dimension header")
    return result


def inspect_image(data, export, **identity):
    if len(data) > 64 * 1024 * 1024:
        raise ValueError("image exceeds diagnostic's 64 MiB bound")
    report(kind="image", **identity, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), **image_geometry(data))
    if export:
        suffix = ".png" if data.startswith(b"\x89PNG") else ".jpg"
        path = Path("/evidence") / (str(identity["image_id"]) + suffix)
        path.write_bytes(data)
        report(kind="export", path=f"build/validation/benchmark-image/{path.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image-id", type=int, required=True)
    parser.add_argument("--index", type=Path, action="append", default=[])
    parser.add_argument("--image", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--export", action="store_true", help="copy inspected image into build/validation/benchmark-image")
    args = parser.parse_args()
    if args.image_id < 0 or args.image_id > 2**64 - 1:
        parser.error("image ID must fit uint64")
    for path in args.index:
        annotation_rows(path, args.image_id)
    if args.image:
        with args.image.open("rb") as stream:
            inspect_image(stream.read(64 * 1024 * 1024 + 1), args.export, path=str(args.image), image_id=args.image_id)
    if args.archive:
        report(kind="archive_scan", path=str(args.archive), image_id=args.image_id)
        with tarfile.open(args.archive, mode="r|*") as archive:
            for member in archive:
                identity = re.search(r"(\d+)\.(?:jpg|jpeg|png)$", member.name, re.IGNORECASE)
                if not member.isfile() or identity is None or int(identity[1]) != args.image_id:
                    continue
                if member.size > 64 * 1024 * 1024:
                    raise ValueError("image exceeds diagnostic's 64 MiB bound")
                with archive.extractfile(member) as stream:
                    inspect_image(stream.read(), args.export, archive=str(args.archive), member=member.name, image_id=args.image_id)
                return
        raise ValueError("requested image is absent from archive")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error, tarfile.TarError) as error:
        sys.exit(f"benchmark image diagnostic: {error}")
