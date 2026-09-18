"""Static contract guard for the production SPP authentication path.

The firmware has no host-side Bluetooth transport seam, so this test protects
the security-critical source contract that can be checked without a watch:
SPP Hello precedes SPPv2 startup and normal authentication never replays a
captured DeviceInfo transcript.
"""

from pathlib import Path
import re
import sys


SOURCE = Path(__file__).with_name("src").joinpath("main.cpp")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    while start >= 0:
        opening = source.find("{", start)
        semicolon = source.find(";", start)
        if opening >= 0 and (semicolon < 0 or opening < semicolon):
            break
        start = source.find(signature, start + len(signature))
    if start < 0:
        raise AssertionError(f"missing function definition: {signature}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


def expect(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    failures: list[str] = []
    expected_hello = (
        "constexpr uint8_t kSppHello[] = {\n"
        "    0xba, 0xdc, 0xfe, 0x00, 0xc0, 0x03, 0x00, 0x00, 0x01, 0x00, 0xef,\n"
        "};"
    )
    expect(expected_hello in source, "SPP Hello must be BA DC FE 00 C0 03 00 00 01 00 EF", failures)

    try:
        session_start = function_body(source, "bool startSppV2Session(")
    except AssertionError as error:
        failures.append(str(error))
        session_start = ""
    expect(
        session_start.find("watchBt.write(kSppHello") < session_start.find("watchBt.write(kSppV2SessionStartRequest"),
        "SPP Hello must be written before the SPPv2 Session Start request",
        failures,
    )

    for signature in ("void authenticateWatchSpp(", "void bridgeWatchQuickApp("):
        try:
            body = function_body(source, signature)
        except AssertionError as error:
            failures.append(str(error))
            continue
        expect("startSppV2Session(" in body, f"{signature} must use the shared SPP session initializer", failures)

    try:
        auth = function_body(source, "void authenticateWatchSpp(")
    except AssertionError:
        auth = ""
    expect("decryptRound4CapturedAuthDeviceInfo" not in auth, "production auth must not decrypt a historical capture", failures)
    expect("round4AuthDeviceInfo" not in auth, "production auth must not override DeviceInfo from a historical capture", failures)
    expect("WATCH_R4_AUTH_IDENTITY_REPLAY" not in auth, "production auth must not log a capture replay", failures)
    expect("private_capture_local_only" not in auth, "production auth must not advertise private-capture mode", failures)

    if failures:
        print("AUTH_PRODUCTION_PATH_FAIL")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print("AUTH_PRODUCTION_PATH_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
