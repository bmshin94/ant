const assert = require('node:assert');

function literal() { return [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]; }
const expected = Array.from({length: 10}, (_, i) => i);
const mutations = [
  a => { a[2] = 99; },
  a => { a[2] = {value: 99}; },
  a => { delete a[2]; },
  a => { a.length = 2; },
  a => { a.length = 40; a[30] = 99; },
  a => a.push(10),
  a => a.pop(),
  a => a.shift(),
  a => a.unshift(99),
  a => a.splice(2, 3, 99),
  a => a.splice(2, 0, 99, 98, 97),
  a => a.reverse(),
  a => a.sort((x, y) => y - x),
  a => a.fill(99, 2, 5),
  a => a.copyWithin(2, 0, 4),
  a => Object.defineProperty(a, '2', {value: 99, writable: false}),
  a => Object.defineProperty(a, '2', {get() { return 99; }, configurable: true}),
  a => Reflect.set(a, '2', 99),
  a => Reflect.deleteProperty(a, '2'),
];

for (let round = 0; round < 100; round++) {
  for (const mutate of mutations) {
    const a = literal(), b = literal();
    const reference = Array.from({length: 10}, (_, i) => i);
    assert.notStrictEqual(a, b);
    mutate(a);
    mutate(reference);
    assert.deepStrictEqual(a, reference);
    assert.deepStrictEqual(b, expected);
    assert.deepStrictEqual(literal(), expected);
  }
}

// Hot numeric writes must bail to detachment before taking the JIT store path.
function overwrite(a, n) { a[3] = n; return a[3]; }
for (let i = 0; i < 20000; i++) {
  const a = literal();
  assert.strictEqual(overwrite(a, i), i);
}
assert.deepStrictEqual(literal(), expected);

function increment(a) { return a[0]++; }
function readThenWrite(a) { const old = a[1]; a[1] = old + 1; return a[1]; }
for (let i = 0; i < 20000; i++) {
  const a = literal();
  assert.strictEqual(increment(a), 0);
  assert.strictEqual(a[0], 1);
  assert.strictEqual(readThenWrite(a), 2);
}
assert.deepStrictEqual(literal(), expected);

// Callback mutations must not leave an iteration reading cached shared backing.
let a = literal();
assert.deepStrictEqual(a.flatMap((v, i, src) => {
  if (i === 0) src[1] = 99;
  return [v];
}), [0, 99, 2, 3, 4, 5, 6, 7, 8, 9]);
a = literal();
assert.deepStrictEqual(a.map((v, i, src) => {
  if (i === 0) src[1] = 99;
  return v;
}), [0, 99, 2, 3, 4, 5, 6, 7, 8, 9]);

function dynamic(value) { return [value, 1, 2]; }
function conditional(value) { return [value ? value : 0, 1, 2]; }
for (let i = 0; i < 10000; i++) {
  assert.strictEqual(dynamic(i)[0], i);
  assert.strictEqual(conditional(i)[0], i);
}

const frozen = Object.freeze(literal());
assert.throws(() => { 'use strict'; frozen[0] = 99; }, TypeError);
assert.deepStrictEqual(literal(), expected);

function mixedLiteral() { return [true, false, null, 1.5, 1e309]; }
for (let i = 0; i < 20000; i++) {
  const a = mixedLiteral(), b = mixedLiteral();
  assert.strictEqual(a[0], true);
  assert.strictEqual(a[1], false);
  assert.strictEqual(a[2], null);
  assert.strictEqual(a[3], 1.5);
  assert.strictEqual(a[4], Infinity);
  a[0] = false;
  assert.strictEqual(b[0], true);
}
console.log('PASS shared literal identity, mutation, callbacks and JIT stores');
