import fetch from '@system.fetch'

// The watch-side tunnel always exposes the ESP32 at this private gateway.
// Keep this module deliberately narrow: these are the only routes used by the
// single-page Wi-Fi provisioning app.
const WATCH_SETUP_BASE_URL = 'http://10.1.10.1'
const MAX_RESPONSE_CHARS = 64 * 1024
const REQUEST_TIMEOUT_MS = 8000
const MAX_RETRIES = 1
const WIFI_SCAN_RETRY_BUDGET_MS = 15000
const WIFI_SCAN_RETRY_DEFAULT_MS = 1000
const STORAGE_CHUNK_DEFAULT_LENGTH = 1024
const STORAGE_CHUNK_MAX_LENGTH = 2048
let generation = 1
let scanGeneration = 1

export function cancelAll() {
  generation += 1
  scanGeneration += 1
}

function request(path, method, body, callbacks, retries, requestHeaders) {
  const done = callbacks || {}
  const requestGeneration = generation
  const retryLimit = Math.max(0, Math.min(MAX_RETRIES, Number(retries) || 0))
  let attempt = 0
  let finished = false
  let timer = null

  function fail(data, code) {
    if (finished || requestGeneration !== generation) return
    finished = true
    if (timer) clearTimeout(timer)
    if (done.fail) done.fail(data, code)
  }

  function succeed(data, code) {
    if (finished || requestGeneration !== generation) return
    finished = true
    if (timer) clearTimeout(timer)
    if (done.success) done.success(data, code)
  }

  function run() {
    if (finished || requestGeneration !== generation) return
    attempt += 1
    if (timer) clearTimeout(timer)
    timer = setTimeout(() => {
      if (finished || requestGeneration !== generation) return
      if (attempt <= retryLimit) return run()
      fail('timeout', 0)
    }, REQUEST_TIMEOUT_MS)

    const header = { Accept: 'application/json' }
    if (requestHeaders) {
      for (const key in requestHeaders) header[key] = requestHeaders[key]
    } else {
      header['X-Hoshino-Watch-Setup'] = '1'
    }
    if (body !== undefined && body !== null) header['Content-Type'] = 'application/json'

    fetch.fetch({
      url: WATCH_SETUP_BASE_URL + path,
      method,
      header,
      data: body === undefined || body === null ? undefined : JSON.stringify(body),
      responseType: 'text',
      success: (response) => {
        if (finished || requestGeneration !== generation) return
        if (timer) clearTimeout(timer)
        const code = Number(response && response.code)
        const text = String(response && response.data !== undefined ? response.data : '')
        if (text.length > MAX_RESPONSE_CHARS) return fail('response_too_large', code)

        let parsed = {}
        try {
          parsed = text ? JSON.parse(text) : {}
        } catch (e) {
          return fail('invalid_json', code)
        }
        if (code < 200 || code >= 300) return fail(parsed, code)
        succeed(parsed, code)
      },
      fail: (data, code) => {
        if (finished || requestGeneration !== generation) return
        if (timer) clearTimeout(timer)
        if (attempt <= retryLimit) return run()
        fail(data, code)
      }
    })
  }

  run()
}

export function watchSetupStatus(callbacks) {
  request('/watch-setup/status', 'GET', null, callbacks, 1)
}

export function watchSetupScan(callbacks) {
  const done = callbacks || {}
  const requestScanGeneration = scanGeneration
  const deadline = Date.now() + WIFI_SCAN_RETRY_BUDGET_MS
  let retryTimer = null
  let finished = false

  function isActive() {
    return !finished && requestScanGeneration === scanGeneration
  }

  function fail(data, code) {
    if (!isActive()) return
    finished = true
    if (retryTimer) clearTimeout(retryTimer)
    if (done.fail) done.fail(data, code)
  }

  function scheduleRetry(data, code) {
    const remaining = deadline - Date.now()
    if (remaining <= 0) {
      return fail({
        ok: false,
        status: 'wifi_connect_timeout',
        error: 'wifi_connect_timeout',
        message: 'Wi-Fi 连接超时，请稍后重试'
      }, code)
    }
    const requestedDelay = Number(data && data.retryAfterMs)
    const delay = Math.max(250, Math.min(2000, Number.isFinite(requestedDelay) ? requestedDelay : WIFI_SCAN_RETRY_DEFAULT_MS, remaining))
    retryTimer = setTimeout(() => {
      retryTimer = null
      scan()
    }, delay)
  }

  function handle(data, code) {
    if (!isActive()) return
    if (data && data.status === 'wifi_connecting') return scheduleRetry(data, code)
    finished = true
    if (done.success) done.success(data, code)
  }

  function scan() {
    if (!isActive()) return
    request('/watch-setup/scan', 'GET', null, {
      success: handle,
      fail: (data, code) => {
        if (data && data.status === 'wifi_connecting') return scheduleRetry(data, code)
        fail(data, code)
      }
    }, 0)
  }

  scan()
}

export function watchSetupWifi(ssid, wifiPassword, callbacks) {
  // Preserve SSID/password code units exactly; ESP32 validates byte length and
  // passes the received UTF-8 bytes to WiFi.begin without trimming.
  const safeSsid = String(ssid === undefined || ssid === null ? '' : ssid)
  const safePassword = String(wifiPassword === undefined || wifiPassword === null ? '' : wifiPassword)
  request('/watch-setup/wifi', 'POST', {
    ssid: safeSsid,
    wifiPassword: safePassword
  }, callbacks, 0)
}

function storageCallbacks(callbacks, validate) {
  const done = callbacks || {}
  return {
    success: (data, code) => {
      if (!data || data.ok !== true || (validate && !validate(data))) {
        if (done.fail) done.fail(data || 'invalid_storage_response', code)
        return
      }
      if (done.success) done.success(data, code)
    },
    fail: done.fail
  }
}

function safeStoragePath(path) {
  return String(path === undefined || path === null ? '' : path)
}

export function storageStatus(callbacks) {
  request('/api/v1/storage/status', 'GET', null,
    storageCallbacks(callbacks, (data) => data.mounted === true), 0,
    { 'X-Hoshino-Storage': '1' })
}

export function storageList(path, callbacks) {
  const safePath = safeStoragePath(path)
  request('/api/v1/storage/list?path=' + encodeURIComponent(safePath), 'GET', null,
    storageCallbacks(callbacks, (data) => Array.isArray(data.items)), 0,
    { 'X-Hoshino-Storage': '1' })
}

export function storageChunk(path, offset, length, callbacks) {
  const safePath = safeStoragePath(path)
  const safeOffset = Math.max(0, Math.floor(Number(offset) || 0))
  const requestedLength = length === undefined || length === null
    ? STORAGE_CHUNK_DEFAULT_LENGTH
    : Math.floor(Number(length) || STORAGE_CHUNK_DEFAULT_LENGTH)
  const safeLength = Math.max(1, Math.min(STORAGE_CHUNK_MAX_LENGTH, requestedLength))
  const query = '?path=' + encodeURIComponent(safePath) +
    '&offset=' + safeOffset + '&length=' + safeLength
  request('/api/v1/storage/chunk' + query, 'GET', null,
    storageCallbacks(callbacks, (data) => data.encoding === 'base64' && typeof data.data === 'string'), 0,
    { 'X-Hoshino-Storage': '1' })
}
