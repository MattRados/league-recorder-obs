// Builds League Recorder's two native programs with MSVC (Visual Studio's C++ tools, found with
// vswhere), into resources/obs/bin/64bit next to the OBS libraries they use:
//   native/recorder -> league-recorder-obs.exe    the recorder, on OBS's core library (obs.dll)
//   native/media    -> league-recorder-media.exe  audio track jobs, on the FFmpeg libraries OBS bundles
// Needs the bundled OBS (scripts/fetch-obs.mjs), the libobs headers (scripts/fetch-obs-sdk.mjs) and
// the FFmpeg headers (scripts/fetch-ffmpeg-headers.mjs). Import libraries are generated from the DLLs' exports.

import { execFileSync } from 'child_process'
import { copyFileSync, existsSync, mkdirSync, rmSync, writeFileSync } from 'fs'
import { join } from 'path'

const root = join(import.meta.dirname, '..')
const obsBin = join(root, 'resources', 'obs', 'bin', '64bit')
const obsSdk = join(root, 'out', 'obs-sdk', 'libobs')
const ffmpegSdk = join(root, 'out', 'ffmpeg-sdk')
const work = join(root, 'out', 'native-build')
if (!existsSync(join(obsBin, 'obs.dll'))) throw new Error('Run scripts/fetch-obs.mjs first')
if (!existsSync(join(obsSdk, 'obs.h'))) throw new Error('Run scripts/fetch-obs-sdk.mjs first')
if (!existsSync(join(ffmpegSdk, 'libavformat', 'avformat.h'))) throw new Error('Run scripts/fetch-ffmpeg-headers.mjs first')

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
msvc([
  `${cl} /I "${obsSdk}" "${join(root, 'native', 'recorder', 'recorder.cpp')}" ${importLib('obs.dll')} user32.lib shell32.lib ole32.lib mmdevapi.lib /Fe:league-recorder-obs.exe /link /NOLOGO /SUBSYSTEM:WINDOWS`
])
copyFileSync(join(work, 'league-recorder-obs.exe'), join(obsBin, 'league-recorder-obs.exe'))
console.log('Built', join(obsBin, 'league-recorder-obs.exe'))

// The media tool (a console program: the core reads its progress from stdout)
const ffmpegLibs = ['avformat-60.dll', 'avcodec-60.dll', 'avutil-58.dll', 'swresample-4.dll', 'swscale-7.dll'].map(importLib).join(' ')
msvc([
  `${cl} /I "${ffmpegSdk}" "${join(root, 'native', 'media', 'media.cpp')}" ${ffmpegLibs} shell32.lib /Fe:league-recorder-media.exe /link /NOLOGO /SUBSYSTEM:CONSOLE`
])
copyFileSync(join(work, 'league-recorder-media.exe'), join(obsBin, 'league-recorder-media.exe'))
console.log('Built', join(obsBin, 'league-recorder-media.exe'))
