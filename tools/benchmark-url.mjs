// Isolated normalization microbenchmark, not Chrome or application acceptance.
import { readFile } from 'node:fs/promises';
import { performance } from 'node:perf_hooks';
import assert from 'node:assert/strict';
const source = await readFile(new URL('../build/extension/worker.js', import.meta.url), 'utf8');
const start = source.indexOf('function urlForStorage(');
const end = source.indexOf('function webUrl(', start);
assert(start >= 0 && end > start);
const encoder = new TextEncoder();
const current = new Function('encoder', source.slice(start, end) + '\nreturn urlForStorage;')(encoder);
function baseline(value) {
    const normalized = value.toWellFormed();
    return encoder.encode(normalized).length <= 8192 ? normalized : '';
}
const typical = Array.from({ length: 1000 }, (_, i) =>
    `https://example.test/${i % 4 === 0 ? '検索/😀' : 'documents'}/${i}?q=${'query'.repeat(i % 40)}`);
const boundary = ['a'.repeat(8192), 'a'.repeat(8193), '\u0800'.repeat(2730), '\u0800'.repeat(2731), '😀'.repeat(2048), '\ud800'.repeat(2731)];
let checksum = 0;
function measure(fn, values, passes) {
    const begin = performance.now();
    let size = 0;
    for (let pass = 0; pass < passes; pass++) for (const value of values) size += fn(value).length;
    checksum ^= size;
    return (performance.now() - begin) * 1e6 / (passes * values.length);
}
function stats(samples) {
    samples.sort((a, b) => a - b);
    return { min: samples[0], median: samples[Math.floor(samples.length / 2)], max: samples.at(-1) };
}
const results = {};
for (const [name, values, passes] of [['typical', typical, 200], ['boundary', boundary, 1000]]) {
    for (const value of values) assert.equal(current(value), baseline(value));
    for (let warm = 0; warm < 3; warm++) { measure(baseline, values, passes); measure(current, values, passes); }
    const old = [], optimized = [];
    for (let trial = 0; trial < 9; trial++) {
        if (trial % 2) { optimized.push(measure(current, values, passes)); old.push(measure(baseline, values, passes)); }
        else { old.push(measure(baseline, values, passes)); optimized.push(measure(current, values, passes)); }
    }
    results[name] = { baseline_ns_per_url: stats(old), optimized_ns_per_url: stats(optimized) };
}
console.log(JSON.stringify({ runtime: process.version, platform: process.platform, architecture: process.arch, results, checksum,
    scope: 'Synthetic URL normalization only; excludes Chrome APIs, transport, full extension memory, and WASM.' }, null, 2));
