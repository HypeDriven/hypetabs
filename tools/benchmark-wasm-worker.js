// Appended only to the disposable test extension; uses its actual urlForStorage.
async function benchmarkWasmUrl() {
const bytes = await (await fetch(chrome.runtime.getURL('url.wasm'))).arrayBuffer();
const module = await WebAssembly.compile(bytes);
if (WebAssembly.Module.imports(module).length) throw new Error('Unexpected WASM imports');
const { exports } = await WebAssembly.instantiate(module);
const current = urlForStorage;
const pointer = exports.url_input();
const input = new Uint16Array(exports.memory.buffer, pointer, 8192);
function candidate(value) {
    if (value.length > 8192) return '';
    if (value.length <= 2730) return value.toWellFormed();
    for (let i = 0; i < value.length; ++i) input[i] = value.charCodeAt(i);
    return exports.fits_url(pointer, value.length) ? value.toWellFormed() : '';
}
function verify(value) {
    const normalized = value.toWellFormed();
    const expected = encoder.encode(normalized).length <= 8192 ? normalized : '';
    if (current(value) !== expected || candidate(value) !== expected) throw new Error('URL equivalence failed');
}
let seed = 92731;
function random() { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed; }
for (let trial = 0; trial < 1000; ++trial) {
    const units = Array.from({ length: random() % 9000 }, () => random() & 0xffff);
    verify(String.fromCharCode(...units));
}
const typical = Array.from({ length: 1000 }, (_, i) => `https://example.test/${i % 4 ? 'documents' : '検索/😀'}/${i}?q=${'query'.repeat(i % 40)}`);
const boundary = ['a'.repeat(8192), 'a'.repeat(8193), '\u0800'.repeat(2730), '\u0800'.repeat(2731), '😀'.repeat(2048), '\ud800'.repeat(2731)];
let checksum = 0;
function measure(fn, values, passes) {
    const start = performance.now(); let size = 0;
    for (let pass = 0; pass < passes; ++pass) for (const value of values) size += fn(value).length;
    checksum ^= size;
    return (performance.now() - start) * 1e6 / (passes * values.length);
}
function stats(values) { values.sort((a,b)=>a-b); return { min: values[0], median: values[4], max: values.at(-1) }; }
const results = {};
for (const [name, values, passes] of [['typical', typical, 200], ['boundary', boundary, 1000]]) {
    for (const value of values) verify(value);
    for (let warmup = 0; warmup < 3; ++warmup) { measure(current, values, passes); measure(candidate, values, passes); }
    const js = [], wasm = [];
    for (let trial = 0; trial < 9; ++trial) {
        if (trial % 2) { wasm.push(measure(candidate, values, passes)); js.push(measure(current, values, passes)); }
        else { js.push(measure(current, values, passes)); wasm.push(measure(candidate, values, passes)); }
    }
    results[name] = { javascript_ns_per_url: stats(js), wasm_bridge_ns_per_url: stats(wasm) };
}

return { runtime: navigator.userAgent, moduleBytes: bytes.byteLength,
    linearMemoryBytes: exports.memory.buffer.byteLength, results, checksum,
    scope: 'Chrome extension worker normalization including UTF-16 copying; excludes event forwarding, transport, total memory and lifecycle.' };
}
