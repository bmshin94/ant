const assert = require('node:assert');

function first() { this.out.value = this.input.value + 1; }
function second() { this.out.value = this.input.value + 2; }
function third() { this.out.value = this.input.value + 3; }
function fourth() { this.out.value = this.input.value + 4; }
function fifth() { this.out.value = this.input.value + 5; }

function targetSetDriver(items, count) {
  let sum = 0;
  for (let i = 0; i < count; i++) {
    const item = items[i % items.length];
    item.run();
    sum += item.out.value;
  }
  return sum;
}

const items = [first, second, third].map(run => ({run, input: {value: 10}, out: {value: 0}}));
assert.equal(targetSetDriver(items, 30000), 360000);
items.push({run: fourth, input: {value: 10}, out: {value: 0}});
assert.equal(targetSetDriver(items, 40000), 500000);
items.push({run: fifth, input: {value: 10}, out: {value: 0}});
assert.equal(targetSetDriver(items, 50000), 650000);

// A new bound target must use its bound receiver and preserve generic fallback.
const boundReceiver = {input: {value: 100}, out: items[0].out};
items[0].run = first.bind(boundReceiver);
assert.equal(targetSetDriver(items.slice(0, 1), 1000), 101000);

// Closures sharing function metadata must keep their own captured cells.
function makeRun(increment) { return function () { this.out.value = increment; }; }
items[0].run = makeRun(17);
items[1].run = makeRun(29);
assert.equal(targetSetDriver(items.slice(0, 2), 20000), 460000);

// A getter observed after the call must not replay a completed callee.
let calls = 0;
items[0].run = function () { calls++; };
Object.defineProperty(items[0].out, 'value', {get() { return 7; }, configurable: true});
assert.equal(targetSetDriver(items.slice(0, 1), 1000), 7000);
assert.equal(calls, 1000);

let outputs = 0, inputs = 0;
const nested = {
  input() { inputs++; return this.source; },
  output() { outputs++; return this.destination; },
  source: {value: 19}, destination: {value: 0},
  execute() { this.output().value = this.input().value; }
};
function nestedDriver(n) { for (let i = 0; i < n; i++) nested.execute(); }
nestedDriver(20000);
assert.equal(inputs, 20000);
assert.equal(outputs, 20000);
assert.equal(nested.destination.value, 19);
const thrown = new Error('input failed');
nested.input = function () { inputs++; throw thrown; };
assert.throws(() => nestedDriver(1), error => error === thrown);
assert.equal(outputs, 20001);
assert.equal(inputs, 20001);

let fetched = 0;
const assignments = {
  out: {},
  fetch() { fetched++; return this.out; },
  run() { this.fetch().left = 1; this.fetch().right = 2; },
};
function assignmentDriver(obj, count) { for (let i = 0; i < count; i++) obj.run(); }
assignmentDriver(assignments, 20000);
assert.equal(fetched, 40000);
assert.equal(assignments.out.left, 1);
assert.equal(assignments.out.right, 2);

let lookups = 0, invoked = 0;
let chosen = function one() { invoked++; return 1; };
const getterMethod = {get run() { lookups++; return chosen; }};
function lookupDriver(obj, count) {
  let sum = 0;
  for (let i = 0; i < count; i++) sum += obj.run();
  return sum;
}
assert.equal(lookupDriver(getterMethod, 20000), 20000);
chosen = function two() { invoked++; return 2; };
assert.equal(lookupDriver(getterMethod, 20000), 40000);
assert.equal(lookups, 40000);
assert.equal(invoked, 40000);

function capturedDriver(list, count) {
  let sum = 0;
  for (let i = 0; i < count; i++) {
    const obj = list[i % list.length];
    obj.run();
    sum += obj.out.value;
  }
  return sum;
}
const captured = [makeRun(17), makeRun(29), function () { this.out.value = 99; }]
  .map(run => ({run, out: {value: 0}}));
assert.equal(capturedDriver(captured, 30000), 1450000);

function branchWrite() {
  if (this.mode == 1) this.out.value = this.input + 2;
  else this.out.value = this.input - 2;
}
const branched = {run: branchWrite, mode: 1, out: {value: 0}, input: 20};
function branchDriver(obj, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) { obj.run(); sum += obj.out.value; }
  return sum;
}
assert.equal(branchDriver(branched, 20000), 440000);
branched.mode = 2;
assert.equal(branchDriver(branched, 20000), 360000);
let coercions = 0;
branched.mode = {valueOf() { coercions++; return 1; }};
assert.equal(branchDriver(branched, 1), 22);
assert.equal(coercions, 1);

function capturedLookup(n) {
  let lookups = 0;
  function one() { return 1; }
  function two() { return 2; }
  const obj = {get run() { lookups++; return lookups < n / 2 ? one : two; }};
  let sum = 0;
  for (let i = 0; i < n; i++) sum += obj.run();
  return [lookups, sum];
}
assert.deepEqual(capturedLookup(30000), [30000, 45001]);

function thinCaller(obj) { return obj.run(); }
const thin = {run() { return 1; }};
for (let i = 0; i < 1000; i++) assert.equal(thinCaller(thin), 1);
thin.run = function () { return 2; };
for (let i = 0; i < 1000; i++) assert.equal(thinCaller(thin), 2);
console.log('polymorphic method calls: passed');
