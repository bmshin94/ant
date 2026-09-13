function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function syntaxError(run, label) {
  try {
    run();
  } catch (error) {
    assert(error instanceof SyntaxError, label + ": expected SyntaxError, got " + error);
    return;
  }
  throw new Error(label + ": expected a SyntaxError");
}

const duplicatePatterns = [
  "[id, id]",
  "{id, id}",
  "{first: id, second: id}",
  "[id, {nested: id}]",
  "{nested: [id]}, id",
  "id, [id]",
  "[id], id",
  "id, id = 1",
  "id = 1, id",
  "id, id, other = 1",
  "id, ...id",
  "[id, ...id]",
  "{id, ...id}",
  "...[id, id]",
  "[id = 1, id]",
  "{id = 1}, id",
  "[id], {id}",
  "[id, \\u0069d]",
];

for (const params of duplicatePatterns) {
  const source = "(function (" + params + ") {})";
  syntaxError(() => eval(source), "eval " + params);
  syntaxError(() => Function(params, ""), "Function " + params);
  syntaxError(() => eval("(" + params + ") => {}"), "arrow " + params);
}

for (const source of [
  "(id, id) => id",
  "async (id, id) => id",
  "({method(id, id) {}})",
  "({*method(id, id) {}})",
  "({async method(id, id) {}})",
  "({async *method(id, id) {}})",
  "(class { method(id, id) {} })",
  "(function (id, id) { 'use strict'; })",
  "'use strict'; (function ([id, id]) {})",
  "'use strict'; (function ({nested: [eval]}) {})",
  "'use strict'; (function ([arguments]) {})",
  "(function ([id]) { 'use strict'; })",
]) {
  syntaxError(() => eval(source), source);
}

// Literal eval has a separate compile-time eligibility check. Its syntax error
// must remain catchable at the eval call, before any source side effects occur.
syntaxError(() => eval("(function ([id, id]) {})"), "literal eval");
let ran = false;
syntaxError(() => eval("ran = true; (function ([id, id]) {})"), "early error");
assert(!ran, "invalid parameters must be rejected before evaluation");

// Eligibility checks must carry a function's strictness into nested functions.
syntaxError(() => eval('(function () { "use strict"; return (function ([arguments]) {}); })'),
  "nested strict array binding in literal eval");
syntaxError(() => eval('(function () { "use strict"; return (function ({x: eval}) {}); })'),
  "nested strict object binding in literal eval");
syntaxError(() => eval('(function () { "use strict"; return (function (id, id) {}); })'),
  "nested strict simple duplicates in literal eval");
syntaxError(() => eval('(() => { "use strict"; return (() => (function ([arguments]) {})); })'),
  "strictness through multiple nested functions in literal eval");

function uncalledInvalidEval() {
  eval('(function () { "use strict"; return (function ([arguments]) {}); })');
}
assert(typeof uncalledInvalidEval === "function", "uncalled eval must not reject its enclosing script");

const siblings = eval('[(function () { "use strict"; }), (function ([arguments]) { return arguments; })]');
assert(siblings[1]([7]) === 7, "strictness must not leak between sibling functions");

// Only bound names count: function names, property keys and default expressions
// can reuse a parameter name without introducing another binding.
assert((function d([d]) { return d; })([true]) === true, "named function parameter");
assert((function ([id], other = id) { return other; })([7]) === 7, "default reference");
assert((function ({id: first, id: second}) { return first + second; })({id: 3}) === 6,
  "repeated property keys");
assert((function ({id: value}, id) { return value + id; })({id: 3}, 4) === 7,
  "property key matching another parameter");
assert((function (id, {[id]: value}) { return value; })("key", {key: 9}) === 9,
  "computed property key");
assert((function (id = function (id) { return id; }) { return id(5); })() === 5,
  "nested default function has its own bindings");
(function () {
  "use strict";
  assert((function ({eval: value, arguments: other}) { return value + other; })({eval: 1, arguments: 2}) === 3,
    "strict restricted names used only as property keys");
})();

// Sloppy ordinary, generator and async functions still permit simple duplicates.
assert((function (id, id) { return id; })(1, 2) === 2, "sloppy duplicate parameters");
assert(Function("id", "id", "return id")(1, 2) === 2, "sloppy Function parameters");
assert((function* (id, id) { yield id; })(1, 2).next().value === 2, "sloppy generator parameters");
assert(typeof eval("(async function (id, id) {})") === "function", "sloppy async parameters");
assert(typeof eval("(async function* (id, id) {})") === "function", "sloppy async generator parameters");

// A single pattern can contain many more bindings than the parameter count.
for (const count of [0, 1, 16, 17, 300]) {
  const names = Array.from({length: count}, (_, i) => "p" + i);
  const params = "[" + names.join(",") + "]";
  assert(typeof Function(params, "") === "function", "unique pattern with " + count + " bindings");
  if (count > 0)
    syntaxError(() => Function(params, "p" + (count - 1), ""), "duplicate after " + count + " bindings");
}

console.log("duplicate parameter tests passed");
