import { mkdir } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const root = new URL('../', import.meta.url);
const compiler = process.env.HYPETABS_WASI_CXX ?? '/tmp/hypetabs-wasi-toolchain/wasi-sdk-27.0-x86_64-linux/bin/clang++';
await mkdir(new URL('build/wasm-experiment/', root), { recursive: true });
const result = spawnSync(compiler, ['--target=wasm32-unknown-unknown', '-std=c++20', '-O3', '-nostdlib', '-fno-exceptions', '-fno-rtti',
    '-Wl,--no-entry,--export=fits_url,--export=url_input,--export-memory,--initial-memory=65536,--max-memory=65536,-z,stack-size=16384',
    fileURLToPath(new URL('extension/wasm/url.cpp', root)), '-o', fileURLToPath(new URL('build/wasm-experiment/url.wasm', root))], { stdio: 'inherit' });
if (result.error) throw result.error;
if (result.status !== 0) process.exit(result.status ?? 1);
console.log('Built experimental WASM without runtime libraries; normal extension packaging is unchanged.');
