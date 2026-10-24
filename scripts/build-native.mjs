// Builds League Recorder's native parts with MSVC (Visual Studio's C++ tools, found with vswhere),
// into resources/obs next to the OBS libraries they use:
//   native/recorder    -> bin/64bit/league-recorder-obs.exe    the recorder, on OBS's core library (obs.dll)
//   native/media       -> bin/64bit/league-recorder-media.exe  audio track jobs, on the FFmpeg libraries OBS bundles
//   native/win-capture -> obs-plugins/64bit/win-capture.dll    OBS's own capture plugin, built from its source
//                                                              with our patch, in place of the bundled one
// Needs the bundled OBS (scripts/fetch-obs.mjs), OBS's source (scripts/fetch-obs-sdk.mjs), the FFmpeg headers
// (scripts/fetch-ffmpeg-headers.mjs) and jansson (scripts/fetch-jansson.mjs). Import libraries are generated
// from the DLLs' exports.
//
//   node scripts/build-native.mjs                all three
//   node scripts/build-native.mjs win-capture    only the ones named (recorder, media, win-capture)

import { execFileSync } from 'child_process'
import { copyFileSync, cpSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'fs'
import { join } from 'path'

const root = join(import.meta.dirname, '..')
const obsBin = join(root, 'resources', 'obs', 'bin', '64bit')
const obsPlugins = join(root, 'resources', 'obs', 'obs-plugins', '64bit')
const obsSource = join(root, 'out', 'obs-sdk')
const obsSdk = join(obsSource, 'libobs')
const ffmpegSdk = join(root, 'out', 'ffmpeg-sdk')
const janssonSource = join(root, 'out', 'jansson-src', 'src')
const work = join(root, 'out', 'native-build')
const wanted = process.argv.slice(2)
const building = (name) => wanted.length === 0 || wanted.includes(name)
if (!existsSync(join(obsBin, 'obs.dll'))) throw new Error('Run scripts/fetch-obs.mjs first')
if (!existsSync(join(obsSdk, 'obs.h'))) throw new Error('Run scripts/fetch-obs-sdk.mjs first')
if (building('win-capture') && !existsSync(join(obsSource, 'plugins', 'win-capture', 'game-capture.c')))
  throw new Error('Run scripts/fetch-obs-sdk.mjs --force first (the capture plugin\'s source is missing)')
if (building('win-capture') && !existsSync(join(janssonSource, 'jansson.h'))) throw new Error('Run scripts/fetch-jansson.mjs first')
if (building('media') && !existsSync(join(ffmpegSdk, 'libavformat', 'avformat.h')))
  throw new Error('Run scripts/fetch-ffmpeg-headers.mjs first')

const vswhere = join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
const vs = execFileSync(vswhere, ['-latest', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'])
  .toString()
  .trim()
if (!vs) throw new Error('Visual Studio C++ build tools not found')
const vcvars = join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')

rmSync(work, { recursive: true, force: true })
mkdirSync(work, { recursive: true })

/** Run commands inside a Visual Studio x64 developer environment. */
function msvc(commands) {
  const script = join(work, 'run.cmd')
  writeFileSync(script, ['@echo off', `call "${vcvars}" >nul 2>&1`, ...commands.map((c) => `${c} || exit /b 1`)].join('\r\n'))
  try {
    return execFileSync('cmd.exe', ['/d', '/c', script], { cwd: work }).toString()
  } catch (err) {
    throw new Error(String(err.stdout ?? '') + String(err.stderr ?? ''))
  }
}

/** A DLL's exports -> .def -> import .lib (named after the DLL) */
function importLib(dll) {
  const name = dll.replace(/\.dll$/i, '')
  const dump = msvc([`dumpbin /nologo /exports "${join(obsBin, dll)}"`])
  const names = [...dump.matchAll(/^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)/gm)].map((m) => m[1])
  if (names.length < 20) throw new Error(`Couldn't read ${dll}'s exports`)
  writeFileSync(join(work, `${name}.def`), `LIBRARY "${dll}"\r\nEXPORTS\r\n${names.map((n) => `  ${n}`).join('\r\n')}\r\n`)
  msvc([`lib /nologo /def:${name}.def /machine:x64 /out:${name}.lib`])
  return `${name}.lib`
}

const cl = 'cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /DUNICODE /D_UNICODE /DNOMINMAX'

// The recorder
if (building('recorder')) {
  msvc([
    `${cl} /I "${obsSdk}" "${join(root, 'native', 'recorder', 'recorder.cpp')}" ${importLib('obs.dll')} user32.lib shell32.lib ole32.lib mmdevapi.lib /Fe:league-recorder-obs.exe /link /NOLOGO /SUBSYSTEM:WINDOWS`
  ])
  copyFileSync(join(work, 'league-recorder-obs.exe'), join(obsBin, 'league-recorder-obs.exe'))
  console.log('Built', join(obsBin, 'league-recorder-obs.exe'))
}

// The media tool (a console program: the core reads its progress from stdout)
if (building('media')) {
  const ffmpegLibs = ['avformat-60.dll', 'avcodec-60.dll', 'avutil-58.dll', 'swresample-4.dll', 'swscale-7.dll'].map(importLib).join(' ')
  msvc([
    `${cl} /I "${ffmpegSdk}" "${join(root, 'native', 'media', 'media.cpp')}" ${ffmpegLibs} shell32.lib /Fe:league-recorder-media.exe /link /NOLOGO /SUBSYSTEM:CONSOLE`
  ])
  copyFileSync(join(work, 'league-recorder-media.exe'), join(obsBin, 'league-recorder-media.exe'))
  console.log('Built', join(obsBin, 'league-recorder-media.exe'))
}

// The capture plugin. OBS's own, with native/win-capture/win-capture.patch on top: the 30.1.2 one can crash
// the recorder at a game's start (see the patch). The hook it puts into the game, and the helpers it starts,
// stay the bundled ones from the same OBS version. Built the way OBS's CMake builds it: C, the shared CRT,
// OBS's compiler settings (cmake/windows/compilerconfig.cmake), jansson linked in.
if (building('win-capture')) {
  const src = join(work, 'obs')
  for (const dir of ['libobs', 'libobs-winrt', 'plugins/win-capture', 'deps/ipc-util']) cpSync(join(obsSource, dir), join(src, dir), { recursive: true })
  // Inside this repo, git apply would take the patch's paths from the repo's root and skip them all, so it's
  // kept from finding the repo. Then each patched file is checked to have changed.
  const patch = join(root, 'native', 'win-capture', 'win-capture.patch')
  execFileSync('git', ['apply', '--whitespace=nowarn', patch], { cwd: src, env: { ...process.env, GIT_CEILING_DIRECTORIES: work } })
  for (const [, file] of readFileSync(patch, 'utf8').matchAll(/^\+\+\+ b\/(.+)$/gm))
    if (readFileSync(join(src, file.trim())).equals(readFileSync(join(obsSource, file.trim())))) throw new Error(`The patch didn't change ${file}`)

  const cc = 'cl /nologo /c /O2 /Oi /Gy /MD /Brepro /W3 /utf-8 /permissive- /Zc:preprocessor /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS'
  const janssonConfig = join(root, 'native', 'win-capture', 'jansson')
  mkdirSync(join(work, 'jansson'), { recursive: true })
  const janssonFiles = readdirSync(janssonSource).filter((f) => f.endsWith('.c'))
  msvc([
    `${cc} /DHAVE_CONFIG_H /wd4005 /wd4996 /I "${janssonConfig}" /I "${janssonSource}" /Fo:jansson\\ ${janssonFiles.map((f) => `"${join(janssonSource, f)}"`).join(' ')}`,
    `lib /nologo /Brepro /out:jansson.lib jansson\\*.obj`
  ])

  const capture = join(src, 'plugins', 'win-capture')
  const files = [
    ...['app-helpers.c', 'audio-helpers.c', 'compat-helpers.c', 'cursor-capture.c', 'dc-capture.c', 'duplicator-monitor-capture.c',
      'game-capture-file-init.c', 'game-capture.c', 'load-graphics-offsets.c', 'monitor-capture.c', 'nt-stuff.c', 'plugin-main.c',
      'window-capture.c', 'inject-library.c'].map((f) => join(capture, f)),
    join(src, 'deps', 'ipc-util', 'ipc-util', 'pipe-windows.c'),
    join(src, 'libobs', 'util', 'windows', 'obfuscate.c')
  ]
  const includes = [src, join(src, 'libobs'), join(src, 'libobs-winrt'), join(src, 'deps', 'ipc-util'), join(obsSource, 'deps', 'w32-pthreads'), janssonConfig, janssonSource]
  mkdirSync(join(work, 'win-capture'), { recursive: true })
  msvc([
    `${cc} ${includes.map((i) => `/I "${i}"`).join(' ')} /Fo:win-capture\\ ${files.map((f) => `"${f}"`).join(' ')}`,
    `link /nologo /DLL /Brepro /OPT:REF /OPT:ICF /OUT:win-capture.dll win-capture\\*.obj jansson.lib ${importLib('obs.dll')} ${importLib('w32-pthreads.dll')} user32.lib gdi32.lib shell32.lib advapi32.lib`
  ])
  copyFileSync(join(work, 'win-capture.dll'), join(obsPlugins, 'win-capture.dll'))
  // jansson is MIT-licensed: its notice goes with the copy built into the plugin. And the GPL asks a changed
  // file to say so: the folder's readme (first written by scripts/fetch-obs.mjs) now says what changed and where.
  copyFileSync(join(janssonSource, '..', 'LICENSE'), join(root, 'resources', 'obs', 'LICENSE-jansson.txt'))
  writeFileSync(
    join(root, 'resources', 'obs', 'README-OBS.txt'),
    'This folder contains OBS Studio 30.1.2 (https://obsproject.com) with some plugins left out. It is unmodified\r\n' +
      'except obs-plugins/64bit/win-capture.dll, which League Recorder builds from OBS 30.1.2\'s source with a patch\r\n' +
      '(a crash fix at a game\'s start, and no download of OBS\'s compatibility list), with jansson 2.14 built in\r\n' +
      '(MIT licence: see LICENSE-jansson.txt). The patch and how it is built: https://github.com/MattRados/league-recorder-obs\r\n' +
      'OBS Studio is licensed under the GNU General Public License v2: see LICENSE-GPLv2.txt.\r\n' +
      'Source code for this exact version: https://github.com/obsproject/obs-studio/tree/30.1.2\r\n'
  )
  console.log('Built', join(obsPlugins, 'win-capture.dll'))
}
