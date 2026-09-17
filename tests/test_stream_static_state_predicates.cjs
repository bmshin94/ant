const assert = require('node:assert');
const stream = require('node:stream');

async function endedReadableReportsFalse() {
  const readable = new stream.Readable({
    read() {
      this.push(Buffer.from('ok'));
      this.push(null);
    }
  });

  assert.strictEqual(stream.isReadable(readable), true);
  assert.strictEqual(readable.readable, true);
  assert.strictEqual(readable._readableState.destroyed, false);
  assert.strictEqual(stream.isErrored(readable), false);
  assert.strictEqual(stream.isDestroyed(readable), false);
  assert.strictEqual(stream.isDisturbed(readable), false);

  readable.resume();
  await new Promise((resolve) => readable.once('end', resolve));
  assert.strictEqual(readable.readableEnded, true);
  assert.strictEqual(readable.readable, false);
  assert.strictEqual(readable._readableState.endEmitted, true);
  assert.strictEqual(stream.isReadable(readable), false);
}

function destroyedReadableReportsFalse() {
  const readable = new stream.Readable({
    read() {
      this.push(Buffer.from('ok'));
      this.push(null);
    }
  });

  readable.destroy();
  assert.strictEqual(readable.destroyed, true);
  assert.strictEqual(readable._readableState.destroyed, true);
  assert.strictEqual(readable.readable, false);
  assert.strictEqual(readable._readableState.endEmitted, false);
  assert.strictEqual(stream.isDestroyed(readable), true);
  assert.strictEqual(stream.isReadable(readable), false);
}

endedReadableReportsFalse().then(() => {
  destroyedReadableReportsFalse();
  console.log('stream:static-state-predicates:ok');
});
