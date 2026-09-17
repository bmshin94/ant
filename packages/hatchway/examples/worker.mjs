import { createHatchway } from 'hatchway';
import { createWorkerHandler } from 'hatchway/workers';

const app = createHatchway({
  name: 'My Worker',
  names: ['help', 'status'],
  evaluate(input, { data: env }) {
    if (input.trim() === 'help') return 'Commands: help, status';
    if (input.trim() === 'status') return { app: 'My Worker', environment: env.ENVIRONMENT ?? 'development' };
    throw new Error(`Unknown command: ${input}`);
  },
});

const inspect = createWorkerHandler(app, {
  authorize: (request, env) => !!env.HATCHWAY_TOKEN && request.headers.get('Authorization') === `Bearer ${env.HATCHWAY_TOKEN}`,
});

export default {
  async fetch(request, env) {
    return await inspect(request, env) ?? new Response('Hello from the app');
  },
};
