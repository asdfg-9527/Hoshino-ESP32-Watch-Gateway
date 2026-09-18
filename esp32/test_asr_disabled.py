"""Source contract for the default low-memory Interconnect firmware build."""

from pathlib import Path
import re
import sys


SOURCE = Path(__file__).with_name("src").joinpath("main.cpp")


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    failures: list[str] = []

    if not re.search(r"^#define\s+HOSHINO_ENABLE_ASR\s+0\s*$", source, re.MULTILINE):
        failures.append("default firmware must define HOSHINO_ENABLE_ASR as 0")

    opus_include = source.find("#include <opus.h>")
    guard = source.rfind("#if HOSHINO_ENABLE_ASR", 0, opus_include)
    if opus_include < 0 or guard < 0:
        failures.append("Opus include must be compiled only when HOSHINO_ENABLE_ASR is enabled")

    if failures:
        print("ASR_DISABLED_CONTRACT_FAIL")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("ASR_DISABLED_CONTRACT_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
