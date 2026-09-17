const assert = require('node:assert');
const zlib = require('node:zlib');
const { Readable, Writable } = require('node:stream');
const { Z_SYNC_FLUSH } = zlib.constants;
const trailer = Buffer.from([0, 0, 255, 255]);

function checkState(stream, ended) {
  assert.strictEqual(stream._readableState.endEmitted, ended);
  assert.strictEqual(stream._readableState.ended, ended);
  assert.strictEqual(stream.readableEnded, ended);
  assert.strictEqual(stream.readable, !ended);
}

function flush(stream, kind) {
  return new Promise((resolve, reject) => {
    const done = (err) => err ? reject(err) : resolve();
    if (kind === undefined) stream.flush(done);
    else stream.flush(kind, done);
  });
}

async function testFactories() {
  for (const name of [
    'Gzip', 'Gunzip', 'Deflate', 'Inflate', 'DeflateRaw', 'InflateRaw',
    'Unzip', 'BrotliCompress', 'BrotliDecompress',
  ]) {
    const stream = zlib[`create${name}`]();
    const other = new zlib[name]();
    assert(stream instanceof Readable);
    checkState(stream, false);
    checkState(other, false);
    assert.notStrictEqual(stream._readableState, other._readableState);
    const state = stream._readableState;
    stream.reset();
    assert.strictEqual(stream._readableState, state);
    checkState(stream, false);
    stream.close();
    assert.strictEqual(state.endEmitted, false, 'close must not synthesize end');
    assert.strictEqual(state.destroyed, true);
    assert.strictEqual(stream.destroyed, true);
    assert.strictEqual(stream.readable, false);
    other.close();
  }
}

async function testMessages(noContextTakeover) {
  const deflate = zlib.createDeflateRaw({ chunkSize: 128 });
  const inflate = zlib.createInflateRaw({ chunkSize: 128 });
  const state = inflate._readableState;
  let compressed = [];
  let output = [];
  let ends = 0;
  deflate.on('data', (chunk) => compressed.push(chunk));
  inflate.on('data', (chunk) => output.push(chunk));
  inflate.on('end', () => ends++);
  deflate.on('error', (err) => { throw err; });
  inflate.on('error', (err) => { throw err; });

  try {
    for (const message of [
      Buffer.from('compressed websocket message '.repeat(20)),
      Buffer.from('compressed websocket message '.repeat(20)),
      Buffer.alloc(0),
      Buffer.from([0, 255, 128, 1, 0, 254]),
      Buffer.from('large fragmented message '.repeat(2000)),
    ]) {
      deflate.write(message);
      await flush(deflate, Z_SYNC_FLUSH);
      const bytes = Buffer.concat(compressed);
      assert.strictEqual(bytes.subarray(-4).toString('hex'), trailer.toString('hex'));
      const payload = bytes.subarray(0, -4);
      compressed = [];

      // ws flushes each fragment, and restores the trailer on the final one.
      const split = Math.floor(payload.length / 2);
      inflate.write(payload.subarray(0, split));
      await flush(inflate);
      checkState(inflate, false);
      inflate.write(payload.subarray(split));
      inflate.write(trailer);
      await flush(inflate);
      checkState(inflate, false);
      assert.strictEqual(Buffer.concat(output).toString('hex'), message.toString('hex'));
      output = [];
      assert.strictEqual(inflate._readableState, state);

      if (noContextTakeover) {
        deflate.reset();
        inflate.reset();
        checkState(inflate, false);
        assert.strictEqual(inflate._readableState, state);
      }
    }
    assert.strictEqual(ends, 0);
  } finally {
    deflate.close();
    inflate.close();
  }
}

async function testCompletion() {
  for (const completion of ['end', 'trailer', 'combined']) {
    const trailingInput = completion !== 'end';
    const stream = zlib.createInflateRaw({ autoDestroy: false, chunkSize: 64 });
    const state = stream._readableState;
    const output = [];
    let ends = 0;
    stream.on('data', (chunk) => output.push(chunk));
    stream.on('error', (err) => { throw err; });
    const ended = new Promise((resolve) => {
      stream.on('end', () => {
        ends++;
        checkState(stream, true);
        resolve();
      });
    });

    const compressed = zlib.deflateRawSync('finished websocket message');
    stream.write(completion === 'combined' ? Buffer.concat([compressed, trailer]) : compressed);
    if (completion === 'trailer') stream.write(trailer);
    await flush(stream);
    checkState(stream, trailingInput);
    if (!trailingInput) stream.end();
    await ended;
    assert.strictEqual(ends, 1);
    assert.strictEqual(Buffer.concat(output).toString(), 'finished websocket message');
    assert.strictEqual(stream._readableState, state);
    stream.reset();
    checkState(stream, true);
    stream.end();
    assert.strictEqual(ends, 1, 'reset/end must not emit end twice');
    stream.close();
    assert.strictEqual(state.endEmitted, true);
  }
}

async function testEndFactories() {
  const input = Buffer.from('stream completion');
  for (const [name, data] of [
    ['Gzip', input], ['Gunzip', zlib.gzipSync(input)],
    ['Deflate', input], ['Inflate', zlib.deflateSync(input)],
    ['DeflateRaw', input], ['InflateRaw', zlib.deflateRawSync(input)],
    ['Unzip', zlib.gzipSync(input)], ['BrotliCompress', input],
    ['BrotliDecompress', zlib.brotliCompressSync(input)],
  ]) {
    const stream = zlib[`create${name}`]();
    stream.on('data', () => checkState(stream, false));
    await new Promise((resolve, reject) => {
      stream.on('error', reject);
      stream.on('end', () => {
        checkState(stream, true);
        resolve();
      });
      stream.end(data);
    });
    stream.close();
  }
}

async function testBufferedCompletion() {
  const message = Buffer.from('buffered websocket output '.repeat(20));
  for (const paused of [false, true]) {
    const stream = zlib.createInflateRaw({ chunkSize: 64 });
    const output = [];
    let ends = 0;
    stream.on('error', (err) => { throw err; });
    const ended = new Promise((resolve) => {
      stream.on('end', () => {
        ends++;
        checkState(stream, true);
        resolve();
      });
    });
    if (paused) {
      stream.on('data', (chunk) => output.push(chunk));
      stream.pause();
    }
    const finished = new Promise((resolve) => stream.on('finish', resolve));
    stream.end(zlib.deflateRawSync(message));
    await finished;
    await new Promise(setImmediate);
    assert.strictEqual(stream._readableState.endEmitted, false);
    assert.strictEqual(stream._readableState.length, message.length);
    assert.strictEqual(ends, 0);
    assert.strictEqual(output.length, 0);

    if (!paused) {
      let chunk;
      while ((chunk = stream.read()) !== null) output.push(chunk);
    }
    stream.resume();
    await ended;
    assert.strictEqual(stream._readableState.length, 0);
    assert.strictEqual(Buffer.concat(output).toString(), message.toString());
    assert.strictEqual(ends, 1);
    stream.close();
  }

  const source = zlib.createInflateRaw();
  const output = [];
  const dest = new Writable({
    write(chunk, encoding, callback) { output.push(chunk); callback(); },
  });
  const finished = new Promise((resolve, reject) => {
    dest.on('finish', resolve);
    source.on('error', reject);
    dest.on('error', reject);
  });
  source.pipe(dest);
  source.end(zlib.deflateRawSync(message));
  await finished;
  assert.strictEqual(Buffer.concat(output).toString(), message.toString());
  source.close();
}

// Optional integration run: pass the installed ws/lib/permessage-deflate.js path.
async function testWs(modulePath) {
  const PerMessageDeflate = require(modulePath);
  const run = (pmd, method, data, fin = true) => new Promise((resolve, reject) => {
    pmd[method](data, fin, (err, result) => err ? reject(err) : resolve(result));
  });
  for (const noContextTakeover of [false, true]) {
    const encoder = new PerMessageDeflate({ isServer: true, zlibDeflateOptions: { chunkSize: 128 } });
    const decoder = new PerMessageDeflate({ zlibInflateOptions: { chunkSize: 128 } });
    encoder.params = decoder.params = { server_no_context_takeover: noContextTakeover };
    let inflater;
    for (const data of [
      Buffer.from('websocket '.repeat(1000)),
      Buffer.from('websocket '.repeat(1000)),
      Buffer.alloc(0),
      Buffer.from([0, 255, 1, 128]),
    ]) {
      const compressed = await run(encoder, 'compress', data);
      const out = await run(decoder, 'decompress', compressed);
      assert.strictEqual(out.toString('hex'), data.toString('hex'));
      assert.strictEqual(decoder._inflate._readableState.endEmitted, false);
      if (inflater) assert.strictEqual(decoder._inflate, inflater);
      inflater = decoder._inflate;
    }
    const first = await run(encoder, 'compress', Buffer.from('fragment one '), false);
    const firstOut = await run(decoder, 'decompress', first, false);
    const last = await run(encoder, 'compress', Buffer.from('fragment two'));
    const lastOut = await run(decoder, 'decompress', last);
    assert.strictEqual(Buffer.concat([firstOut, lastOut]).toString(), 'fragment one fragment two');
    encoder.cleanup();
    decoder.cleanup();

    const finished = new PerMessageDeflate();
    finished.params = {};
    for (const value of ['finished message one', 'finished message two']) {
      assert.strictEqual((await run(finished, 'decompress', zlib.deflateRawSync(value))).toString(), value);
      assert.strictEqual(finished._inflate, null, 'ws must close a completed inflater');
    }
    finished.cleanup();
  }
  console.log('ws permessage-deflate: ok');
}

const timeout = setTimeout(() => {
  console.error('timed out testing zlib readable state');
  process.exit(1);
}, 5000);

(async () => {
  await testFactories();
  await testMessages(false);
  await testMessages(true);
  await testCompletion();
  await testEndFactories();
  await testBufferedCompletion();
  if (process.argv[2]) await testWs(process.argv[2]);
  clearTimeout(timeout);
  console.log('zlib readable state: ok');
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
