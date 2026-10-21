#!/usr/bin/env python3
"""Merge header-derived facts into symbol-derived IR 1.4.0.

Usage:
  python merge-symbol-ir.py --symbol <symbol-ir-dir> --header <header-ir-dir> --output <dir>
"""

import argparse
import copy
import hashlib
import json
import os
import re


def load_records(path):
    with open(path, encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def record_key(record):
    return record.get("id") or record.get("declarationId") or ""


def serialize(records):
    ordered = sorted(records, key=lambda record: (record_key(record), record.get("recordType", "")))
    return "".join(
        json.dumps(record, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"
        for record in ordered
    )


def sha256_text(text):
    return "sha256:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def normalize_signature(identifier):
    if "#" not in identifier:
        return identifier
    prefix, signature = identifier.split("#", 1)
    signature = signature.replace(" ", "")
    signature = signature.replace("class", "").replace("struct", "").replace("enum", "")
    signature = signature.replace("std::__1::", "std::").replace("std::__cxx11::", "std::")
    signature = signature.replace("Puv::Legacy::", "")
    signature = signature.replace("const", "").replace("volatile", "")
    signature = signature.replace("unsigned__int64", "unsignedlonglong")
    signature = signature.replace("__int64", "longlong")
    signature = signature.replace("unsignedlong", "unsignedlonglong")
    signature = signature.replace("std::basic_string<char,std::char_traits<char>,std::allocator<char>>", "std::string")
    signature = signature.replace("std::basic_string<unsignedshort,std::char_traits<unsignedshort>,std::allocator<unsignedshort>>", "std::u16string")
    # The demangler prints default allocator/delete arguments; clang canonical types omit them.
    while True:
        simplified = re.sub(r",std::allocator<[^<>]*>>", ">", signature)
        simplified = re.sub(r",std::allocator<[^<>]*>", "", simplified)
        simplified = re.sub(r",std::default_delete<[^<>]*>>", ">", simplified)
        simplified = re.sub(r",std::default_delete<[^<>]*>", "", simplified)
        if simplified == signature:
            break
        signature = simplified
    return prefix + "#" + signature


def find_shard(manifest, configuration):
    for shard in manifest.get("shards", []):
        if shard.get("configuration") == configuration:
            return shard
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--header", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--classes", default="Actor,BlockPos")
    args = parser.parse_args()

    classes = [item.strip() for item in args.classes.split(",") if item.strip()]
    with open(os.path.join(args.symbol, "ir-manifest.json"), encoding="utf-8") as stream:
        symbol_manifest = json.load(stream)
    with open(os.path.join(args.header, "ir-manifest.json"), encoding="utf-8") as stream:
        header_manifest = json.load(stream)

    configurations = [configuration["id"] for configuration in symbol_manifest.get("configurations", [])]
    symbol_records = {configuration: [] for configuration in configurations}
    for configuration in configurations:
        shard = find_shard(symbol_manifest, configuration)
        if shard is None:
            raise SystemExit(f"symbol manifest has no shard for {configuration}")
        symbol_records[configuration] = load_records(os.path.join(args.symbol, shard["path"]))

    header_configuration = header_manifest.get("configurations", [{}])[0].get("id", "win-x64-server")
    header_shard = find_shard(header_manifest, header_configuration)
    if header_shard is None:
        raise SystemExit("header manifest has no shard")
    header_records = load_records(os.path.join(args.header, header_shard["path"]))

    header_declarations = {
        record["id"]: record for record in header_records if record.get("recordType") == "declaration"
    }
    header_variants = {
        record["declarationId"]: record
        for record in header_records
        if record.get("recordType") == "variant" and record.get("configuration") == header_configuration
    }
    header_members = {}
    for record in header_records:
        if record.get("recordType") == "member":
            header_members.setdefault(record.get("declarationId"), []).append(record)

    symbol_declarations = {
        record["id"]: record
        for records in symbol_records.values()
        for record in records
        if record.get("recordType") == "declaration"
    }

    added_declarations = 0
    copied_fields = 0
    copied_methods = 0
    copied_layouts = 0
    processed_base_classes = set()

    def ensure_header_class(class_id):
        nonlocal added_declarations, copied_fields, copied_layouts
        if class_id in processed_base_classes:
            return
        processed_base_classes.add(class_id)

        bases = []
        for configuration, records in symbol_records.items():
            for record in records:
                if record.get("recordType") == "variant" and record.get("declarationId") == class_id:
                    bases.extend(record.get("bases", []))

        if class_id not in symbol_declarations and class_id in header_declarations:
            symbol_records[configurations[0]].append(copy.deepcopy(header_declarations[class_id]))
            symbol_declarations[class_id] = header_declarations[class_id]
            added_declarations += 1

            header_variant = header_variants.get(class_id, {})
            layout = header_variant.get("layout")
            header_class_bases = layout.get("bases", []) if layout else []
            clean_layout = None
            if layout and (layout.get("fieldOffsets") or header_class_bases):
                clean_layout = {
                    "size": layout.get("size", 0),
                    "align": layout.get("align", 0),
                    "fieldOffsets": layout.get("fieldOffsets", []),
                    "bases": [],
                }

            for configuration in configurations:
                has_variant = any(
                    record.get("recordType") == "variant"
                    and record.get("declarationId") == class_id
                    and record.get("configuration") == configuration
                    for record in symbol_records[configuration]
                )
                if not has_variant:
                    variant = {
                        "recordType": "variant",
                        "declarationId": class_id,
                        "configuration": configuration,
                        "availability": "present",
                    }
                    if clean_layout is not None:
                        variant["layout"] = copy.deepcopy(clean_layout)
                        copied_layouts += 1
                    if header_class_bases:
                        variant["bases"] = copy.deepcopy(header_class_bases)
                        bases.extend(header_class_bases)
                    symbol_records[configuration].append(variant)

                for header_member in header_members.get(class_id, []):
                    if header_member.get("kind") not in ("field", "static_field"):
                        continue
                    if any(
                        record.get("recordType") == "member"
                        and record.get("id") == header_member.get("id")
                        and record.get("configuration") == configuration
                        for record in symbol_records[configuration]
                    ):
                        continue
                    member = copy.deepcopy(header_member)
                    member["configuration"] = configuration
                    symbol_records[configuration].append(member)
                    copied_fields += 1

        for base in bases:
            ensure_header_class(base.get("declarationId"))

    for class_name in classes:
        declaration_id = "bedrock:" + class_name
        symbol_declaration = symbol_declarations.get(declaration_id)
        header_declaration = header_declarations.get(declaration_id)
        if symbol_declaration is None or header_declaration is None:
            continue

        symbol_declaration["abstract"] = header_declaration.get("abstract", symbol_declaration.get("abstract", False))
        symbol_declaration["copyConstructible"] = header_declaration.get(
            "copyConstructible", symbol_declaration.get("copyConstructible", False)
        )

        header_variant = header_variants.get(declaration_id, {})
        layout = header_variant.get("layout")
        header_bases = layout.get("bases", []) if layout else []
        usable_layout = None
        if layout and (layout.get("fieldOffsets") or header_bases):
            usable_layout = {
                "size": layout.get("size", 0),
                "align": layout.get("align", 0),
                "fieldOffsets": layout.get("fieldOffsets", []),
                "bases": [],
            }

        if usable_layout is not None:
            for configuration, records in symbol_records.items():
                for record in records:
                    if (
                        record.get("recordType") == "variant"
                        and record.get("declarationId") == declaration_id
                        and record.get("configuration") == configuration
                    ):
                        record["layout"] = copy.deepcopy(usable_layout)
                        copied_layouts += 1

        if header_bases:
            for configuration, records in symbol_records.items():
                for record in records:
                    if (
                        record.get("recordType") == "variant"
                        and record.get("declarationId") == declaration_id
                        and record.get("configuration") == configuration
                    ):
                        existing = record.setdefault("bases", [])
                        known = {base.get("declarationId") for base in existing}
                        for base in header_bases:
                            if base.get("declarationId") not in known:
                                existing.append(copy.deepcopy(base))
                                known.add(base.get("declarationId"))

        existing_methods = {
            normalize_signature(record.get("id", ""))
            for records in symbol_records.values()
            for record in records
            if record.get("recordType") == "member" and record.get("declarationId") == declaration_id
        }
        existing_keys = {
            (record.get("id"), configuration)
            for configuration, records in symbol_records.items()
            for record in records
        }

        for header_member in header_members.get(declaration_id, []):
            kind = header_member.get("kind")
            if kind in ("field", "static_field"):
                for configuration in configurations:
                    member = copy.deepcopy(header_member)
                    member["configuration"] = configuration
                    key = (member.get("id"), configuration)
                    if key not in existing_keys:
                        symbol_records[configuration].append(member)
                        existing_keys.add(key)
                        copied_fields += 1
                continue

            if kind not in ("method", "constructor", "destructor", "operator"):
                continue
            identifier = header_member.get("id", "")
            if normalize_signature(identifier) in existing_methods:
                continue
            for configuration in configurations:
                member = copy.deepcopy(header_member)
                member["configuration"] = configuration
                member["inline"] = True
                member["defined"] = True
                key = (member.get("id"), configuration)
                if key not in existing_keys:
                    symbol_records[configuration].append(member)
                    existing_keys.add(key)
                    copied_methods += 1

    base_class_ids = [
        base.get("declarationId")
        for records in symbol_records.values()
        for record in records
        if record.get("recordType") == "variant"
        for base in record.get("bases", [])
    ]
    for base_class_id in base_class_ids:
        if base_class_id:
            ensure_header_class(base_class_id)

    os.makedirs(args.output, exist_ok=True)
    shard_paths = {}
    for configuration in configurations:
        text = serialize(symbol_records[configuration])
        path = f"{configuration}.jsonl"
        with open(os.path.join(args.output, path), "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        shard_paths[configuration] = (path, text)

    declarations = 0
    variants = 0
    members = 0
    for records in symbol_records.values():
        declarations += sum(1 for record in records if record.get("recordType") == "declaration")
        variants += sum(1 for record in records if record.get("recordType") == "variant")
        members += sum(1 for record in records if record.get("recordType") == "member")
    # Declarations are only emitted in the first configuration shard.
    declarations = sum(
        1 for record in symbol_records[configurations[0]] if record.get("recordType") == "declaration"
    )

    for shard in symbol_manifest.get("shards", []):
        configuration = shard.get("configuration")
        if configuration in shard_paths:
            path, text = shard_paths[configuration]
            shard["path"] = path
            shard["records"] = sum(1 for record in symbol_records[configuration])
            shard["sha256"] = sha256_text(text)
            shard["sha256Compressed"] = sha256_text(text)

    symbol_manifest["counts"] = {
        "declarations": declarations,
        "variants": variants,
        "members": members,
    }
    with open(os.path.join(args.output, "ir-manifest.json"), "w", encoding="utf-8", newline="\n") as stream:
        json.dump(symbol_manifest, stream, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        stream.write("\n")

    print(
        f"merged: layouts={copied_layouts} fields={copied_fields} methods={copied_methods} "
        f"declarations+={added_declarations}"
    )


if __name__ == "__main__":
    main()
