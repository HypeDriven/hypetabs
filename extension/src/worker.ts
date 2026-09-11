// Chrome's API bridge stays small; the native host owns indexing and search.
// This is the baseline for the pending steady-state WASM comparison.
declare const chrome: any;
type Tab = { id?: number; windowId: number; incognito: boolean; title?: string; url?: string; lastAccessed?: number; sessionId?: string };
type Port = { postMessage(value: object): void; disconnect(): void; onMessage: any; onDisconnect: any };
let port: Port | undefined;
let connecting = false;
let collecting = false;
let synchronizing = false;
let collectionEpoch = 0;
let flushing = false;
const dirty = new Set<number>();
const closures = new Map<number, number>();
const knownOpen = new Set<number>();
const suppressedSessions = new Set<string>();
let importing = false;
let importAgain = false;
let historyEnabled = true;
let historyFloor = 0;
const encoder = new TextEncoder();
function urlForStorage(value: string): string {
    // UTF-8 uses at least one byte per UTF-16 code unit, and at most three
    // after replacing lone surrogates. Most URLs need no byte buffer at all.
    if (value.length > 8192) return '';
    const wellFormed = value.toWellFormed();
    // Never save a truncated URL that could later navigate to a different page.
    return wellFormed.length <= 2730 || encoder.encode(wellFormed).length <= 8192 ? wellFormed : '';
}
function webUrl(value: unknown): value is string {
    if (typeof value !== 'string' || !value || /[\s\x00-\x1f\\]/u.test(value) || encoder.encode(value).length > 8192) return false;
    try { const parsed = new URL(value); return parsed.protocol === 'http:' || parsed.protocol === 'https:'; } catch { return false; }
}
async function importRecent(): Promise<void> {
    if (!collecting || !port || !historyEnabled) return;
    if (importing) { importAgain = true; return; }
    importing = true;
    const current = port, epoch = collectionEpoch;
    try {
        const sessions = await chrome.sessions.getRecentlyClosed({ maxResults: 25 });
        if (port !== current || !collecting || epoch !== collectionEpoch || !historyEnabled) return;
        for (const session of sessions) {
            const candidates: Tab[] = session.tab ? [session.tab] : session.window?.incognito === false ? (session.window.tabs ?? []) : [];
            for (let index = 0; index < candidates.length; index++) {
                const tab = candidates[index];
                const id = session.tab ? tab.sessionId : `window:${session.window.sessionId}:${index}`;
                if (session.lastModified * 1000 <= historyFloor || tab.incognito !== false || typeof id !== 'string' || id.length > 256 || suppressedSessions.has(id) || typeof tab.url !== 'string') continue;
                const url = urlForStorage(tab.url);
                if (!url) continue;
                send({ type: 'recent', session: id, title: (tab.title ?? '').slice(0, 1000).toWellFormed(),
                    url, closed: Math.trunc(session.lastModified * 1000), incognito: false });
            }
        }
    } catch { /* Missing recent-session data must not prevent open-tab search. */ }
    finally {
        importing = false;
        if (importAgain) { importAgain = false; void importRecent(); }
    }
}
let retry = 1;
function send(value: object): void { port?.postMessage({ v: 1, ...value }); }
function normalized(tab: Tab): object | undefined {
    if (tab.incognito || !Number.isInteger(tab.id) || tab.id! < 0) return;
    return { type: 'upsert', id: tab.id, window: tab.windowId, title: (tab.title ?? '').slice(0, 1000).toWellFormed(),
        url: urlForStorage(tab.url ?? ''), used: Math.max(0, Math.trunc(tab.lastAccessed ?? 0)), incognito: false };
}
async function flush(): Promise<void> {
    if (flushing || synchronizing || !collecting || !port) return;
    flushing = true;
    const current = port;
    const epoch = collectionEpoch;
    try {
        while (dirty.size && collecting && port === current && epoch === collectionEpoch) {
            const id = dirty.values().next().value!; dirty.delete(id);
            const closed = closures.get(id);
            if (closed !== undefined) {
                closures.delete(id);
                if (knownOpen.delete(id)) send({ type: 'close', id, closed });
                continue;
            }
            try {
                const tab: Tab = await chrome.tabs.get(id);
                if (!collecting || port !== current || epoch !== collectionEpoch) break;
                const packet = normalized(tab);
                if (packet) { knownOpen.add(id); send(packet); } else { knownOpen.delete(id); send({ type: 'remove', id }); }
            } catch { if (!closures.has(id) && collecting && port === current && epoch === collectionEpoch) send({ type: 'remove', id }); }
        }
    } finally { flushing = false; if (dirty.size) void flush(); }
}
function changed(id: number): void {
    if (!collecting || !Number.isInteger(id) || id < 0) return;
    if (dirty.size >= 10000) { collecting = false; port?.disconnect(); return; }
    dirty.add(id); void flush();
}
async function snapshot(): Promise<void> {
    if (!port || !collecting || synchronizing) return;
    synchronizing = true;
    const current = port;
    const epoch = collectionEpoch;
    try {
        const tabs: Tab[] = await chrome.tabs.query({});
        if (!collecting || port !== current || epoch !== collectionEpoch) return;
        if (tabs.length > 10000) { current.disconnect(); return; }
        for (const [id, closed] of closures) { if (knownOpen.delete(id)) send({ type: 'close', id, closed }); dirty.delete(id); }
        closures.clear();
        send({ type: 'begin' }); knownOpen.clear();
        for (const tab of tabs) { const packet = normalized(tab); if (packet) { knownOpen.add(tab.id!); send(packet); } }
        send({ type: 'end' });
        void importRecent();
    } catch { if (port === current) current.disconnect(); }
    finally {
        synchronizing = false;
        if (collecting && epoch !== collectionEpoch) void snapshot();
        else void flush();
    }
}
function integerInRange(value: any, minimum: number, maximum: number): boolean {
    return Number.isInteger(value) && value >= minimum && value <= maximum;
}
async function command(message: any, current: Port): Promise<void> {
    if (port !== current || !message || message.v !== 1) return;
    if (message.type === 'setLabel') {
        if (typeof message.label !== 'string' || !message.label.trim() || message.label.length > 32 || /[\x00-\x1f\x7f]/u.test(message.label)) return;
        const label = message.label.trim().toWellFormed();
        try {
            await chrome.storage.local.set({ label });
            if (port === current) send({ type: 'label', label });
        } catch { if (port === current) send({ type: 'labelError' }); }
        return;
    }
    if (message.type === 'collect' && typeof message.enabled === 'boolean') {
        ++collectionEpoch; collecting = message.enabled; dirty.clear(); closures.clear();
        historyEnabled = message.history !== false;
        if (Number.isSafeInteger(message.since) && message.since >= 0) historyFloor = message.since;
        if (collecting) retry = 1;
        if (collecting) await snapshot();
        return;
    }
    if (message.type === 'restore') {
        if (!Number.isSafeInteger(message.request) || message.request < 1 || typeof message.session !== 'string' || message.session.length > 256 || !webUrl(message.url)) return;
        let status = 'unavailable';
        try {
            const sessions = await chrome.sessions.getRecentlyClosed({ maxResults: 25 });
            if (port !== current) return;
            const exact = sessions.find((item: any) => item.tab?.incognito === false && item.tab.sessionId === message.session && item.tab.url === message.url);
            // Only a verified individual-tab session is restored. Whole-window
            // entries use URL reopening, so no additional tabs open implicitly.
            if (message.session) suppressedSessions.add(message.session);
            let restored: Tab | undefined;
            if (exact) restored = (await chrome.sessions.restore(message.session)).tab;
            else restored = await chrome.tabs.create({ url: message.url, active: true });
            status = 'restored';
            if (restored?.incognito === false && restored.id !== undefined) {
                changed(restored.id);
                await chrome.windows.update(restored.windowId, { focused: true });
                const window = await chrome.windows.get(restored.windowId);
                if (!window.focused) status = 'focus';
            }
        } catch { if (status === 'unavailable' && message.session) suppressedSessions.delete(message.session); }
        // Keep this memory bound; old IDs already disappear from Chrome's recent list.
        if (suppressedSessions.size > 1000) suppressedSessions.delete(suppressedSessions.values().next().value!);
        if (port === current) send({ type: 'result', request: message.request, status });
        return;
    }
    if ((message.type !== 'activate' && message.type !== 'prepare') || !Number.isSafeInteger(message.request) || message.request < 1 ||
        !Number.isInteger(message.id) || message.id < 0 || message.id > 2147483647) return;
    let status = 'unavailable';
    try {
        const tab: Tab = await chrome.tabs.get(message.id);
        if (tab.incognito === false && port === current) {
            await chrome.tabs.update(message.id, { active: true });
            // Resolve again in case the tab moved into another window during activation.
            const latest: Tab = await chrome.tabs.get(message.id);
            if (port !== current || latest.incognito !== false) return;
            if (message.type === 'prepare') {
                const window = await chrome.windows.get(latest.windowId);
                const verified: Tab = await chrome.tabs.get(message.id);
                if (port !== current) return;
                if (verified.incognito === false && verified.active === true && verified.windowId === latest.windowId &&
                    integerInRange(window.left, -100000, 100000) && integerInRange(window.top, -100000, 100000) &&
                    integerInRange(window.width, 1, 32768) && integerInRange(window.height, 1, 32768)) {
                    send({ type: 'prepared', request: message.request, title: (verified.title ?? '').slice(0, 1000).toWellFormed(),
                        left: window.left, top: window.top, width: window.width, height: window.height });
                    return;
                }
                if (port === current) send({ type: 'result', request: message.request, status: 'unavailable' });
                return;
            }
            await chrome.windows.update(latest.windowId, { focused: true });
            const window = await chrome.windows.get(latest.windowId);
            status = window.focused ? 'ok' : 'focus';
            if (status === 'ok' && message.guided === true && port === current &&
                integerInRange(window.left, -100000, 100000) && integerInRange(window.top, -100000, 100000) &&
                integerInRange(window.width, 1, 32768) && integerInRange(window.height, 1, 32768)) {
                send({ type: 'located', request: message.request, title: (latest.title ?? '').slice(0, 1000).toWellFormed(),
                    left: window.left, top: window.top, width: window.width, height: window.height });
            }
        }
    } catch { /* Return a fixed status, never browser errors containing URLs. */ }
    if (port === current) send({ type: 'result', request: message.request, status });
}
// Chrome reports window bounds in its own DIP layout; the host needs each
// display's DIP bounds and DPI to map them onto physical monitors.
async function sendDisplays(): Promise<void> {
    const current = port;
    if (!current) return;
    try {
        const displays: any[] = await chrome.system.display.getInfo();
        if (port !== current) return;
        const usable = displays.filter(d => d?.bounds && Number.isInteger(d.bounds.left) && Number.isInteger(d.bounds.top) &&
            Number.isInteger(d.bounds.width) && Number.isInteger(d.bounds.height) && Number.isInteger(d.dpiX) && d.dpiX >= 48 && d.dpiX <= 960).slice(0, 16);
        if (usable.length === 0) return;
        usable.forEach((d, index) => send({ type: 'display', index, count: usable.length, left: d.bounds.left, top: d.bounds.top,
            width: d.bounds.width, height: d.bounds.height, dpi: d.dpiX, primary: d.isPrimary === true }));
    } catch { /* Layout hints are optional; the host falls back to direct activation. */ }
}
async function connect(): Promise<void> {
    if (port || connecting) return;
    connecting = true;
    try {
        const saved = await chrome.storage.local.get(['profile', 'label']);
        let profile: string = saved.profile;
        if (typeof profile !== 'string' || !/^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(profile)) {
            profile = crypto.randomUUID(); await chrome.storage.local.set({ profile });
        }
        const savedLabel = typeof saved.label === 'string' ? saved.label.slice(0, 32).replace(/[\x00-\x1f\x7f]/gu, '').trim().toWellFormed() : '';
        const label = savedLabel || `Profile ${profile.slice(0, 6)}`;
        const current: Port = chrome.runtime.connectNative('com.hypetabs.bridge');
        port = current;
        current.onMessage.addListener((message: unknown) => { void command(message, current); });
        current.onDisconnect.addListener(() => {
            void chrome.runtime.lastError;
            if (port !== current) return;
            port = undefined; collecting = false; ++collectionEpoch; dirty.clear(); closures.clear(); knownOpen.clear();
            void chrome.alarms.create('reconnect', { delayInMinutes: retry });
            retry = Math.min(5, retry * 2);
        });
        send({ type: 'hello', profile, label });
        void sendDisplays();
    } catch { void chrome.alarms.create('reconnect', { delayInMinutes: 1 }); }
    finally { connecting = false; }
}
chrome.tabs.onCreated.addListener((tab: Tab) => { if (!tab.incognito && tab.id !== undefined) changed(tab.id); });
chrome.tabs.onUpdated.addListener((id: number, delta: any, tab: Tab) => {
    if (!tab.incognito && ('title' in delta || 'url' in delta || 'status' in delta)) changed(id);
});
chrome.tabs.onRemoved.addListener((id: number) => {
    if (!collecting || !knownOpen.has(id)) return;
    closures.set(id, Date.now()); changed(id);
});
chrome.sessions.onChanged.addListener(() => { void importRecent(); });
chrome.tabs.onActivated.addListener((info: { tabId: number }) => changed(info.tabId));
chrome.tabs.onAttached.addListener((id: number) => changed(id));
chrome.tabs.onDetached.addListener((id: number) => changed(id));
chrome.tabs.onMoved.addListener((id: number) => changed(id));
chrome.tabs.onReplaced.addListener((added: number, removed: number) => { changed(removed); changed(added); });
chrome.system.display.onDisplayChanged.addListener(() => { void sendDisplays(); });
chrome.alarms.onAlarm.addListener((alarm: { name: string }) => { if (alarm.name === 'reconnect') void connect(); });
chrome.action.onClicked.addListener(() => { retry = 1; void connect(); });
chrome.runtime.onStartup.addListener(() => { void connect(); });
chrome.runtime.onInstalled.addListener(() => { void connect(); });
void connect();
