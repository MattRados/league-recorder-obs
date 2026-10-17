// Fetches the headers of OBS's core library (libobs) for the same OBS version the app bundles, so
// native/recorder can be compiled against it: the libobs folder of OBS's source release, into
// out/obs-sdk/libobs. Import libraries come from the bundled DLLs (scripts/build-native.mjs).
//
//   node scripts/fetch-obs-sdk.mjs           only if missing
//   node scripts/fetch-obs-sdk.mjs --force   re-download

import { execFileSync } from 'child_process'
import { createHash } from 'crypto'
import { existsSync, mkdirSync, mkdtempSync, renameSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'

const VERSION = '30.1.2'
const URL = `https://github.com/obsproject/obs-studio/archive/refs/tags/${VERSION}.zip`
// Pinned after the first download (GitHub's archive of the tag)
const SHA256 = 'c2dd03fa7fd01fad5beafce8f7156da11f9ed9a588373fd40b44a06f4c03b867'

const target = join(import.meta.dirname, '..', 'out', 'obs-sdk')
if (existsSync(join(target, 'libobs', 'obs.h')) && !process.argv.includes('--force')) {
  console.log('OBS SDK headers already present')
  process.exit(0)
}

console.log('Downloading', URL)
const res = await fetch(URL)
if (!res.ok) throw new Error(`${URL}: HTTP ${res.status}`)
const zip = Buffer.from(await res.arrayBuffer())
const actual = createHash('sha256').update(zip).digest('hex')
if (SHA256 && actual !== SHA256) throw new Error(`Checksum mismatch: expected ${SHA256}, got ${actual}`)
console.log('sha256', actual)

const work = mkdtempSync(join(tmpdir(), 'obs-sdk-'))
try {
  const zipPath = join(work, 'src.zip')
  writeFileSync(zipPath, zip)
  const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe')
  execFileSync(tar, ['-xf', zipPath, '-C', work, `obs-studio-${VERSION}/libobs`])
  rmSync(target, { recursive: true, force: true })
  mkdirSync(target, { recursive: true })
  renameSync(join(work, `obs-studio-${VERSION}`, 'libobs'), join(target, 'libobs'))
  console.log(`OBS ${VERSION} headers in ${target}`)
} finally {
  rmSync(work, { recursive: true, force: true })
}
