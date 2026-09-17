const assert = require('node:assert');

if (process.argv[2] !== '--child') {
  const { spawnSync } = require('node:child_process');
  const run = spawnSync(process.execPath, [__filename, '--child'], {
    encoding: 'utf8',
    env: { ...process.env, ANT_JIT_SSA: '1', ANT_DEBUG: 'dump/vm:jit dump/vm:op-warn' },
    timeout: 30000,
    maxBuffer: 32 * 1024 * 1024,
  });
  assert.strictEqual(run.status, 0, String(run.error || run.stderr));
  assert.match(run.stdout, /PASS SSA execution/);
  assert.match(run.stderr, /ssa_ssaSum_[^\n]*:\s*module/,
    'must execute the SSA compiler rather than pass through the ordinary JIT');
  assert.match(run.stderr, /ssa_ssaOSR_[^\n]*:\s*module/);
  assert.match(run.stderr, /ssa_ssaStrictMatrix_[^\n]*:\s*module/);
  assert.doesNotMatch(run.stderr, /osr entry-rejected func=ssaOSR/);
  console.log('PASS SSA execution and compiler routing');
} else {
  function ssaSum(values, count) {
    var sum = 0;
    for (var i = 0; i < count; i++) sum += values[i];
    return sum;
  }
  function ssaSwap(a, b, count) {
    while (count > 0) {
      var previous = a;
      a = b;
      b = previous;
      count--;
    }
    return a * 10 + b;
  }
  const numbers = [1, 2, 3, 4, 5, 6, 7, 8];
  function ssaOSR(n) {
    var sum = 17;
    for (var i = 0; i < n; i++) sum += i;
    return sum;
  }
  assert.strictEqual(ssaOSR(20000), 199990017);
  for (var round = 0; round < 300; round++) {
    assert.strictEqual(ssaSum(numbers, numbers.length), 36);
    assert.strictEqual(ssaSwap(3, 7, 7), 73);
    assert.strictEqual(ssaSwap(3, 7, 8), 37);
  }
  // Fail an arithmetic guard after two loop iterations. Resuming from entry
  // would discard the already accumulated value and replay earlier accesses.
  let reads = 0;
  const mixed = [1, 2, 'x', 3];
  Object.defineProperty(mixed, 3, { get() { reads++; return 3; } });
  assert.strictEqual(ssaSum(mixed, 4), '3x3');
  assert.strictEqual(reads, 1);
  assert.strictEqual(ssaSum(numbers, 0), 0);
  assert.throws(() => ssaSum([1, Symbol('bad')], 2), TypeError);

  function ssaMutate(a, b, call) {
    var old = a.x;
    b.x = 7;
    call();
    return old + a.x;
  }
  const object = { x: 2 };
  for (let i = 0; i < 300; i++) {
    object.x = 2;
    assert.strictEqual(ssaMutate(object, object, () => { object.x = 9; }), 11);
  }
  function ssaWrite(array, end) {
    'use strict';
    var sum = 0;
    for (var i = 0; i < end; i++) { array[i] = i + 1; sum += array[i]; }
    return sum;
  }
  const writable = Array.from({ length: 16 }, () => 0);
  for (let i = 0; i < 300; i++) assert.strictEqual(ssaWrite(writable, 16), 136);
  assert.strictEqual(ssaWrite([], 0), 0);
  assert.strictEqual(ssaWrite([], 3), 6);
  assert.throws(() => ssaWrite(Object.freeze([1, 2]), 2), TypeError);

  const receiver = { bias: 3 };
  const implementations = [
    function first(x) { var s=0; for(var i=0;i<3;i++) s+=x+this.bias; return s; },
    function second(x) { return x+this.bias+1; },
  ];
  const targets = implementations.map(run => ({ run, bias: 3 }));
  function ssaDispatch(target, n) {
    var sum=0;
    for(var i=0;i<n;i++) sum+=target.run(i);
    return sum;
  }
  for (let i=0; i<400; i++) assert.strictEqual(ssaDispatch(targets[i&1], 5), i&1 ? 30 : 75);
  // Bound receivers and arguments must not enter a specialization with the
  // unbound call's receiver or argument vector.
  assert.strictEqual(ssaDispatch({ run: implementations[1].bind(receiver, 10) }, 5), 70);

  function ssaProperties(value, count, mutate) {
    var sum=0;
    for(var i=0;i<count;i++) {
      sum += value.x + value.y;
      if (i===1 && mutate) mutate(value);
    }
    return sum;
  }
  const fields = { x: 2, y: 3 };
  for(let i=0;i<300;i++) assert.strictEqual(ssaProperties(fields, 4), 20);
  let getterCalls=0;
  assert.strictEqual(ssaProperties(fields, 4, value => {
    Object.defineProperty(value, 'x', { configurable: true, get() { getterCalls++; return 7; } });
  }), 30);
  assert.strictEqual(getterCalls, 2);

  let captured = Array.from({ length: 8 }, (_, i) => i+1);
  function ssaCaptured(end) {
    var sum=0;
    for(var i=0;i<end;i++) sum+=captured[i];
    return sum;
  }
  for(let i=0;i<300;i++) assert.strictEqual(ssaCaptured(8), 36);
  captured = [10,20,30];
  assert.strictEqual(ssaCaptured(3), 60);
  captured = [];
  let indexGetterCalls=0;
  Object.defineProperty(captured, 0, { get() { indexGetterCalls++; return 5; } });
  assert.strictEqual(ssaCaptured(0), 0);
  assert.strictEqual(indexGetterCalls, 0);
  assert.strictEqual(ssaCaptured(1), 5);
  assert.strictEqual(indexGetterCalls, 1);

  function ssaCompare(a, b, count) {
    var visits=0, matches=0;
    for(var i=0;i<count;i++) { visits++; if(a==b) matches++; }
    return visits*1000+matches;
  }
  let coercions=0;
  const left={ [Symbol.toPrimitive]() { coercions++; return 5; } }, right={};
  for(let i=0;i<300;i++) assert.strictEqual(ssaCompare(left, right, 8), 8000);
  assert.strictEqual(coercions, 0);
  assert.strictEqual(ssaCompare(left, left, 8), 8008);
  assert.strictEqual(ssaCompare('5', 5, 8), 8008);
  assert.strictEqual(ssaCompare(left, 5, 8), 8008);
  assert.strictEqual(coercions, 8);

  // Exercise the generic native equality path after warming with objects.
  // In particular, -Infinity must not be mistaken for an object tag, and
  // strings/BigInts compare by value even when their storage differs.
  function ssaStrictMatrix(a, b, count) {
    var total=0;
    for(var i=0;i<count;i++) {
      if(a===b) total++;
      else if(a!==b) total+=2;
    }
    return total;
  }
  const array=[], fn=()=>{}, symbol=Symbol('x');
  const equalityCases=[
    [0,-0,true], [NaN,NaN,false], [Infinity,Infinity,true], [-Infinity,Infinity,false],
    [1,1,true], [1,2,false], ['same',['sa','me'].join(''),true], ['a','b',false],
    [1n,BigInt('1'),true], [1n,2n,false], [1n,1,false], ['1',1,false], [true,1,false],
    [true,true,true], [false,false,true], [false,true,false], [null,null,true],
    [undefined,undefined,true], [null,undefined,false], [left,left,true], [left,right,false],
    [array,array,true], [array,[],false], [fn,fn,true], [fn,()=>{},false],
    [symbol,symbol,true], [symbol,Symbol('x'),false], [left,-Infinity,false],
    [null,0,false], [undefined,NaN,false],
  ];
  for(let i=0;i<300;i++) assert.strictEqual(ssaStrictMatrix(left,right,8),16);
  for(const [a,b,equal] of equalityCases) {
    assert.strictEqual(ssaStrictMatrix(a,b,8),equal?8:16);
    assert.strictEqual(ssaStrictMatrix(b,a,8),equal?8:16);
  }
  assert.strictEqual(coercions, 8, 'strict equality must not invoke ToPrimitive');

  function ssaParameterLoops(count, start, step) {
    var sum=0;
    while(--count>=0) {
      var n=start, product=n;
      while((n=n-step)>1) product*=n;
      sum+=product;
    }
    return sum;
  }
  for(let i=0;i<300;i++) assert.strictEqual(ssaParameterLoops(3,5,1),360);
  assert.strictEqual(ssaParameterLoops(3,'5',1),360);
  let conversions=0;
  const numberLike={ valueOf() { conversions++; return 5; } };
  assert.strictEqual(ssaParameterLoops(0,numberLike,1),0);
  assert.strictEqual(conversions,0);
  assert.strictEqual(ssaParameterLoops(3,numberLike,1),360);
  assert.strictEqual(conversions,6, 'entry-guard deopt must not replay coercions');
  console.log('PASS SSA execution');
}
