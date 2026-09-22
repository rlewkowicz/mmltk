"""Read retained benchmark image and annotation geometry through ./mmltk."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile


def report(**fields):
    print(json.dumps(fields), flush=True)


def compiled_sample(path, sample):
    with path.open("rb") as stream:
        header = stream.read(8328)
        if len(header) != 8328 or struct.unpack_from("<QI", header) != (0x464153544C445232, 9):
            raise ValueError(f"unsupported compiled dataset: {path}")
        count, width, height = struct.unpack_from("<III", header, 12)
        index_offset, label_offset = struct.unpack_from("<QQ", header, 32)
        if sample < 0 or sample >= count:
            raise ValueError("sample index is outside the compiled dataset")
        stream.seek(index_offset + sample * 40)
        entry = stream.read(40)
        if len(entry) != 40:
            raise ValueError("truncated compiled image entry")
        _, first_label, instances, _, label_bytes, source_width, source_height, has_id, source, _, image_id = struct.unpack("<QIHHIIIBBHQ", entry)
        if label_bytes != instances * 60:
            raise ValueError("compiled label count disagrees with byte extent")
        stream.seek(label_offset + first_label)
        labels = []
        for _ in range(instances):
            payload = stream.read(60)
            if len(payload) != 60:
                raise ValueError("truncated compiled label")
            category, flags, x1, y1, x2, y2, mask_offset, mask_pairs, area, annotation_id, source_category, ordinal = struct.unpack("<BBffffQHdQQQ", payload)
            class_name = header[80 + category * 32:112 + category * 32].split(b"\0", 1)[0].decode("utf-8")
            labels.append(dict(class_id=category, class_name=class_name, flags=flags, bbox=[x1, y1, x2, y2],
                               mask_pairs=mask_pairs, annotation_id=annotation_id, source_category_id=source_category, original_area=area))
        report(kind="compiled_sample", path=str(path), sample=sample, image_count=count,
               width=width, height=height, original_width=source_width, original_height=source_height,
               source=source, image_id=image_id if has_id else None, instances=labels)
        return image_id if has_id else None


def annotation_rows(path, image_id, include_objects=False):
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
                    if include_objects:
                        box_offset = struct.unpack_from("<Q", header, 44)[0] + first_box * 64
                        if boxes > 65535 or box_offset + boxes * 64 > path.stat().st_size:
                            raise ValueError("normalized annotation slice exceeds admission")
                        stream.seek(box_offset)
                        payload = stream.read(boxes * 64)
                        if len(payload) != boxes * 64:
                            raise ValueError("truncated normalized annotations")
                        for box in struct.iter_unpack("<ffffQIBB2xdQQQ", payload):
                            x1, y1, x2, y2, _, pairs, category, flags, area, annotation_id, source_category, ordinal = box
                            report(kind="annotation_object", path=str(path), image_id=identity, annotation_id=annotation_id,
                                   class_id=category, source_category_id=source_category, flags=flags,
                                   bbox=[x1, y1, x2, y2], mask_pairs=pairs, original_area=area, source_ordinal=ordinal)
                    return


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
    parser.add_argument("--image-id", type=int)
    parser.add_argument("--compiled", type=Path)
    parser.add_argument("--sample", type=int, help="zero-based compiled sample index")
    parser.add_argument("--index", type=Path, action="append", default=[])
    parser.add_argument("--objects", action="store_true", help="include normalized object records for each matching index row")
    parser.add_argument("--image", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--parquet", type=Path, help="inspect a raw COCONut mask and segment metadata")
    parser.add_argument("--panoptic", type=Path, help="count RGB segment IDs in a retained panoptic PNG")
    parser.add_argument("--export", action="store_true", help="copy inspected image into build/validation/benchmark-image")
    args = parser.parse_args()
    if (args.compiled is None) != (args.sample is None):
        parser.error("--compiled and --sample are required together")
    if args.compiled:
        identity = compiled_sample(args.compiled, args.sample)
        if args.image_id is not None and identity != args.image_id:
            parser.error("--image-id disagrees with the compiled sample")
        args.image_id = identity
    if args.image_id is None and not args.compiled:
        parser.error("--image-id or --compiled with --sample is required")
    if args.image_id is not None and (args.image_id < 0 or args.image_id > 2**64 - 1):
        parser.error("image ID must fit uint64")
    if args.image_id is None and (args.index or args.image or args.archive or args.parquet or args.panoptic):
        parser.error("this compiled sample has no source image ID")
    if args.parquet or args.panoptic:
        executable = Path("/evidence/diagnose_coconut_mask")
        subprocess.run([
            "/opt/gcc-16.2/bin/g++", "-std=c++26", "-O2",
            "-isystem", "/opt/arrow/include", "-I/workspace/third_party/json/include", "-I/workspace/third_party/stb",
            "/workspace/tools/diagnose_coconut_mask.cpp", "-L/opt/arrow/lib",
            "-lparquet", "-larrow", "-larrow_bundled_dependencies", "-pthread", "-ldl",
            "-o", str(executable),
        ], check=True, timeout=120)
        for mode, path in (("parquet", args.parquet), ("png", args.panoptic)):
            if path:
                subprocess.run([str(executable), mode, str(path), str(args.image_id), str(int(args.export))], check=True, timeout=120)
    for path in args.index:
        annotation_rows(path, args.image_id, args.objects)
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
    except (OSError, ValueError, struct.error, tarfile.TarError, subprocess.SubprocessError) as error:
        sys.exit(f"benchmark image diagnostic: {error}")
