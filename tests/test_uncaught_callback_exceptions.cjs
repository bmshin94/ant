// Exceptions escaping event-loop callbacks must be reported, not dropped.
// Without an 'uncaughtException' listener: print the error, exit 1, run nothing
// further. With a listener: deliver (error, origin), keep the loop running, and
// leave no pending exception behind for later callbacks.
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-uncaught-'));
let index = 0;

function run(source, { useEval = false } = {}) {
  const args = useEval ? ['-e', source] : [path.join(dir, `case${index++}.cjs`)];
  if (!useEval) fs.writeFileSync(args[0], source);
  return spawnSync(process.execPath, args, { encoding: 'utf8' });
}

const throwers = {
  setTimeout: `setTimeout(() => { throw new Error('boom-setTimeout'); }, 0);`,
  setInterval: `const id = setInterval(() => { clearInterval(id); throw new Error('boom-setInterval'); }, 0);`,
  setImmediate: `setImmediate(() => { throw new Error('boom-setImmediate'); });`,
  nextTick: `process.nextTick(() => { throw new Error('boom-nextTick'); });`,
  queueMicrotask: `queueMicrotask(() => { throw new Error('boom-queueMicrotask'); });`,
  fsReadFile: `require('fs').readFile(__filename, () => { throw new Error('boom-fsReadFile'); });`,
  fsStat: `require('fs').stat(__filename, () => { throw new Error('boom-fsStat'); });`,
};

const later = `
setTimeout(() => {
  let state;
  try { JSON.stringify([1, 2]); state = 'clean'; } catch (e) { state = 'stale:' + e.message; }
  console.log('LATER ' + state);
}, 30);
`;

for (const [kind, thrower] of Object.entries(throwers)) {
  const plain = run(thrower + later);
  assert(plain.status === 1, `${kind} without listener: exit ${plain.status}, stderr=${plain.stderr}`);
  assert(plain.stderr.includes(`boom-${kind}`), `${kind} without listener: error not printed, stderr=${plain.stderr}`);
  assert(!plain.stdout.includes('LATER'), `${kind} without listener: loop kept running, stdout=${plain.stdout}`);

  const handled = run(
    `process.on('uncaughtException', (err, origin) => console.log('SAW ' + err.message + ' ' + origin));\n` +
    thrower + later,
  );
  assert(handled.status === 0, `${kind} with listener: exit ${handled.status}, stderr=${handled.stderr}`);
  assert(handled.stdout.includes(`SAW boom-${kind} uncaughtException`), `${kind} with listener: not delivered, stdout=${handled.stdout}`);
  assert(handled.stdout.split('\n').filter((l) => l.startsWith('SAW ')).length === 1, `${kind} with listener: delivered more than once, stdout=${handled.stdout}`);
  assert(handled.stdout.includes('LATER clean'), `${kind} with listener: later callback missing or saw stale exception, stdout=${handled.stdout}`);
}

const topLevel = run(
  `process.on('uncaughtException', (err) => console.log('SAW ' + err.message));\n` +
  `setTimeout(() => console.log('LATER ran'), 5);\n` +
  `throw new Error('boom-top');\n`,
);
assert(topLevel.status === 0, `top-level with listener: exit ${topLevel.status}, stderr=${topLevel.stderr}`);
assert(topLevel.stdout.includes('SAW boom-top') && topLevel.stdout.includes('LATER ran'), `top-level with listener: stdout=${topLevel.stdout}`);

const topLevelPlain = run(`setTimeout(() => console.log('LATER ran'), 0);\nthrow new Error('boom-top-plain');\n`);
assert(topLevelPlain.status === 1, `top-level without listener: exit ${topLevelPlain.status}`);
assert(topLevelPlain.stderr.includes('boom-top-plain'), `top-level without listener: stderr=${topLevelPlain.stderr}`);
assert(!topLevelPlain.stdout.includes('LATER'), `top-level without listener: loop ran, stdout=${topLevelPlain.stdout}`);

const evalTimer = run(`setTimeout(() => { throw new Error('boom-eval'); }, 0);`, { useEval: true });
assert(evalTimer.status === 1 && evalTimer.stderr.includes('boom-eval'), `-e timer throw: exit ${evalTimer.status}, stderr=${evalTimer.stderr}`);

const failingListener = run(
  `process.on('uncaughtException', () => { throw new Error('listener-broke'); });\n` +
  `setTimeout(() => { throw new Error('boom-first'); }, 0);\n` + later,
);
assert(failingListener.status === 7, `throwing listener: exit ${failingListener.status}, stderr=${failingListener.stderr}`);
assert(!failingListener.stdout.includes('LATER'), `throwing listener: loop kept running`);

fs.rmSync(dir, { recursive: true, force: true });
console.log('PASS');
