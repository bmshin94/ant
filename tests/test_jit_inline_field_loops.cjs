const assert = require('node:assert');
function append(queue) {
  this.link = null;
  if (queue == null) return this;
  var peek, next = queue;
  while ((peek = next.link) != null) next = peek;
  next.link = this;
  return queue;
}
function invoke(node, head) {
  // Keep this numeric-loop caller in the ordinary JIT, where its method call
  // can inline the field-walking callee.
  for (let once = 0; once < 1; once++) return node.append(head);
}
for (let i = 0; i < 20000; i++) {
  const tail = {link: null}, head = {link: tail}, node = {append, link: null};
  assert.equal(invoke(node, head), head);
  assert.equal(tail.link, node);
  assert.equal(node.link, null);
}
const empty = {append};
assert.equal(invoke(empty, null), empty);
assert.equal(invoke(empty, undefined), empty);

let clears = 0, reads = 0, linked;
const node = {append};
Object.defineProperty(node, 'link', {get() { return linked; }, set(value) { clears++; linked = value; }});
const last = {link: null};
const middle = {get link() { reads++; return last; }};
const head = {link: middle};
assert.equal(invoke(node, head), head);
assert.equal(clears, 1);
assert.equal(reads, 1);
assert.equal(last.link, node);

const thrown = new Error('link getter failed');
const bad = {link: {get link() { reads++; throw thrown; }}};
assert.throws(() => invoke(node, bad), error => error === thrown);
assert.equal(clears, 2);
assert.equal(reads, 2);
console.log('inline field loops: passed');
