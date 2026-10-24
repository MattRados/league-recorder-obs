// Fetches jansson 2.14, the JSON library OBS builds into its capture plugin (it reads the compatibility list
// with it), so scripts/build-native.mjs can build win-capture.dll: the src folder and
// licence of jansson's release archive, into out/jansson-src. Its two config headers, which jansson's own
// CMake build would write, are in native/win-capture/jansson.
//
//   node scripts/fetch-jansson.mjs                only if missing
//   node scripts/fetch-jansson.mjs --force        re-download
//   node scripts/fetch-jansson.mjs --tar <file>   from a copy of the same archive already on disk (checked the same way)

import { execFileSync } from 'child_process'
import { createHash } from 'crypto'
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'

const VERSION = '2.14'
const URL = `https://github.com/akheron/jansson/releases/download/v${VERSION}/jansson-${VERSION}.tar.bz2`
// Pinned after the first download (434,481 bytes, the size GitHub lists for the release file)
const SHA256 = 'fba956f27c6ae56ce6dfd52fbf9d20254aad42821f74fa52f83957625294afb9'

const target = join(import.meta.dirname, '..', 'out', 'jansson-src')
const local = process.argv.includes('--tar') ? process.argv[process.argv.indexOf('--tar') + 1] : null
if (existsSync(join(target, 'src', 'jansson.h')) && !process.argv.includes('--force') && !local) {
  console.log('jansson already present')
  process.exit(0)
}

let archive
if (local) {
  archive = readFileSync(local)
} else {
  console.log('Downloading', URL)
  const res = await fetch(URL)
  if (!res.ok) throw new Error(`${URL}: HTTP ${res.status}`)
  archive = Buffer.from(await res.arrayBuffer())
}
const actual = createHash('sha256').update(archive).digest('hex')
if (SHA256 && actual !== SHA256) throw new Error(`Checksum mismatch: expected ${SHA256}, got ${actual}`)
console.log('sha256', actual)

const work = mkdtempSync(join(tmpdir(), 'jansson-'))
try {
  const path = join(work, 'jansson.tar.bz2')
  writeFileSync(path, archive)
  const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe')
  execFileSync(tar, ['-xf', path, '-C', work, `jansson-${VERSION}/src`, `jansson-${VERSION}/LICENSE`])
  rmSync(target, { recursive: true, force: true })
  mkdirSync(target, { recursive: true })
  cpSync(join(work, `jansson-${VERSION}`, 'src'), join(target, 'src'), { recursive: true })
  cpSync(join(work, `jansson-${VERSION}`, 'LICENSE'), join(target, 'LICENSE'))
  console.log(`jansson ${VERSION} in ${target}`)
} finally {
  rmSync(work, { recursive: true, force: true })
}
