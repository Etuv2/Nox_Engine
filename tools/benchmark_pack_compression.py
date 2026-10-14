"""Read-only pack payload benchmark; this does not build a NOX runtime archive.

Optional codecs: pip install --target build/research/python zstandard lz4
Example: python tools/benchmark_pack_compression.py --codec-path build/research/python
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import io
import json
from pathlib import Path
import platform
import statistics
import sys
import time
import zipfile
import zlib


ASSET_ROOTS = {"config", "fonts", "hdrs", "images", "scenes", "shaders", "sounds", "textures", "models", "data", "generated"}
MUTABLE = {"config/editor_settings.json", "config/editor_layout.ini", "projects/nox-racing/data/speedwall.json"}
BLOCK_SIZE = 256 * 1024


def raw_deflate(data: bytes) -> bytes:
    compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


def codecs():
    result = [("deflate-6", raw_deflate, lambda data: zlib.decompress(data, -15))]
    versions = {"zlib": zlib.ZLIB_RUNTIME_VERSION}
    try:
        import zstandard
        versions["zstandard_python"] = zstandard.__version__
        versions["zstd_library"] = list(zstandard.ZSTD_VERSION)
        for level in (3, 9):
            compressor = zstandard.ZstdCompressor(level=level)
            decompressor = zstandard.ZstdDecompressor()
            result.append((f"zstd-{level}", compressor.compress, decompressor.decompress))
    except ImportError:
        pass
    try:
        import lz4
        import lz4.frame
        versions["lz4_python"] = lz4.__version__
        result.append(("lz4-fast", lz4.frame.compress, lz4.frame.decompress))
    except ImportError:
        pass
    return result, versions


def encode(data, compress, block_size):
    pieces = []
    for offset in range(0, len(data), block_size or max(len(data), 1)):
        block = data[offset:offset + block_size] if block_size else data
        compressed = compress(block)
        # Do not pay decompression cost for less than 1% or 64 bytes of savings.
        use_compression = len(block) - len(compressed) >= max(64, len(block) // 100)
        pieces.append((use_compression, compressed if use_compression else block))
    return pieces


def decode(pieces, decompress):
    return b"".join(decompress(data) if compressed else data for compressed, data in pieces)


def load_assets(manifest_path):
    manifest_path = manifest_path.resolve(strict=True)
    root = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assets = []
    seen = set()
    for entry in manifest["files"]:
        name = entry["path"].replace("\\", "/")
        if name.split("/")[0] not in ASSET_ROOTS or name in MUTABLE:
            continue
        path = (root / name).resolve(strict=True)
        if not path.is_relative_to(root):
            raise ValueError(f"Manifest path escapes package: {name}")
        if name.casefold() in seen:
            raise ValueError(f"Duplicate/case-colliding virtual path: {name}")
        seen.add(name.casefold())
        assets.append((name, path, entry))
    return manifest, assets


def measure(manifest_path, codec_list, repeats):
    manifest, assets = load_assets(manifest_path)
    modes = [(name, compress, decompress, 0) for name, compress, decompress in codec_list]
    modes += [(name + "-256k", compress, decompress, BLOCK_SIZE)
              for name, compress, decompress in codec_list if name in {"zstd-3", "lz4-fast"}]
    totals = {name: {"payload_bytes": 0, "unique_payload_bytes": 0, "compress_seconds": 0.0,
                     "decode_seconds": 0.0, "compressed_blocks": 0, "stored_blocks": 0,
                     "block_count": 0, "by_extension": {}, "largest_savings": []}
              for name, _, _, _ in modes}
    by_ext = defaultdict(lambda: {"files": 0, "bytes": 0})
    fingerprints = []
    seen_hashes = set()
    duplicates = []
    changed = []
    raw_bytes = 0
    unique_raw_bytes = 0
    path_bytes = 0
    for name, path, expected in assets:
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != expected["sha256"] or len(data) != expected["bytes"]:
            changed.append(name)
        duplicate = digest in seen_hashes
        seen_hashes.add(digest)
        if duplicate:
            duplicates.append(name)
        else:
            unique_raw_bytes += len(data)
        raw_bytes += len(data)
        path_bytes += len(name.encode("utf-8"))
        ext = Path(name).suffix.lower()
        by_ext[ext]["files"] += 1
        by_ext[ext]["bytes"] += len(data)
        fingerprints.append({"path": name, "bytes": len(data), "sha256": digest})
        for mode, compress, decompress, block_size in modes:
            encode_times, decode_times = [], []
            for _ in range(repeats):
                start = time.perf_counter()
                pieces = encode(data, compress, block_size)
                encode_times.append(time.perf_counter() - start)
                start = time.perf_counter()
                restored = decode(pieces, decompress)
                decode_times.append(time.perf_counter() - start)
                if restored != data:
                    raise AssertionError(f"Round-trip mismatch: {mode}: {name}")
            size = sum(len(block) for _, block in pieces)
            total = totals[mode]
            total["payload_bytes"] += size
            total["unique_payload_bytes"] += 0 if duplicate else size
            total["compress_seconds"] += statistics.median(encode_times)
            total["decode_seconds"] += statistics.median(decode_times)
            total["compressed_blocks"] += sum(compressed for compressed, _ in pieces)
            total["stored_blocks"] += sum(not compressed for compressed, _ in pieces)
            total["block_count"] += len(pieces)
            group = total["by_extension"].setdefault(ext, {"raw_bytes": 0, "payload_bytes": 0})
            group["raw_bytes"] += len(data)
            group["payload_bytes"] += size
            total["largest_savings"].append({"path": name, "saved_bytes": len(data) - size})
    for name, _, _, block_size in modes:
        total = totals[name]
        total["saved_percent"] = 100 * (1 - total["payload_bytes"] / max(raw_bytes, 1))
        total["largest_savings"] = sorted(total["largest_savings"], key=lambda entry: entry["saved_bytes"], reverse=True)[:10]
        # A sizing assumption for a possible custom index, NOT a committed format.
        total["illustrative_index_bytes"] = 64 + 64 * len(assets) + path_bytes + (32 * total["block_count"] if block_size else 0)
        if name == "deflate-6":
            # Exact for ordinary ZIP entries with no comments, extra fields, descriptors or ZIP64.
            total["zip_container_bytes"] = total["payload_bytes"] + 22 + 76 * len(assets) + 2 * path_bytes
    snapshot = json.dumps(fingerprints, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {"profile": manifest["profile"], "configuration": manifest["configuration"],
            "asset_files": len(assets), "raw_bytes": raw_bytes, "unique_raw_bytes": unique_raw_bytes,
            "identical_file_savings_bytes": raw_bytes - unique_raw_bytes,
            "duplicate_paths": duplicates, "changed_since_manifest": changed,
            "snapshot_sha256": hashlib.sha256(snapshot).hexdigest(),
            "by_extension": dict(sorted(by_ext.items())), "modes": totals}


def self_test(codec_list):
    noise = b"".join(hashlib.sha256(str(i).encode("ascii")).digest() for i in range(1024))
    samples = [b"", b"small", b"repeat-me" * 100000, bytes(range(256)) * 1024, noise]
    for name, compress, decompress in codec_list:
        for data in samples:
            for block_size in (0, BLOCK_SIZE):
                pieces = encode(data, compress, block_size)
                assert decode(pieces, decompress) == data, name
                assert sum(len(part) for _, part in pieces) <= len(data), name
    assert not any(compressed for compressed, _ in encode(noise, raw_deflate, 0))
    sample = samples[2]
    pieces = encode(sample, raw_deflate, 0)
    stream = io.BytesIO()
    path = "textures/check.bin"
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr(path, sample, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    assert len(stream.getvalue()) == sum(len(part) for _, part in pieces) + 22 + 76 + 2 * len(path)
    print("Codec round trips, adaptive storage, block boundaries, and ZIP sizing passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", nargs="+", type=Path, default=[Path("Package/NOX_Racing/package-manifest.json"), Path("Package/NOX_Engine/package-manifest.json")])
    parser.add_argument("--codec-path", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/research/pack-compression.json"))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    if args.codec_path:
        sys.path.insert(0, str(args.codec_path.resolve(strict=True)))
    codec_list, versions = codecs()
    if args.self_test:
        self_test(codec_list)
        return
    report = {"python": platform.python_version(), "platform": platform.platform(),
              "codecs": versions, "repeats_per_file": args.repeats, "block_bytes": BLOCK_SIZE,
              "method": "Per-file median in-memory encode/decode times, summed. No disk I/O, asset parsing or GPU uploads in timings. Store when savings are below max(64 bytes, 1%). Runtime assets only; mutable editor settings, DLLs, executable, licenses and generated files absent from manifest are excluded.",
              "packages": []}
    for manifest in args.manifest:
        measured = measure(manifest, codec_list, args.repeats)
        report["packages"].append(measured)
        print(f"{measured['profile']}: {measured['asset_files']} assets, {measured['raw_bytes'] / 2**20:.2f} MiB")
        for name, row in measured["modes"].items():
            print(f"  {name:18} {row['payload_bytes'] / 2**20:7.2f} MiB  {row['saved_percent']:5.1f}% saved  encode {row['compress_seconds']:.3f}s  decode {row['decode_seconds']:.3f}s")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(f"Results: {args.output}")


if __name__ == "__main__":
    main()
