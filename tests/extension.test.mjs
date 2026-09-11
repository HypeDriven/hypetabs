import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { runInNewContext } from 'node:vm';
import { webcrypto } from 'node:crypto';
const source = await readFile(new URL('../build/extension/worker.js', import.meta.url), 'utf8');
const event = () => ({ listeners: [], addListener(fn) { this.listeners.push(fn); }, fire(...args) { for (const fn of this.listeners) fn(...args); } });
const settle = async () => { for (let i = 0; i < 6; i++) await new Promise(setImmediate); };
function fixture() {
    const tabs = new Map([
        [1, { id: 1, windowId: 10, title: 'Work', url: 'https://example.test/work', incognito: false, lastAccessed: 100 }],
        [2, { id: 2, windowId: 11, title: 'Private', url: 'https://private.test', incognito: true }]
    ]);
    const sent = [], actions = [], alarms = [], storage = {};
    let recent = [];
    let displays = [
        { id: 'a', isPrimary: true, bounds: { left: 0, top: 0, width: 3840, height: 1600 }, dpiX: 96, dpiY: 96 },
        { id: 'b', isPrimary: false, bounds: { left: 3840, top: -1481, width: 1728, height: 3073 }, dpiX: 120, dpiY: 120 },
        { id: 'bad', isPrimary: false, bounds: { left: 1.5, top: 0, width: 10, height: 10 }, dpiX: 96, dpiY: 96 }
    ];
    const port = { onMessage: event(), onDisconnect: event(), postMessage(m) { sent.push(m); }, disconnect() { this.onDisconnect.fire(); } };
    const chrome = {
        storage: { local: { async get() { return storage; }, async set(o) { Object.assign(storage, o); } } },
        runtime: { connectNative(name) { assert.equal(name, 'com.hypetabs.bridge'); return port; }, onStartup: event(), onInstalled: event() },
        tabs: { async query() { return [...tabs.values()]; }, async get(id) { if (!tabs.has(id)) throw Error('gone'); return { ...tabs.get(id) }; }, async update(id, change) { actions.push(['tab', id, change]); Object.assign(tabs.get(id), change); }, async create(options) { actions.push(['create', options.url]); const tab = { id: 101, windowId: 10, url: options.url, incognito: false }; tabs.set(tab.id, tab); return tab; }, ...Object.fromEntries(['onCreated','onUpdated','onRemoved','onActivated','onAttached','onDetached','onMoved','onReplaced'].map(k => [k, event()])) },
        sessions: { async getRecentlyClosed() { return recent; }, async restore(id) {
            actions.push(['restore', id]); const entry = recent.find(item => item.tab?.sessionId === id); if (!entry) throw Error('gone');
            recent = recent.filter(item => item !== entry);
            const tab = { ...entry.tab, id: 100, windowId: 10 }; tabs.set(tab.id, tab); return { tab };
        }, onChanged: event() },
        windows: { async update(id, change) { actions.push(['window', id, change]); }, async get() { return { focused: true, left: 120, top: 140, width: 900, height: 600 }; } },
        alarms: { async create(name, config) { alarms.push([name, config]); }, onAlarm: event() },
        system: { display: { async getInfo() { return displays; }, onDisplayChanged: event() } },
        action: { onClicked: event() }
    };
    const helpers = runInNewContext(source + '\n({ urlForStorage });', { chrome, crypto: webcrypto, console, TextEncoder, URL });
    return { chrome, port, sent, actions, alarms, storage, tabs, helpers, setRecent(value) { recent = value; }, setDisplays(value) { displays = value; } };
}
test('URL byte limits preserve exact boundaries and Unicode replacement', async () => {
    const f = fixture(); await settle();
    const encoder = new TextEncoder();
    const samples = ['', 'a'.repeat(8192), 'a'.repeat(8193), '\u0800'.repeat(2730), '\u0800'.repeat(2731),
        '\u0800'.repeat(2730) + 'aa', '\u0800'.repeat(2730) + 'aaa', '\ud800'.repeat(2730),
        '😀'.repeat(2048), '😀'.repeat(2049), '\udc00abc\ud800'];
    let seed = 123456;
    for (let sample = 0; sample < 100; sample++) {
        let value = '';
        for (let i = 0; i < 2600 + sample * 3; i++) {
            seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
            value += String.fromCharCode(seed >>> 16);
        }
        samples.push(value);
    }
    for (const value of samples) {
        const normalized = value.toWellFormed();
        assert.equal(f.helpers.urlForStorage(value), encoder.encode(normalized).length <= 8192 ? normalized : '');
    }
});
test('profile-scoped snapshot excludes incognito; activation revalidates the target', async () => {
    const f = fixture(); await settle();
    assert.equal(f.sent[0].type, 'hello');
    assert.match(f.storage.profile, /^[0-9a-f-]{36}$/);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    assert.deepEqual(f.sent.map(m => m.type), ['hello', 'display', 'display', 'begin', 'upsert', 'end']);
    assert.equal(f.sent[4].id, 1);
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 1, id: 1 }); await settle();
    assert.deepEqual(f.actions.map(a => a.slice(0, 2)), [['tab', 1], ['window', 10]]);
    assert.equal(f.sent.at(-1).status, 'ok');
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 2, id: 2 }); await settle();
    assert.equal(f.actions.length, 2); assert.equal(f.sent.at(-1).status, 'unavailable');
});
test('updates, removal, pause and resume reconcile without collecting private tabs', async () => {
    const f = fixture(); await settle();
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    f.tabs.get(1).title = 'Changed'; f.chrome.tabs.onUpdated.fire(1, { title: 'Changed' }, f.tabs.get(1)); await settle();
    assert.equal(f.sent.at(-1).title, 'Changed');
    f.tabs.delete(1); f.chrome.tabs.onRemoved.fire(1); await settle();
    assert.equal(f.sent.at(-1).type, 'close');
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: false });
    const before = f.sent.length;
    f.chrome.tabs.onActivated.fire({ tabId: 2 }); await settle(); assert.equal(f.sent.length, before);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    assert.deepEqual(f.sent.slice(before).map(m => m.type), ['begin', 'end']);
    f.port.disconnect(); assert.equal(f.alarms.length, 1);
});
test('guidance reports a title only after successful non-private foreground activation', async () => {
    const f = fixture(); await settle();
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 20, id: 1, guided: true }); await settle();
    assert.deepEqual(f.sent.slice(-2).map(m => m.type), ['located', 'result']);
    assert.equal(f.sent.at(-2).title, 'Work');
    assert.equal(f.sent.at(-2).request, 20);
    assert.deepEqual(['left', 'top', 'width', 'height'].map(k => f.sent.at(-2)[k]), [120, 140, 900, 600]);
    const count = () => f.sent.filter(m => m.type === 'located').length;
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 21, id: 2, guided: true }); await settle();
    assert.equal(count(), 1);
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 22, id: 1 }); await settle();
    assert.equal(count(), 1);
    f.chrome.windows.get = async () => ({ focused: false });
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 23, id: 1, guided: true }); await settle();
    assert.equal(count(), 1);
    assert.equal(f.sent.at(-1).status, 'focus');
    for (const bounds of [{}, { left: 0, top: 0, width: 0, height: 600 }, { left: 0.5, top: 0, width: 900, height: 600 }]) {
        f.chrome.windows.get = async () => ({ focused: true, ...bounds });
        f.port.onMessage.fire({ v: 1, type: 'activate', request: 24, id: 1, guided: true }); await settle();
        assert.equal(count(), 1);
        assert.equal(f.sent.at(-1).status, 'ok');
    }
});
test('malformed activation commands and unknown protocol versions do not act', async () => {
    const f = fixture(); await settle();
    for (const message of [null, {v:2,type:'activate',id:1,request:1}, {v:1,type:'activate',id:-1,request:1}, {v:1,type:'activate',id:1,request:'1'}, {v:1,type:'execute',id:1,request:1}]) f.port.onMessage.fire(message);
    await settle(); assert.equal(f.actions.length, 0);
});
test('pausing collection preserves activation without collecting new metadata', async () => {
    const f = fixture(); await settle();
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: false }); await settle();
    const before = f.sent.length;
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 30, id: 1 }); await settle();
    assert.deepEqual(f.actions.map(a => a.slice(0, 2)), [['tab', 1], ['window', 10]]);
    assert.deepEqual(f.sent.slice(before).map(m => m.type), ['result']);
    assert.equal(f.sent.at(-1).status, 'ok');
    f.port.onMessage.fire({ v: 1, type: 'activate', request: 31, id: 2 }); await settle();
    assert.equal(f.actions.length, 2);
    assert.equal(f.sent.at(-1).status, 'unavailable');
});
test('paused collection still permits explicit safe closed-tab restoration', async () => {
    const f = fixture(); await settle();
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: false }); await settle();
    const before = f.sent.length;
    f.port.onMessage.fire({ v: 1, type: 'restore', request: 32, session: '', url: 'https://example.test/closed' }); await settle();
    assert.deepEqual(f.actions.filter(a => a[0] === 'create'), [['create', 'https://example.test/closed']]);
    assert.deepEqual(f.sent.slice(before).map(m => m.type), ['result']);
    assert.equal(f.sent.at(-1).status, 'restored');
});
test('pause/resume during an outstanding snapshot fetch discards the stale snapshot', async () => {
    const f = fixture(); await settle();
    let release;
    let calls = 0;
    f.chrome.tabs.query = async () => {
        if (++calls === 1) return await new Promise(resolve => { release = resolve; });
        return [{ id: 3, windowId: 10, incognito: false, title: 'Fresh' }];
    };
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: false });
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true });
    release([{ id: 1, windowId: 10, incognito: false, title: 'Stale' }]); await settle();
    assert.equal(calls, 2);
    assert.deepEqual(f.sent.filter(m => m.type === 'upsert').map(m => m.title), ['Fresh']);
});
test('imports individual and window tabs for search but excludes private sessions', async () => {
    const f = fixture(); await settle();
    f.setRecent([
        { lastModified: Date.now()/1000, tab: { sessionId: 'one', title: 'Closed', url: 'https://example.test/closed', incognito: false } },
        { lastModified: Date.now()/1000, tab: { sessionId: 'private', url: 'https://private.test', incognito: true } },
        { lastModified: Date.now()/1000, tab: { sessionId: 'unsafe', url: 'javascript:alert(1)', incognito: false } },
        { lastModified: Date.now()/1000, window: { sessionId: 'window', incognito: false, tabs: [{ title: 'Window child', url: 'https://example.test/child', incognito: false }] } }
    ]);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    assert.deepEqual(f.sent.filter(m => m.type === 'recent').map(m => m.session), ['one', 'unsafe', 'window:window:0']);
});
test('restores only a verified individual session and consumes its import candidate', async () => {
    const f = fixture(); await settle();
    f.setRecent([{ lastModified: Date.now()/1000, tab: { sessionId: 'one', title: 'Closed', url: 'https://example.test/closed', incognito: false } }]);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    f.port.onMessage.fire({ v: 1, type: 'restore', request: 10, session: 'one', url: 'https://example.test/closed' }); await settle();
    assert.deepEqual(f.actions.filter(a => a[0] === 'restore'), [['restore', 'one']]);
    assert.equal(f.actions.some(a => a[0] === 'create'), false);
    assert.equal(f.sent.find(m => m.type === 'result' && m.request === 10).status, 'restored');
    const count = f.sent.filter(m => m.type === 'recent').length;
    f.chrome.sessions.onChanged.fire(); await settle();
    assert.equal(f.sent.filter(m => m.type === 'recent').length, count);
});
test('URL fallback never restores a whole window or opens an unsafe scheme', async () => {
    const f = fixture(); await settle();
    f.setRecent([{ lastModified: Date.now()/1000, window: { sessionId: 'whole', incognito: false, tabs: [] } }]);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    f.port.onMessage.fire({ v: 1, type: 'restore', request: 11, session: 'whole', url: 'https://example.test/only' }); await settle();
    assert.deepEqual(f.actions.filter(a => a[0] === 'create'), [['create', 'https://example.test/only']]);
    assert.equal(f.actions.some(a => a[0] === 'restore'), false);
    const before = f.actions.length;
    for (const url of ['javascript:alert(1)', 'file:///C:/x', 'data:text/html,test', 'https://a.test/\nother', ' https://a.test/'])
        f.port.onMessage.fire({ v: 1, type: 'restore', request: 12, session: '', url });
    await settle(); assert.equal(f.actions.length, before);
});
test('clear cutoff and disabled retention prevent recent history import', async () => {
    const f = fixture(); await settle();
    f.setRecent([{ lastModified: 100, tab: { sessionId: 'old', url: 'https://example.test/old', incognito: false } }]);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true, since: 200000 }); await settle();
    assert.equal(f.sent.filter(m => m.type === 'recent').length, 0);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true, history: false, since: 0 }); await settle();
    assert.equal(f.sent.filter(m => m.type === 'recent').length, 0);
});
test('URLs longer than the protocol limit are not saved as truncated destinations', async () => {
    const f = fixture(); await settle();
    f.tabs.get(1).url = 'https://example.test/' + 'x'.repeat(9000);
    f.port.onMessage.fire({ v: 1, type: 'collect', enabled: true }); await settle();
    assert.equal(f.sent.find(m => m.type === 'upsert').url, '');
});
test('profile renaming persists locally and acknowledges only after storage succeeds', async () => {
    const f = fixture(); await settle();
    f.port.onMessage.fire({ v: 1, type: 'setLabel', label: '  Work profile  ' }); await settle();
    assert.equal(f.storage.label, 'Work profile');
    assert.equal(f.sent.at(-1).type, 'label'); assert.equal(f.sent.at(-1).label, 'Work profile');
    const before = f.sent.length;
    for (const label of ['', 'x'.repeat(33), 'bad\nname', 'bad\u007fname']) f.port.onMessage.fire({ v: 1, type: 'setLabel', label });
    await settle(); assert.equal(f.sent.length, before); assert.equal(f.storage.label, 'Work profile');
    f.chrome.storage.local.set = async () => { throw Error('storage failed'); };
    f.port.onMessage.fire({ v: 1, type: 'setLabel', label: 'Unsaved name' }); await settle();
    assert.equal(f.sent.at(-1).type, 'labelError'); assert.equal(f.storage.label, 'Work profile');
});

test('prepare selects a revalidated tab without focusing its browser window', async () => {
    const f = fixture(); await settle();
    f.chrome.windows.get = async () => ({ focused: false, left: 120, top: 140, width: 900, height: 600 });
    f.port.onMessage.fire({ v: 1, type: 'prepare', request: 50, id: 1 }); await settle();
    assert.deepEqual(f.actions.map(a => a.slice(0, 2)), [['tab', 1]]);
    assert.equal(f.sent.at(-1).type, 'prepared');
    assert.equal(f.sent.at(-1).request, 50);
    assert.equal(f.sent.at(-1).title, 'Work');
    assert.deepEqual(['left','top','width','height'].map(k => f.sent.at(-1)[k]), [120,140,900,600]);
    assert.equal(f.sent.filter(m => m.type === 'result').length, 0);
    f.port.onMessage.fire({ v: 1, type: 'prepare', request: 51, id: 2 }); await settle();
    assert.equal(f.sent.at(-1).status, 'unavailable');
    assert.equal(f.actions.length, 1);
});
test('prepare refuses changed, inactive or private targets and invalid geometry without requesting focus', async () => {
    for (const change of [t => { t.windowId = 99; }, t => { t.active = false; }, t => { t.incognito = true; }]) {
        const f = fixture(); await settle();
        f.chrome.windows.get = async () => { change(f.tabs.get(1)); return { left: 120, top: 140, width: 900, height: 600 }; };
        f.port.onMessage.fire({ v: 1, type: 'prepare', request: 52, id: 1 }); await settle();
        assert.equal(f.sent.at(-1).status, 'unavailable');
        assert.equal(f.sent.some(m => m.type === 'prepared'), false);
        assert.equal(f.actions.some(a => a[0] === 'window'), false);
    }
    const f = fixture(); await settle();
    f.chrome.windows.get = async () => ({ left: 0.5, top: 140, width: 900, height: 600 });
    f.port.onMessage.fire({ v: 1, type: 'prepare', request: 53, id: 1 }); await settle();
    assert.equal(f.sent.at(-1).status, 'unavailable');
    f.chrome.windows.get = async () => { f.port.disconnect(); return { left: 120, top: 140, width: 900, height: 600 }; };
    const before = f.sent.length;
    f.port.onMessage.fire({ v: 1, type: 'prepare', request: 54, id: 1 }); await settle();
    assert.equal(f.sent.length, before);
});
test('display layout is reported after hello, skips malformed entries, and refreshes on change', async () => {
    const f = fixture(); await settle();
    const plain = value => JSON.parse(JSON.stringify(value));
    const displays = plain(f.sent.filter(m => m.type === 'display'));
    assert.equal(f.sent.findIndex(m => m.type === 'hello') < f.sent.findIndex(m => m.type === 'display'), true);
    assert.deepEqual(displays, [
        { v: 1, type: 'display', index: 0, count: 2, left: 0, top: 0, width: 3840, height: 1600, dpi: 96, primary: true },
        { v: 1, type: 'display', index: 1, count: 2, left: 3840, top: -1481, width: 1728, height: 3073, dpi: 120, primary: false }
    ]);
    f.setDisplays([{ id: 'a', isPrimary: true, bounds: { left: 0, top: 0, width: 1920, height: 1080 }, dpiX: 96, dpiY: 96 }]);
    f.chrome.system.display.onDisplayChanged.fire(); await settle();
    const latest = plain(f.sent.filter(m => m.type === 'display').slice(-1)[0]);
    assert.deepEqual(latest, { v: 1, type: 'display', index: 0, count: 1, left: 0, top: 0, width: 1920, height: 1080, dpi: 96, primary: true });
    f.setDisplays([]); f.chrome.system.display.onDisplayChanged.fire(); await settle();
    assert.equal(f.sent.filter(m => m.type === 'display').length, 3);
});
