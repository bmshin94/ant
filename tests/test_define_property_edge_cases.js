// Test edge cases for Object.defineProperty()

import assert from 'node:assert';

console.log("Test 1: Error when called with < 3 arguments");
assert.throws(() => Object.defineProperty({}, "foo"), TypeError);

console.log("\nTest 2: Error when first argument is not an object");
assert.throws(() => Object.defineProperty(42, "foo", { value: 1 }), TypeError);

console.log("\nTest 3: Error when descriptor is not an object");
assert.throws(() => Object.defineProperty({}, "foo", "not an object"), TypeError);

console.log("\nTest 4: Error when mixing data and accessor descriptors");
assert.throws(() => {
  Object.defineProperty({}, "foo", {
    value: 42,
    get: function() { return 0; }
  });
}, TypeError);

console.log("\nTest 5: Can define property with just value");
const obj5 = {};
Object.defineProperty(obj5, "test", { value: 100 });
assert.strictEqual(obj5.test, 100);

console.log("\nTest 6: Can define an own __proto__ property without changing the prototype");
const obj6 = {};
const prototype = Object.getPrototypeOf(obj6);
const value = { marker: "own property" };
assert.strictEqual(Object.defineProperty(obj6, "__proto__", { value }), obj6);
assert.ok(Object.hasOwn(obj6, "__proto__"));
assert.strictEqual(obj6.__proto__, value);
assert.strictEqual(Object.getPrototypeOf(obj6), prototype);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(obj6, "__proto__"), {
  value,
  writable: false,
  enumerable: false,
  configurable: false
});

console.log("\nTest 7: Works with arrays");
const arr = [1, 2, 3];
Object.defineProperty(arr, "myProp", { value: "test" });
assert.strictEqual(arr.myProp, "test");

console.log("\nTest 8: Works with functions");
function myFunc() {}
Object.defineProperty(myFunc, "customProp", { value: 42 });
assert.strictEqual(myFunc.customProp, 42);

console.log("\nAll edge case tests passed!");
