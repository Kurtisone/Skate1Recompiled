#!/usr/bin/env python3
"""Merge skate1's Vulkan shader storage from several devices.

The storage lists every guest shader (45410813.xsh) and every pipeline
(45410813.fbo.vk.xpso) a device has seen; at launch the game builds all of
them up front, so a pipeline already in the storage never stutters in game.
Both files are GPU-independent, so merging those of a Steam Deck, an Android
handheld and a PC gives each of them the pipelines the others met.

  tools/merge_shader_storage.py OUT_DIR IN_DIR [IN_DIR ...]

Each IN_DIR holds the files of one device, as found in
<user data>/cache/shaders/shareable/. Files of another format version are
skipped with a warning. Copy OUT_DIR's files back into that folder on each
device (with the game closed).
"""

import os
import struct
import sys

TITLE = "45410813"
SHADERS = TITLE + ".xsh"
PIPELINES = TITLE + ".fbo.vk.xpso"
SHADER_FILE_HEADER = 8  # magic, version
SHADER_RECORD_HEADER = 12  # ucode hash (u64), dword count (31 bits) + type (1 bit)
PIPELINE_FILE_HEADER = 12  # magic, magic_api, version
PIPELINE_RECORD = 66  # description hash (u64) + PipelineDescription


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def merge_shaders(inputs):
    header, seen, records = None, set(), []
    for data in inputs:
        if data is None or len(data) < SHADER_FILE_HEADER:
            continue
        if header is None:
            header = data[:SHADER_FILE_HEADER]
        elif data[:SHADER_FILE_HEADER] != header:
            print("warning: skipping shaders of another format version", file=sys.stderr)
            continue
        pos = SHADER_FILE_HEADER
        while pos + SHADER_RECORD_HEADER <= len(data):
            ucode_hash, packed = struct.unpack_from("<QI", data, pos)
            size = SHADER_RECORD_HEADER + (packed & 0x7FFFFFFF) * 4
            if pos + size > len(data):
                break  # Truncated last record.
            if ucode_hash not in seen:
                seen.add(ucode_hash)
                records.append(data[pos:pos + size])
            pos += size
    return header, records


def merge_pipelines(inputs):
    header, seen, records = None, set(), []
    for data in inputs:
        if data is None or len(data) < PIPELINE_FILE_HEADER:
            continue
        if header is None:
            header = data[:PIPELINE_FILE_HEADER]
        elif data[:PIPELINE_FILE_HEADER] != header:
            print("warning: skipping pipelines of another format version", file=sys.stderr)
            continue
        body = data[PIPELINE_FILE_HEADER:]
        for pos in range(0, len(body) - PIPELINE_RECORD + 1, PIPELINE_RECORD):
            record = body[pos:pos + PIPELINE_RECORD]
            if record not in seen:
                seen.add(record)
                records.append(record)
    return header, records


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 1
    out_dir, in_dirs = sys.argv[1], sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)
    shader_header, shaders = merge_shaders(read(os.path.join(d, SHADERS)) for d in in_dirs)
    pipeline_header, pipelines = merge_pipelines(
        read(os.path.join(d, PIPELINES)) for d in in_dirs)
    if shader_header:
        with open(os.path.join(out_dir, SHADERS), "wb") as f:
            f.write(shader_header + b"".join(shaders))
    if pipeline_header:
        with open(os.path.join(out_dir, PIPELINES), "wb") as f:
            f.write(pipeline_header + b"".join(pipelines))
    print(f"{len(shaders)} shaders, {len(pipelines)} pipelines -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
