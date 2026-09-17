const assert = {
  equal(actual, expected) {
    if (actual !== expected) throw new Error(`Expected ${expected}, got ${actual}`);
  },
  throws(callback, expected) {
    try { callback(); } catch (error) {
      if (error instanceof expected) return;
      throw error;
    }
    throw new Error(`Expected ${expected.name}`);
  },
};
const registry = new FinalizationRegistry(() => {});
const callback = () => {};
const reference = new WeakRef(callback);
assert.equal(registry.register(callback, () => {}, reference), undefined);
assert.equal(registry.unregister(reference), true);

const targets = [{}, [], () => {}, function ordinary() {}, callback.bind(null), Math.max, Symbol('local')];
for (const target of targets) {
  assert.equal(registry.register(target, 'held', target), undefined);
  assert.equal(registry.unregister(target), true);
  assert.equal(registry.unregister(target), false);
  assert.throws(() => registry.register(target, target), TypeError);
}
for (const primitive of [null, undefined, true, 1, 'text', Symbol.for('registered')]) {
  assert.throws(() => registry.register(primitive, 'held'), TypeError);
  assert.throws(() => registry.unregister(primitive), TypeError);
  if (primitive !== undefined) assert.throws(() => registry.register({}, 'held', primitive), TypeError);
}
assert.equal(registry.register({}, 'held', undefined), undefined);
console.log('FinalizationRegistry weak targets passed');
