import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { Script, runInNewContext } from 'node:vm';

const source = readFileSync(new URL('../components/web_gateway/web_gateway.c', import.meta.url), 'utf8');
const htmlStart = source.indexOf('static const char INDEX_HTML[] =');
const htmlEnd = source.indexOf('";\n\nstatic bool websocket_is_active', htmlStart);
assert.notEqual(htmlStart, -1, 'embedded HTML exists');
assert.notEqual(htmlEnd, -1, 'embedded HTML literal terminates');

const htmlCStrings = source.slice(htmlStart, htmlEnd + 1).match(/"(?:\\.|[^"\\])*"/g);
assert.ok(htmlCStrings?.length, 'extract HTML from the actual firmware string literals');
const html = htmlCStrings.map((literal) => JSON.parse(literal)).join('');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(scriptMatch, 'embedded page contains its script');
new Script(scriptMatch[1]);

const helperStart = source.indexOf('"function answerSetupForOffer');
const helperEnd = source.indexOf('"async function offer', helperStart);
assert.notEqual(helperStart, -1, 'SDP-role helper exists');
assert.notEqual(helperEnd, -1, 'SDP-role and touch helpers precede the offer handler');
const helperCStrings = source.slice(helperStart, helperEnd).match(/"(?:\\.|[^"\\])*"/g);
assert.ok(helperCStrings?.length, 'extract helpers from actual embedded HTML strings');
const helperSource = helperCStrings.map((literal) => JSON.parse(literal)).join('');
const { answerSetupForOffer, normalizedVideoPoint } = runInNewContext(
  `${helperSource}; ({ answerSetupForOffer, normalizedVideoPoint })`,
  {},
);

assert.equal(
  answerSetupForOffer('v=0\r\na=setup:actpass\r\n', 'v=0\r\na=setup:active\r\n'),
  'v=0\r\na=setup:passive\r\n',
  'actpass offer selects a passive browser answer',
);
assert.equal(
  answerSetupForOffer('v=0\r\na=setup:active\r\n', 'v=0\r\na=setup:passive\r\n'),
  'v=0\r\na=setup:passive\r\n',
  'active offer keeps its passive answer',
);
assert.equal(
  answerSetupForOffer('v=0\r\na=setup:passive\r\n', 'v=0\r\na=setup:active\r\n'),
  'v=0\r\na=setup:active\r\n',
  'passive offer keeps its active answer',
);
assert.equal(
  answerSetupForOffer(
    'v=0\r\na=setup:actpass\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\na=setup:active\r\n',
    'v=0\r\na=setup:active\r\n',
  ),
  'v=0\r\na=setup:active\r\n',
  'mixed offer roles are not rewritten globally',
);
assert.equal(
  answerSetupForOffer('v=0\r\na=setup:actpass\r\na=setup:holdconn\r\n',
    'v=0\r\na=setup:active\r\n'),
  'v=0\r\na=setup:active\r\n',
  'unknown and holdconn setup roles are not rewritten',
);
assert.equal(
  answerSetupForOffer('v=0\r\n', 'v=0\r\na=setup:active\r\n'),
  'v=0\r\na=setup:active\r\n',
  'offers without setup attributes are left untouched',
);

const rect = { left: 10, top: 20, width: 400, height: 300 };
assert.deepEqual(
  { ...normalizedVideoPoint(210, 170, rect, 1920, 1080, 'down') },
  { x: 0.5, y: 0.5 },
  'landscape video maps through the visible 16:9 rectangle',
);
assert.equal(
  normalizedVideoPoint(210, 40, rect, 1920, 1080, 'down'),
  null,
  'a down event in a top letterbox is rejected',
);
assert.equal(
  normalizedVideoPoint(210, 310, rect, 1920, 1080, 'move').y,
  1,
  'a move beyond the displayed picture clamps to its bottom edge',
);
assert.equal(
  normalizedVideoPoint(0, 170, rect, 1920, 1080, 'up').x,
  0,
  'an up event beyond the displayed picture clamps to its left edge',
);
assert.deepEqual(
  { ...normalizedVideoPoint(210, 170, rect, 1080, 1920, 'down') },
  { x: 0.5, y: 0.5 },
  'portrait video maps through the visible pillarboxed rectangle',
);
assert.equal(
  normalizedVideoPoint(20, 170, rect, 1080, 1920, 'down'),
  null,
  'a down event in a side pillarbox is rejected',
);
assert.deepEqual(
  { ...normalizedVideoPoint(210, 170, rect, 0, 0, 'down') },
  { x: 0.5, y: 0.5 },
  'dimensions before the first decoded frame use the video element bounds',
);
assert.equal(
  normalizedVideoPoint(210, 170, { ...rect, width: 0 }, 1920, 1080, 'down'),
  null,
  'a zero-sized video element has no touchable area',
);

// Exercise the actual page script through refresh/close races and a dead link.
const nodes = new Map();
const sockets = [];
const timeouts = new Map();
const intervals = [];
let nextTimer = 1;
let now = 1000;
class MockWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  constructor() { this.readyState = 0; this.sent = []; this.closeCalls = 0; sockets.push(this); }
  send(data) { this.sent.push(JSON.parse(data)); }
  close() { this.closeCalls++; this.readyState = 3; }
}
class MockPeer {
  async setRemoteDescription(description) { this.remoteDescription = description; }
  async createAnswer() { return { type: 'answer', sdp: 'v=0\r\n' }; }
  async setLocalDescription(description) { this.localDescription = description; }
  async getStats() { return new Map(); }
  close() { this.closed = true; }
  iceGatheringState = 'complete';
}
const page = runInNewContext(`${scriptMatch[1]}; ({connect,offer,currentPeer:()=>pc,currentSocket:()=>ws})`, {
  document: { querySelector(selector) {
    if (!nodes.has(selector)) nodes.set(selector, { addEventListener() {}, value: '', hidden: false });
    return nodes.get(selector);
  } },
  location: { protocol: 'http:', host: '192.168.4.1' },
  WebSocket: MockWebSocket,
  RTCPeerConnection: MockPeer,
  Date: { now: () => now },
  setTimeout(callback, delay) { const id = nextTimer++; timeouts.set(id, { callback, delay }); return id; },
  clearTimeout(id) { timeouts.delete(id); },
  setInterval(callback, delay) { intervals.push({ callback, delay }); },
  addEventListener() {},
  fetch: async () => ({ json: async () => ({}) }),
});
const first = sockets[0];
first.readyState = MockWebSocket.OPEN;
first.onopen();
await page.offer('v=0\r\n');
const oldPeer = page.currentPeer();
nodes.get('#pairing').hidden = true;
page.connect();
const second = sockets[1];
second.readyState = MockWebSocket.OPEN;
second.onopen();
assert.equal(nodes.get('#pairing').hidden, false, 'manual reconnect restores the pairing form');
await page.offer('v=0\r\n');
const newPeer = page.currentPeer();
const newState = nodes.get('#state').textContent;
first.onclose();
first.onmessage({ data: JSON.stringify({ type: 'state', payload: 'stale' }) });
oldPeer.onconnectionstatechange();
assert.equal(page.currentPeer(), newPeer, 'late old-socket close preserves the new peer');
assert.equal(nodes.get('#state').textContent, newState, 'old socket/peer events cannot overwrite current state');
second.close();
second.onclose();
assert.equal(newPeer.closed, true, 'disconnect closes the matching media peer');
const retries = [...timeouts.entries()].filter(([, timer]) => timer.delay === 2000);
assert.equal(retries.length, 1, 'disconnect schedules one automatic reconnect');
timeouts.delete(retries[0][0]);
retries[0][1].callback();
const third = sockets[2];
third.readyState = MockWebSocket.OPEN;
third.onopen();
const heartbeat = intervals.find((timer) => timer.delay === 5000);
heartbeat.callback();
assert.equal(third.sent.at(-1).type, 'ping', 'heartbeat runs before pairing too');
now += 21000;
heartbeat.callback();
assert.equal(third.closeCalls, 1, 'silent server closes the link so reconnect can run');
const retryCount = () => [...timeouts.values()].filter((timer) => timer.delay === 2000).length;
const retriesBeforePolicyClose = retryCount();
third.onclose({ code: 1008 });
assert.equal(retryCount(), retriesBeforePolicyClose, 'capacity rejection must not create a reconnect storm');
assert.match(nodes.get('#state').textContent, /上限/, 'capacity rejection gives a visible reason');

process.stdout.write('embedded web JS syntax, DTLS role, touch rectangle, reconnect races and heartbeat tests passed\n');
