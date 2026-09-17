import { createHatchway } from '../../dist/index.js';
import { createWorkerHandler } from '../../dist/workers.js';

const state = { commands: 0, nested: { platform: 'workers' } };
const app = createHatchway({
  name: 'Test Worker', names: ['status', 'fail'],
  evaluate(input, { data: env }) {
    if (input === 'status') return state;
    if (input === 'fail') throw new Error('Worker command failed');
    app.console.log('command executed', input);
    state.commands++;
    return { input, environment: env.ENVIRONMENT };
  },
});
const inspect = createWorkerHandler(app, {
  authorize: (request, env) => request.headers.get('Authorization') === `Bearer ${env.HATCHWAY_TOKEN}`,
});
export default { async fetch(request, env) {
  return await inspect(request, env) ?? new Response('application response');
} };
