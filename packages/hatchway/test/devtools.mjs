// Optional real frontend check: npx playwright install chromium && npm run test:devtools
import assert from 'node:assert/strict';
import { chromium } from 'playwright';
import { createHatchway } from 'hatchway';
import { listen } from 'hatchway/node';

const commands = [];
const app = createHatchway({
  name: 'Hatchway frontend test', names: ['status', 'fail'],
  evaluate(input) {
    commands.push(input);
    if (input.trim() === 'fail') throw new Error('Hatchway example failure');
    return { message: 'hatchway-ui-ok', nested: { answer: 42 } };
  },
});
const server = await listen(app, { port: 0 });
let browser;
try {
  browser = await chromium.launch({ headless: true, channel: 'chromium' });
  const page = await browser.newPage();
  page.setDefaultTimeout(10000);
  await page.goto(server.devtoolsUrl);
  const prompt = page.getByRole('textbox', { name: 'Console prompt', exact: true });
  await prompt.waitFor();
  await prompt.fill('show state');
  // Give eager evaluation time to run; no command should have executed.
  await page.waitForTimeout(500);
  assert.equal(commands.length, 0, `Unexpected evaluations while typing: ${JSON.stringify(commands)}`);
  await prompt.press('Enter');
  await page.getByText('hatchway-ui-ok', { exact: false }).first().waitFor();
  assert.equal(commands.length, 1);
  assert.equal(commands[0].trim(), 'show state');
  const object = page.locator('.console-user-command-result').last();
  await object.locator('.console-object').first().click();
  const nested = object.getByRole('treeitem', { name: /^nested\s*:/ }).first();
  await nested.click();
  await nested.press('ArrowRight');
  await object.getByRole('treeitem', { name: /^answer\s*:\s*42$/ }).waitFor();
  app.console.log('hatchway-live-log');
  await page.getByText('hatchway-live-log', { exact: false }).first().waitFor();
  await prompt.fill('fail');
  await prompt.press('Enter');
  await page.getByText('Hatchway example failure', { exact: false }).first().waitFor();
  console.log('Real Chrome DevTools: input, preview isolation, object result, logs, and application errors passed.');
} finally {
  await browser?.close();
  app.dispose();
  await server.close();
}
