# Remove ASR and Audit QuickApp Microphone Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the ESP32 XiaoAI/ASR pipeline from the default firmware build and determine whether Vela QuickApps can directly use a microphone.

**Architecture:** Compile the ASR subsystem out at preprocessing time so its Opus dependency, task stack, queue, temporary audio handling, UI configuration, and runtime calls are absent. Preserve SPP authentication, NAPT, SD storage, and QuickApp messaging. Treat the official WeChat package and official documentation as separate evidence sources for microphone support.

**Tech Stack:** ESP32 Arduino/ESP-IDF, PlatformIO, Python source-contract tests, Vela QuickApp package analysis.

## Global Constraints

- Default build must have `HOSHINO_ENABLE_ASR=0`.
- No ASR task, Opus decoder, MiMo audio upload, or XiaoAI controls may be reachable in the default build.
- Existing authentication and QuickApp SPP bridge behavior must remain covered by `test_auth_production_path.py`.
- No secrets are logged or added to source.

---

### Task 1: Add an ASR-disabled source contract

**Files:**
- Create: `test_asr_disabled.py`
- Modify: `src/main.cpp`

- [ ] Write a test that fails until the default source defines `HOSHINO_ENABLE_ASR` as `0` and compiles the Opus include only behind that flag.
- [ ] Run `python test_asr_disabled.py` and confirm the expected failure.
- [ ] Add the default-off macro and preprocessor boundary.
- [ ] Re-run `python test_asr_disabled.py` and confirm it passes.

### Task 2: Remove the runtime ASR path

**Files:**
- Modify: `src/main.cpp`
- Test: `test_asr_disabled.py`, `test_auth_production_path.py`

- [ ] Compile out ASR globals, audio capture/Opus/MiMo functions, and runtime service calls.
- [ ] Preserve the non-ASR SPP/NAPT and QuickApp code paths.
- [ ] Run both Python source-contract tests.

### Task 3: Remove ASR configuration and dependency

**Files:**
- Modify: `src/main.cpp`, `platformio.ini`, `README.md`
- Test: `test_asr_disabled.py`

- [ ] Remove ASR UI inputs, persisted ASR configuration, status fields, and the `esp32_opus` dependency.
- [ ] Update README descriptions so they no longer claim XiaoAI/ASR support.
- [ ] Re-run tests and an ESP32 build if the configured toolchain is available.

### Task 4: Audit QuickApp microphone capability

**Files:**
- Inspect: extracted official WeChat package and official Vela documentation.

- [ ] Check the WeChat manifest for microphone permissions and QuickApp code for a direct microphone API.
- [ ] Check official Vela documentation for a public microphone/record API and record the scope/version caveat.
- [ ] Report evidence separately from inference; do not add a microphone feature without an actual supported API.
