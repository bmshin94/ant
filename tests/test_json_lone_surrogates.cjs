// JSON.parse must accept lone surrogate escapes and raw lone surrogates (ECMA-262 §25.5.1)
// while still rejecting raw control characters and malformed escapes.
function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function units(s) {
  return Array.from({ length: s.length }, (_, i) => s.charCodeAt(i).toString(16).padStart(4, '0')).join(',');
}

function rejects(src) {
  try { JSON.parse(src); } catch (e) { return e instanceof SyntaxError || e.name === 'SyntaxError'; }
  return false;
}

for (let cu = 0xd800; cu <= 0xdfff; cu++) {
  const hex = cu.toString(16);
  const s = JSON.parse('"\\u' + hex + '"');
  assert(s.length === 1 && s.charCodeAt(0) === cu, 'lone escape \\u' + hex);
  const upper = JSON.parse('"\\u' + hex.toUpperCase() + '"');
  assert(upper.charCodeAt(0) === cu, 'lone escape upper \\u' + hex);
}

const pairings = [
  ['\\udc00\\ud800', 'dc00,d800'],
  ['\\ud800\\ud800', 'd800,d800'],
  ['\\ud800A', 'd800,0041'],
  ['\\ud800\\n', 'd800,000a'],
  ['\\ud800\\"', 'd800,0022'],
  ['x\\udfff\\ud83d\\udc1cy', '0078,dfff,d83d,dc1c,0079'],
];
for (const [src, want] of pairings) assert(units(JSON.parse('"' + src + '"')) === want, 'pairing ' + src);

assert(JSON.parse('"\\ud83d\\udc1c"') === '🐜', 'well-formed pair');
assert(JSON.parse('"\\\\ud800"') === '\\ud800', 'escaped backslash is literal');

const seen = [];
const obj = JSON.parse('{"\\ud800":"\\udfff"}', (key, value) => {
  seen.push(units(key) + '=' + (typeof value === 'string' ? units(value) : value));
  return value;
});
assert(units(Object.keys(obj)[0]) === 'd800', 'lone surrogate key');
assert(seen[0] === 'd800=dfff', 'reviver key and value: ' + seen[0]);

const high = '🐜'.slice(0, 1);
const low = '🐜'.slice(1);
assert(JSON.parse(JSON.stringify(high)) === high, 'roundtrip lone high');
assert(JSON.parse(JSON.stringify(low)) === low, 'roundtrip lone low');
assert(JSON.parse(JSON.stringify({ a: [high + 'x', low] })).a[0] === high + 'x', 'roundtrip nested');
assert(JSON.parse('["' + high + '"]')[0] === high, 'raw unescaped lone surrogate');

const rows = [];
const text = 'Words with unicode → café █ 🐜\n'.repeat(4);
for (const part of text.split('\n')) for (let o = 0; o < part.length; o += 29) rows.push(part.slice(o, o + 29));
const back = JSON.parse(JSON.stringify(rows));
assert(back.length === rows.length && back.every((r, i) => r === rows[i]), 'sliced surrogate rows roundtrip');

const bad = [
  '"\\ud800',
  '"\\uD80"',
  '"\\uD80x"',
  '"\\uZZZZ"',
  '"\\x41"',
  '[1,]',
  '[1] x',
  '"a\nb"',
  '"a\u0001b"',
  '{"\\ud800":"\u0002"}',
  '"\\ud800\u001b"',
  '["\\ud800", "\t"]',
];
for (const src of bad) assert(rejects(src), 'should reject ' + JSON.stringify(src));

console.log('PASS');
