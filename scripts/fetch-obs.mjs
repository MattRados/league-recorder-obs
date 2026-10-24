// Downloads OBS Studio (portable zip from the official GitHub release), verifies it, and copies the
// parts of OBS's core League Recorder's own recorder (native/recorder, built into
// bin/64bit/league-recorder-obs.exe by scripts/build-native.mjs) needs into resources/obs:
// libobs and its Direct3D 11 renderer, the ffmpeg libraries and MP4 writer, and five plugins
// (game capture, audio capture, the NVENC/AMF, Quick Sync and x264 encoders). OBS's own program and
// interface (Qt), browser source, streaming, scripting and other plugins are left out.
//
// OBS 30.1.2 is the version tested with League (same as Ascent bundles). The zip's SHA-256 is pinned
// here (OBS doesn't publish checksums for the Windows zip), and the Authenticode signature of
// obs.dll is checked after extracting.
//
//   node scripts/fetch-obs.mjs           only if resources/obs is missing
//   node scripts/fetch-obs.mjs --force   re-download

import { execFileSync } from 'child_process'
import { createHash } from 'crypto'
import { cpSync, existsSync, mkdirSync, mkdtempSync, readdirSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { basename, join } from 'path'

const VERSION = '30.1.2'
const ZIP_URL = `https://github.com/obsproject/obs-studio/releases/download/${VERSION}/OBS-Studio-${VERSION}.zip`
const ZIP_SHA256 = '35be787de3169413eb5e6b6dc8b493c3f1973dd3f8366144b54575f897906dc0'
const PLUGINS = [
  'obs-ffmpeg', // the MP4 writer, AAC, and the NVENC and AMF encoders in 30.x
  'obs-qsv11',
  'obs-x264',
  'win-capture', // Game Capture + graphics hook
  'win-wasapi' // Application Audio Capture + microphones
]
// bin/64bit files only OBS's own program, interface or left-out plugins use
const NOT_NEEDED = /^(obs64\.exe|obs-frontend-api\.dll|obs-scripting\.dll|lua51\.dll|libobs-opengl\.dll|datachannel\.dll|Qt6\w*\.dll|platforms|styles|imageformats|iconengines)$|\.pdb$/i

const target = join(import.meta.dirname, '..', 'resources', 'obs')
if (existsSync(join(target, 'bin', '64bit', 'obs.dll')) && !process.argv.includes('--force')) {
  console.log('OBS already present')
  process.exit(0)
}

console.log('Downloading', ZIP_URL)
const res = await fetch(ZIP_URL)
if (!res.ok) throw new Error(`${ZIP_URL}: HTTP ${res.status}`)
const zip = Buffer.from(await res.arrayBuffer())
const actual = createHash('sha256').update(zip).digest('hex')
if (actual !== ZIP_SHA256) throw new Error(`Checksum mismatch: expected ${ZIP_SHA256}, got ${actual}`)

const work = mkdtempSync(join(tmpdir(), 'obs-'))
try {
  const zipPath = join(work, 'obs.zip')
  const src = join(work, 'x')
  writeFileSync(zipPath, zip)
  mkdirSync(src)
  execFileSync(join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe'), ['-xf', zipPath, '-C', src]) // Windows' own tar (10+) handles zip; Git Bash's GNU tar doesn't

  const core = join(src, 'bin', '64bit', 'obs.dll')
  const sig = execFileSync('powershell.exe', ['-NoProfile', '-Command', `(Get-AuthenticodeSignature '${core}').Status`]).toString().trim()
  if (sig !== 'Valid') throw new Error(`obs.dll signature is ${sig}`)

  rmSync(target, { recursive: true, force: true })
  cpSync(join(src, 'bin'), join(target, 'bin'), { recursive: true, filter: (f) => !NOT_NEEDED.test(basename(f)) })
  cpSync(join(src, 'data', 'libobs'), join(target, 'data', 'libobs'), { recursive: true })
  cpSync(join(src, 'data', 'obs-studio', 'license', 'gplv2.txt'), join(target, 'LICENSE-GPLv2.txt'))
  for (const p of PLUGINS) {
    cpSync(join(src, 'obs-plugins', '64bit', `${p}.dll`), join(target, 'obs-plugins', '64bit', `${p}.dll`))
    const data = join(src, 'data', 'obs-plugins', p)
    if (existsSync(data)) cpSync(data, join(target, 'data', 'obs-plugins', p), { recursive: true })
  }
  // OBS is GPLv2: ship its license and say where the matching source is. scripts/build-native.mjs rewrites this
  // when it replaces win-capture.dll with the one built from source, to say what changed.
  writeFileSync(
    join(target, 'README-OBS.txt'),
    `This folder contains OBS Studio ${VERSION} (https://obsproject.com), unmodified, with some plugins left out.\r\n` +
      `OBS Studio is licensed under the GNU General Public License v2: see LICENSE-GPLv2.txt.\r\n` +
      `Source code for this exact version: https://github.com/obsproject/obs-studio/tree/${VERSION}\r\n`
  )
  console.log(`Installed OBS ${VERSION} to ${target} (${readdirSync(join(target, 'obs-plugins', '64bit')).length} plugins)`)
} finally {
  rmSync(work, { recursive: true, force: true })
}
