import assert from 'node:assert';
import { readFile, writeFile, unlink, mkdir, rmdir, stat, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

console.log('Testing node:fs/promises with async/await...\n');

async function assertMissing(filePath) {
  try {
    await stat(filePath);
  } catch (error) {
    assert.strictEqual(error.code, 'ENOENT');
    return;
  }
  assert.fail(`Expected ${filePath} to be removed`);
}

async function testFs() {
  const tmpRoot = await mkdtemp(join(tmpdir(), 'ant-fs-async-'));
  const testDir = join(tmpRoot, 'nested');
  const testFile = join(testDir, 'async_test.txt');
  const testData = 'Hello from async ant:fs!';

  try {
    console.log('=== Test: mkdir ===');
    await mkdir(testDir);
    const dirStats = await stat(testDir);
    assert.strictEqual(dirStats.isDirectory(), true);
    assert.strictEqual(dirStats.isFile(), false);

    console.log('\n=== Test: writeFile ===');
    await writeFile(testFile, testData);

    console.log('\n=== Test: readFile ===');
    const content = await readFile(testFile);
    assert.ok(Buffer.isBuffer(content));
    assert.strictEqual(content.toString('utf8'), testData);
    assert.strictEqual(await readFile(testFile, 'utf8'), testData);

    console.log('\n=== Test: stat ===');
    const stats = await stat(testFile);
    assert.strictEqual(stats.size, Buffer.byteLength(testData));
    assert.strictEqual(stats.isFile(), true);
    assert.strictEqual(stats.isDirectory(), false);

    console.log('\n=== Test: unlink and rmdir ===');
    await unlink(testFile);
    await assertMissing(testFile);
    await rmdir(testDir);
    await assertMissing(testDir);
  } finally {
    await rm(tmpRoot, { recursive: true, force: true });
  }

  console.log('\nAll async filesystem tests passed!');
}

testFs().catch((error) => {
  console.error(error);
  process.exit(1);
});
