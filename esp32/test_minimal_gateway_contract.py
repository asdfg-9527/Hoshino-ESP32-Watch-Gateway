"""Static contracts for the minimal Watch gateway surface.

These checks intentionally stay source-level: the ESP32 transport has no host
test seam, while the route/serial/startup boundaries and protected transport
functions can be frozen without a device or credentials.
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path


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


# SHA-256 of the function bodies in pre_simplify_20260916_212350/esp32_source.zip.
# No credentials or protocol secrets are included in these constants.
PROTECTED_BODY_SHA256 = {
    "bool beginWatchBluetooth(": "b7776efefa8ecce54c24502562e0f83c67c501472642978a564b4d39a9b878af",
    "bool connectWatchWithSdp(": "049c60e4bf7fbca50d6ffca1dcedd58aca4ba0ca5d3b78f248022f6e6eab6159",
    "bool startSppV2Session(": "6cf9de1bcf5fb94837345c7fb68316c1bc6c5d0b04878fd475ef20ee60898943",
    "bool readSppV2Frame(": "651952769a07a4563cca668254908c7198ebb8b011d6d7841715275e9909625e",
    "bool writeSppV2Frame(": "d0325d096cec4ab422348c938b1360e0c3467692b52a2eb40eb1ab8f741bc030",
    "bool setupWatchNetworkProxy(": "5d35126dd121cb5fcbc155704975abf0220a4b34a48f2ae4d44d05966c72f3af",
    "bool handleWatchDhcp(": "bd379ee058b63fec31a89fe8f76d6cfece968e9aa6b99b4919569451a37d8298",
    "bool injectWatchNetworkPacket(": "6549558ba4b82f338b0392e4ced5afc632543bda423270d682d61255e1f472f3",
}


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    failures: list[str] = []

    expect("kWebUi" not in source, "retired full control page constant remains", failures)
    for forbidden in ("/setup/test", "chatHandler", "startWatch(",
                      "stopWatch(", "resetContext(", "测试 MiMo Chat"):
        expect(forbidden not in source, f"retired AI/control page token remains: {forbidden}", failures)

    minimal_start = source.find("const char kMinimalSetupWebUi[]")
    minimal_end = source.find(")HTML\";", minimal_start)
    expect(minimal_start >= 0 and minimal_end > minimal_start,
           "minimal phone setup page is missing", failures)
    minimal_ui = source[minimal_start:minimal_end] if minimal_start >= 0 and minimal_end > minimal_start else ""
    minimal_fetch_paths = re.findall(r"fetch\(['\"]([^'\"]+)['\"]", minimal_ui)
    expect(minimal_fetch_paths == ["/setup/status", "/setup"],
           f"minimal phone page must only call setup endpoints: {minimal_fetch_paths!r}", failures)

    try:
        routes = function_body(source, "void setupRoutes(")
        setup_status = function_body(source, "void handlePhoneSetupStatus(")
        setup_save = function_body(source, "void handlePhoneSetupSave(")
        setup_mode = function_body(source, "void enterSetupMode(")
        connect_wifi = function_body(source, "void connectWifi(")
        serial = function_body(source, "void handleWatchProbeSerial(")
        scan = function_body(source, "void handleWatchSetupScan(")
        watch_status = function_body(source, "void handleWatchSetupStatus(")
        watch_wifi = function_body(source, "void handleWatchSetupWifi(")
        auth = function_body(source, "void authenticateWatchSpp(")
        setup = function_body(source, "void setup(")
        loop = function_body(source, "void loop(")
    except AssertionError as error:
        print(f"MINIMAL_GATEWAY_CONTRACT_FAIL\n- {error}")
        return 1

    route_paths = re.findall(r'server\.on\("([^\"]+)"\s*,', routes)
    expect(
        route_paths == ["/", "/setup/status", "/setup", "/watch-setup/status", "/watch-setup/scan", "/watch-setup/wifi",
                        "/api/v1/storage/status", "/api/v1/storage/list", "/api/v1/storage/file", "/api/v1/storage/chunk"],
        f"setupRoutes paths changed: {route_paths!r}",
        failures,
    )
    for forbidden in ("/setup/test", "chatHandler"):
        expect(forbidden not in routes, f"forbidden setup route token remains: {forbidden}", failures)
    expect("kMinimalSetupWebUi" in routes, "root must serve the minimal phone setup HTML", failures)
    expect("handlePhoneSetupStatus" in routes and "handlePhoneSetupSave" in routes,
           "phone setup routes must call the bounded AP handlers", failures)
    expect("handleStorageStatus" in routes and "handleStorageList" in routes and
           "handleStorageFile" in routes and "handleStorageChunk" in routes,
           "versioned storage routes must be registered", failures)
    expect("if (gSetupMode)" in routes and 'server.sendHeader("Location", "/", true)' in routes,
           "setup-mode unknown paths must redirect to the setup page", failures)
    expect("isSetupApClient" in setup_status, "setup status must be restricted to the SoftAP client", failures)
    expect("requireSetupAp" in setup_save, "setup save must be restricted to the SoftAP client", failures)
    expect("requireWatchSetupClient" in watch_status, "watch status must remain tunnel-only", failures)
    expect("requireWatchSetupClient" in scan, "watch scan must remain tunnel-only", failures)
    expect("gWifiStaConnecting" in source, "STA scan guard must use an explicit connection marker", failures)
    expect("WATCH_WIFI_SCAN_DEFER reason=sta_connecting" in scan,
           "scan must defer while the explicit STA connection operation is active", failures)
    expect("retryAfterMs" in scan and 'status"] = "wifi_connecting"' in scan,
           "STA connecting response must be bounded and JSON-compatible", failures)
    expect("const wifi_mode_t modeBeforeScan = WiFi.getMode();" in scan,
           "scan must inspect the current Wi-Fi mode before enabling STA", failures)
    expect("const wifi_mode_t scanMode = (modeBeforeScan & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA;" in scan,
           "scan must preserve AP by using APSTA", failures)
    expect("WiFi.disconnect" not in source, "scan fix must not disconnect Wi-Fi", failures)
    expect("requireWatchSetupClient" in watch_wifi, "watch Wi-Fi save must remain tunnel-only", failures)
    expect('"invalid_password"' in watch_wifi and "nextPass.length() < 8" in watch_wifi,
           "watch Wi-Fi must reject empty and 1..7-byte passwords before NVS/WiFi.begin", failures)
    expect('"invalid_ssid"' in watch_wifi and "nextSsid.isEmpty()" in watch_wifi,
           "watch Wi-Fi must reject only a truly empty SSID", failures)
    expect("gPendingWifiSsid = nextSsid" in watch_wifi and
           "WiFi.begin(gPendingWifiSsid.c_str(), gPendingWifiPassword.c_str())" in watch_wifi,
           "STA must receive the exact untrimmed pending SSID/password", failures)
    expect("prefs.putString" not in watch_wifi and "ESP.restart" not in watch_wifi,
           "watch Wi-Fi POST must not persist/reboot before connection success", failures)
    expect("gWifiProvisioningPending" in source and "finishPendingWifiProvisioning(true)" in source,
           "credentials must persist only from the connected state", failures)
    expect("WiFi.onEvent(handleWifiEvent);" in setup,
           "Wi-Fi disconnect events must be captured by the application", failures)
    for reason in ("WIFI_REASON_AUTH_FAIL", "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT",
                   "WIFI_REASON_HANDSHAKE_TIMEOUT", "WIFI_REASON_NO_AP_FOUND"):
        expect(reason in source, f"missing IDF Wi-Fi reason mapping: {reason}", failures)
    for error in ("wrong_password", "network_not_found", "connect_timeout", "connect_failed"):
        expect(error in source, f"missing Wi-Fi error status: {error}", failures)
    expect("WiFi.setAutoReconnect(false)" in source,
           "failed/timeout STA attempt must stop automatic reconnect", failures)

    command_branches = re.findall(r'command\.(?:startsWith|equals)\([^\)]*\)', serial)
    command_literals = re.findall(r'command\s*==\s*"([^"]+)"', serial)
    expect(
        command_literals == ["WATCH_SETUP", "SD_STATUS", "SD_LIST", "SD_HEAD"],
        f"unexpected exact serial commands: {command_literals!r}",
        failures,
    )
    expect(
        command_branches == ['command.startsWith("WATCH_CONFIG ")', 'command.startsWith("SD_LIST ")', 'command.startsWith("SD_HEAD ")'],
        f"unexpected prefix serial commands: {command_branches!r}",
        failures,
    )
    for forbidden in ("WATCH_PROBE", "WATCH_SDP", "WATCH_SPP", "WATCH_VERSION", "WATCH_SESSION",
                      "WATCH_RAW_TRACE", "WATCH_DECRYPT", "ESP_DNS", "WATCH_AUTH ", "WATCH_BRIDGE ",
                      "WATCH_BENCH", "WATCH_STREAM", "WATCH_FINAL_ACCEPTANCE"):
        expect(forbidden not in serial, f"forbidden serial command remains: {forbidden}", failures)

    expect("handleSdSerialStatus" in serial, "SD_STATUS must call the bounded status handler", failures)
    expect("handleSdSerialList" in serial, "SD_LIST must call the bounded list handler", failures)
    expect("handleSdSerialHead" in serial, "SD_HEAD must call the bounded head handler", failures)
    for forbidden in ("openWrite", "removeFile", "SD_WRITE", "SD_DELETE", "SD_FORMAT"):
        expect(forbidden not in serial,
               f"serial command parser must stay read-only: {forbidden}", failures)

    need_setup = re.search(r"const bool needSetup\s*=\s*([^;]+);", setup)
    expect(need_setup is not None, "missing needSetup declaration", failures)
    if need_setup:
        expect("ssid.isEmpty" not in need_setup.group(1), "needSetup still depends on empty SSID", failures)
    expect(
        "if (!gServerStarted && (gSoftApActive || gWatchNetworkReady || WiFi.status() == WL_CONNECTED))" in loop,
        "HTTP startup must accept an active SoftAP as well as bridge/STA readiness",
        failures,
    )
    expect("gSoftApActive = apStarted;" in setup_mode, "setup-mode AP state must be recorded", failures)
    expect("gSoftApActive = apStarted;" in connect_wifi, "normal AP state must be recorded", failures)

    expect("constexpr size_t kResultLimit = 20;" in scan, "scan limit must remain 20", failures)
    expect("selectedSsids[j] == networkName" in scan, "scan must deduplicate SSIDs", failures)
    expect("if (rssi > selectedRssi[j]) selectedRssi[j] = rssi;" in scan, "scan must retain strongest duplicate RSSI", failures)
    expect("if (rssi > selectedRssi[weakest])" in scan, "scan overflow must retain stronger APs", failures)
    expect("beginWifiStaConnection(kWifiStaConnectTimeoutMs);" in auth,
           "nonblocking STA begin must set the explicit connection marker", failures)
    expect("serviceWifiStaConnectionState(gWatchBridgeStopRequested);" in auth,
           "auth hold must service STA success/failure/timeout/cancel endpoints", failures)
    expect("clearWifiStaConnection(gWatchBridgeStopRequested ? \"cancelled\" : \"spp_session_end\");" in auth,
           "auth hold teardown must clear the STA connection marker", failures)
    scan_call_index = scan.find("WiFi.scanNetworks(false, true)")
    delete_index = scan.find("WiFi.scanDelete();", scan_call_index)
    json_index = scan.find("JsonDocument d;", scan_call_index)
    expect(delete_index >= 0 and json_index >= 0 and delete_index < json_index,
           "scanDelete must precede JSON response construction", failures)
    for field in ("rawCount", "truncated", 'd["networks"]'):
        expect(field in scan, f"scan response field missing: {field}", failures)

    expect("kRound4CompatibleDeviceInfo" in auth, "auth must use the current-session DeviceInfo override", failures)
    expect("sessionDeviceInfoLength" in auth and "sessionDeviceInfo, sessionDeviceInfoLength" in auth,
           "auth DeviceInfo override must be passed to buildStep3", failures)
    expect("bool haveType23" in auth and "if (haveType23)" in auth,
           "type23 must use the non-fatal haveType23 path", failures)
    expect("WATCH_ROUND4_TEMPLATE_DECRYPT_FAILED" not in auth,
           "historical template failure must not directly break authentication", failures)
    expect("WATCH_R4W_23/3 SKIPPED" in auth, "type23 skip path must preserve later bootstrap", failures)

    for signature, expected in PROTECTED_BODY_SHA256.items():
        try:
            body = function_body(source, signature)
        except AssertionError as error:
            failures.append(str(error))
            continue
        actual = hashlib.sha256(body.encode("utf-8")).hexdigest()
        expect(actual == expected, f"protected function changed: {signature} ({actual})", failures)

    if failures:
        print("MINIMAL_GATEWAY_CONTRACT_FAIL")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print("MINIMAL_GATEWAY_CONTRACT_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
