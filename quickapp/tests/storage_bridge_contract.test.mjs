import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const bridge = await readFile(join(root, 'src/common/esp32_bridge.js'), 'utf8')

for (const name of ['storageStatus', 'storageList', 'storageChunk']) {
  assert.equal(bridge.includes(`export function ${name}`), true, `${name} export missing`)
}
for (const path of [
  '/api/v1/storage/status',
  '/api/v1/storage/list?path=',
  '/api/v1/storage/chunk'
]) {
  assert.equal(bridge.includes(`'${path}'`) || bridge.includes(`'${path} +`), true, `missing storage route: ${path}`)
}
assert.equal(bridge.includes("'X-Hoshino-Storage': '1'"), true)
assert.equal(bridge.includes("header['X-Hoshino-Watch-Setup'] = '1'"), true)
assert.equal(bridge.includes('encodeURIComponent(safePath)'), true)
assert.equal(bridge.includes('STORAGE_CHUNK_DEFAULT_LENGTH = 1024'), true)
assert.equal(bridge.includes('STORAGE_CHUNK_MAX_LENGTH = 2048'), true)
assert.equal(bridge.includes('Math.min(STORAGE_CHUNK_MAX_LENGTH, requestedLength)'), true)
assert.equal(bridge.includes("data.encoding === 'base64'"), true)
console.log('storage bridge contract: PASS')
