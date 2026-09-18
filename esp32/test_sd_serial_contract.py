"""Static security and resource contracts for read-only SD serial diagnostics."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SOURCE = Path(__file__).with_name("src").joinpath("main.cpp")
STORAGE = Path(__file__).with_name("src").joinpath("sd_storage.cpp")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    opening = source.find("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    storage = STORAGE.read_text(encoding="utf-8")
    failures: list[str] = []

    try:
        parser = function_body(source, "void handleWatchProbeSerial(")
        status = function_body(source, "void handleSdSerialStatus(")
        listing = function_body(source, "void handleSdSerialList(")
        head = function_body(source, "void handleSdSerialHead(")
        resolver = function_body(storage, "bool SdStorageService::resolvePath(")
    except AssertionError as error:
        print(f"SD_SERIAL_CONTRACT_FAIL\n- {error}")
        return 1

    for token in ("SD_STATUS", "SD_LIST", "SD_HEAD"):
        if token not in parser:
            failures.append(f"missing serial command: {token}")
    for token in ("openWrite", "removeFile", "SD_WRITE", "SD_DELETE", "SD_FORMAT", "FILE_WRITE"):
        if token in parser + status + listing + head:
            failures.append(f"serial diagnostics are not read-only: {token}")

    if "sdStorage.ensureMounted" not in status:
        failures.append("status must lazily mount through SdStorageService")
    if "sdStorage.openRead" not in listing or "sdStorage.openRead" not in head:
        failures.append("list/head must use root-confined openRead")
    if "spi_(HSPI)" not in storage or "SD.begin(csPin_, spi_, kSdFrequencyHz)" not in storage:
        failures.append("SD diagnostics must use the isolated HSPI host")
    if "kSerialSdListLimit = 48" not in source:
        failures.append("directory listing must remain bounded to 48 entries")
    if "kSerialSdHeadMaxBytes = 128" not in source:
        failures.append("file preview must remain bounded to 128 bytes")
    if 'document["encoding"] = "base64"' not in head:
        failures.append("file preview must use a log-safe encoding")
    if "serializeJson(document, Serial)" not in source:
        failures.append("serial responses must use JSON escaping")

    for guard in (
        'path.startsWith("/")',
        "path.indexOf('\\\\')",
        "path.indexOf(':')",
        "kMaxRelativePathLength",
    ):
        if guard not in resolver:
            failures.append(f"missing path confinement guard: {guard}")
    for guard in ('segment == "."', 'segment == ".."'):
        if guard not in storage:
            failures.append(f"missing path segment guard: {guard}")

    if not re.search(r"command\.length\(\) < 256", parser):
        failures.append("serial command buffer must remain bounded")

    if failures:
        print("SD_SERIAL_CONTRACT_FAIL")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print("SD_SERIAL_CONTRACT_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
