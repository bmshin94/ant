'use strict';
const assert = require('node:assert');

function readThree(a, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += a[0] + a[1] + a[2];
  return sum;
}
function writeRead(a, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) { a[0] = a[0] + 1; sum += a[0]; }
  return sum;
}
assert.equal(readThree([1, 2, 3], 30000), 180000);
assert.equal(readThree(Object.freeze([2, 3, 4]), 30000), 270000);
assert.equal(writeRead([0], 30000), 450015000);
assert.throws(() => writeRead(Object.freeze([0]), 1), TypeError);

const first = [1, 2, 3], second = [1, 2, 3];
assert.equal(writeRead(first, 10000), 50015000);
assert.deepEqual(second, [1, 2, 3]);
assert.equal(readThree(second, 1), 6);
assert(Number.isNaN(readThree([1], 1)));
const getter = [1, 2, 3];
let getters = 0;
Object.defineProperty(getter, '1', {get() { getters++; this.length = 2; return 7; }});
assert(Number.isNaN(readThree(getter, 1)));
assert.equal(getters, 1);

function callbackBetweenReads(a, callback, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += a[0] + callback(a) + a[1];
  return sum;
}
assert.equal(callbackBetweenReads([2, 3], () => 0, 30000), 150000);
assert(Number.isNaN(callbackBetweenReads([2, 3], a => { a.length = 1; return 0; }, 1)));

// The old argument can remain on the expression stack while the parameter is
// replaced. It must not create header facts for the new parameter value.
function replacedArgument(a, b, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += a[(a = b, 0)] + a[0];
  return sum;
}
assert.equal(replacedArgument([1], [9], 30000), 539992);
function capturedArgument(a, b, n) {
  function key() { a = b; return 0; }
  let sum = 0;
  for (let i = 0; i < n; i++) sum += a[key()] + a[0];
  return sum;
}
assert.equal(capturedArgument([1], [9], 30000), 539992);
console.log('array guard reuse: passed');
