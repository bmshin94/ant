#!/usr/bin/env node
import { parseArgs } from 'node:util';
import { connect } from './node.js';

try {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: { port: { type: 'string' }, help: { type: 'boolean', short: 'h' } },
  });
  if (values.help) {
    console.log('Usage: hatchway connect <http(s)://app/__hatchway> [--port 9229]\n\nSet HATCHWAY_TOKEN to send a Bearer token to the remote endpoint.');
  } else {
    if (positionals.length !== 2 || positionals[0] !== 'connect') throw new Error('Usage: hatchway connect <url> [--port 9229]');
    const port = values.port === undefined ? 9229 : Number(values.port);
    if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('port must be between 0 and 65535');
    const token = process.env.HATCHWAY_TOKEN;
    const server = await connect(positionals[1]!, { port, headers: token ? { Authorization: `Bearer ${token}` } : undefined });
    console.log(`Hatchway listening at ${server.url}\n\nPaste into Chrome:\n${server.devtoolsUrl}`);
    const stop = () => { void server.close().catch(error => { console.error(error.message); process.exitCode = 1; }); };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  }
} catch (error) {
  console.error(error instanceof Error ? error.message : 'Hatchway failed');
  process.exitCode = 1;
}
