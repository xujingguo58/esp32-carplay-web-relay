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

process.stdout.write('embedded web JS syntax, DTLS role, and touch-rectangle tests passed\n');
