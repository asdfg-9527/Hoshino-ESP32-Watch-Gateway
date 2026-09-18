import assert from 'node:assert/strict'
import { readFile, readdir } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const read = (name) => readFile(join(root, name), 'utf8')

const manifest = JSON.parse(await read('src/manifest.json'))
const bridge = await read('src/common/esp32_bridge.js')
const page = await read('src/pages/wifi_setup/wifi_setup.ux')
const inputMethod = await read('src/components/InputMethod/InputMethod.ux')
const inputMethodLicense = await read('src/components/InputMethod/LICENSE')
const inputMethodNotice = await read('src/components/InputMethod/UPSTREAM.md')
const app = await read('src/app.ux')

assert.equal(manifest.router.entry, 'pages/wifi_setup')
assert.deepEqual(Object.keys(manifest.router.pages), ['pages/wifi_setup'])
assert.deepEqual(manifest.features.map((feature) => feature.name), ['system.fetch'])
assert.equal(app.includes('@system.file'), false)
assert.equal(app.includes('@system.audio'), false)

for (const path of ['/watch-setup/status', '/watch-setup/scan', '/watch-setup/wifi']) {
  assert.equal(bridge.includes("WATCH_SETUP_BASE_URL + path"), true)
  assert.equal(bridge.includes(`'${path}'`), true)
}
assert.equal(bridge.includes("'http://10.1.10.1'"), true)
assert.equal(bridge.includes("'X-Hoshino-Watch-Setup': '1'"), true)
assert.equal(bridge.includes("'/api/v1/"), false)
assert.equal(bridge.includes('wifiPassword'), true)
assert.equal(bridge.includes('WIFI_SCAN_RETRY_BUDGET_MS = 15000'), true)
assert.equal(bridge.includes("data.status === 'wifi_connecting'"), true)
assert.equal(bridge.includes('retryAfterMs'), true)
assert.equal(bridge.includes('scanGeneration'), true)
assert.equal(bridge.includes('Preserve SSID/password code units exactly'), true)
assert.equal(page.includes("if (this.scanInProgress) return"), true)
assert.equal(page.includes("data.status === 'wifi_connecting'"), true)
assert.equal(page.includes('this.password.length < 8'), true)
assert.equal(page.includes('pollWifiProvisioning(Date.now() + 20000)'), true)
assert.equal(page.includes("state === 'connected' && data && data.wifiConnected"), true)
for (const message of ['Wi-Fi 密码错误，请重新输入', '未找到该 Wi-Fi', 'Wi-Fi 连接超时，请稍后重试']) {
  assert.equal(page.includes(message), true, `missing Wi-Fi error mapping: ${message}`)
}
assert.equal(manifest.versionName, '2.0.14')
assert.equal(manifest.versionCode, 17)

for (const marker of [
  'watchSetupStatus',
  'watchSetupScan',
  'watchSetupWifi',
  'MAX_WIFI_NETWORKS = 20',
  'nextNetworks.slice(0, MAX_WIFI_NETWORKS)',
  '@click="selectWifi($idx)"',
  '@click="submitWifi"',
  '../../components/InputMethod/InputMethod.ux',
  'screentype="rect"',
  '@key-down="onPasswordKeyDown"',
  '@delete="onPasswordDelete"',
  '@complete="onPasswordComplete"',
  'watchSetupWifi(this.selectedSsid, this.password',
  'maskedPassword',
  'passwordKeyboardHidden'
]) {
  assert.equal(page.includes(marker), true, `missing page contract: ${marker}`)
}

assert.equal(page.includes('type="password"'), false)
assert.equal(page.includes('onPasswordChange'), false)
assert.equal(inputMethod.includes("screentype: { default: 'rect' }"), true)
assert.equal(inputMethod.includes("keyboardtype: { default: 'QWERTY' }"), true)
assert.equal(inputMethod.includes("this.$emit('keyDown', { content })"), true)
assert.equal(inputMethod.includes("this.$emit('complete', { content:"), true)
assert.equal(inputMethod.includes("this.$emit('delete', {})"), true)
assert.equal(inputMethod.includes("@click=\"completeInput\""), true)
assert.equal(inputMethod.includes("screentype==='pill-shaped'"), false)
assert.equal(inputMethod.includes("keyboardtype=='T9'"), false)
assert.equal(inputMethod.includes('@system.file'), false)
assert.equal(inputMethod.includes('@system.vibrator'), false)
assert.equal(inputMethod.includes('dictionary'), false)
assert.equal(inputMethod.includes('cn'), false)
assert.equal(inputMethod.includes('jp'), false)
assert.equal(inputMethodLicense.includes('Copyright (c) 2024 NEORUAA'), true)
assert.equal(inputMethodNotice.includes('462bc948c78ed07eb48361d693cbca1cd6b5c9bf'), true)

const componentEntries = await readdir(join(root, 'src/components/InputMethod'))
assert.deepEqual(componentEntries.sort(), ['InputMethod.ux', 'LICENSE', 'UPSTREAM.md'])

assert.equal(page.includes('../../common/protocol'), false)
assert.equal(page.includes('../../common/interconnect'), false)
console.log('wifi_setup contract: PASS')
