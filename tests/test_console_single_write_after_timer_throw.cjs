// console.* must write each line once, even after an exception escaped a timer callback.
const { spawnSync } = require('child_process');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const source = `
setTimeout(() => { throw new Error('escaped'); }, 0);
setTimeout(() => {
  console.log('out-line');
  console.info('info-line');
  console.error('err-line');
  console.warn('warn-line');
}, 5);
setTimeout(() => console.log('later-line'), 10);
`;

const result = spawnSync(process.execPath, ['-e', source], { encoding: 'utf8' });
const count = (text, needle) => text.split('\n').filter((line) => line === needle).length;

for (const line of ['out-line', 'info-line', 'later-line'])
  assert(count(result.stdout, line) <= 1, `${line} written ${count(result.stdout, line)} times to stdout`);
for (const line of ['err-line', 'warn-line'])
  assert(count(result.stderr, line) <= 1, `${line} written ${count(result.stderr, line)} times to stderr`);

let calls = 0;
const sink = { write() { calls++; return true; } };
const custom = new console.Console({ stdout: sink, stderr: sink });
custom.log('custom-line');
assert(calls === 1, `custom stream write called ${calls} times`);

console.log('PASS');
