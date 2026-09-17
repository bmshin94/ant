import { createHatchway } from 'hatchway';
import { listen } from 'hatchway/node';

const state = { visits: 0, routes: ['/', '/health'], cache: new Map([['greeting', 'hello']]) };
const app = createHatchway({
  name: 'Hatchway example',
  names: ['help', 'state', 'visit', 'fail'],
  evaluate(input) {
    switch (input.trim()) {
      case 'help': return 'Commands: help, state, visit, fail';
      case 'state': return state;
      case 'visit': app.console.info('Visit recorded'); return ++state.visits;
      case 'fail': throw new Error('An application error, rendered in DevTools');
      default: throw new Error(`Unknown command: ${input}`);
    }
  },
});

const server = await listen(app);
console.log(`Paste into Chrome:\n${server.devtoolsUrl}`);
app.console.log('Welcome to Hatchway. Enter help or state.');
const stop = async () => { app.dispose(); await server.close(); };
process.once('SIGINT', stop);
process.once('SIGTERM', stop);
