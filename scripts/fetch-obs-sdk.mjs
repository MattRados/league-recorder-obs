// Fetches the parts of OBS's source release that League Recorder builds against, for the same OBS version
// the app bundles, into out/obs-sdk (same folder layout as OBS's source):
//   libobs                 the core library's headers, for native/recorder
//   plugins/win-capture,   the capture plugin's source and what it needs, for scripts/build-native.mjs to
//   deps/ipc-util,         build win-capture.dll with native/win-capture/win-capture.patch
//   deps/w32-pthreads,
//   libobs-winrt
// Import libraries come from the bundled DLLs (scripts/build-native.mjs).
//
//   node scripts/fetch-obs-sdk.mjs                only if missing
//   node scripts/fetch-obs-sdk.mjs --force        re-download
//   node scripts/fetch-obs-sdk.mjs --zip <file>   from a copy of the same archive already on disk (checked the same way)

import { execFileSync } from 'child_process'
import { createHash } from 'crypto'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { dirname, join } from 'path'

const VERSION = '30.1.2'
const URL = `https://github.com/obsproject/obs-studio/archive/refs/tags/${VERSION}.zip`
// Pinned after the first download (GitHub's archive of the tag)
const SHA256 = 'c2dd03fa7fd01fad5beafce8f7156da11f9ed9a588373fd40b44a06f4c03b867'
const PARTS = ['libobs', 'plugins/win-capture', 'deps/ipc-util', 'deps/w32-pthreads', 'libobs-winrt']

const target = join(import.meta.dirname, '..', 'out', 'obs-sdk')
const local = process.argv.includes('--zip') ? process.argv[process.argv.indexOf('--zip') + 1] : null
const present = existsSync(join(target, 'libobs', 'obs.h')) && existsSync(join(target, 'plugins', 'win-capture', 'game-capture.c'))
if (present && !process.argv.includes('--force') && !local) {
  console.log('OBS source already present')
  process.exit(0)
}

let zip
if (local) {
  zip = readFileSync(local)
} else {
  console.log('Downloading', URL)
  const res = await fetch(URL)
  if (!res.ok) throw new Error(`${URL}: HTTP ${res.status}`)
  zip = Buffer.from(await res.arrayBuffer())
}
const actual = createHash('sha256').update(zip).digest('hex')
if (SHA256 && actual !== SHA256) throw new Error(`Checksum mismatch: expected ${SHA256}, got ${actual}`)
console.log('sha256', actual)

const work = mkdtempSync(join(tmpdir(), 'obs-sdk-'))
try {
  const zipPath = join(work, 'src.zip')
  writeFileSync(zipPath, zip)
  const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe')
  execFileSync(tar, ['-xf', zipPath, '-C', work, ...PARTS.map((p) => `obs-studio-${VERSION}/${p}`)])
  rmSync(target, { recursive: true, force: true })
  for (const part of PARTS) {
    mkdirSync(dirname(join(target, part)), { recursive: true })
    renameSync(join(work, `obs-studio-${VERSION}`, part), join(target, part))
  }
  console.log(`OBS ${VERSION} source (${PARTS.join(', ')}) in ${target}`)
} finally {
  rmSync(work, { recursive: true, force: true })
}
