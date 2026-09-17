import { once } from 'node:events';
import WebSocket from 'ws';

export function harness(app, data) {
  const messages = [];
  const session = app.connect({ send: message => messages.push(JSON.parse(message)) }, data);
  let id = 0;
  return {
    session, messages,
    async command(method, params = {}) {
      const requestId = ++id;
      await session.receive(JSON.stringify({ id: requestId, method, params }));
      return messages.find(message => message.id === requestId);
    },
  };
}

export async function websocket(url, options) {
  const socket = new WebSocket(url, options);
  const events = [];
  const pending = new Map();
  let id = 0;
  socket.on('message', data => {
    const message = JSON.parse(data.toString());
    if ('id' in message) {
      const task = pending.get(message.id);
      if (task) { clearTimeout(task.timer); pending.delete(message.id); task.resolve(message); }
    } else events.push(message);
  });
  socket.on('close', () => {
    for (const task of pending.values()) { clearTimeout(task.timer); task.reject(new Error('Socket closed')); }
    pending.clear();
  });
  await once(socket, 'open');
  return {
    socket, events,
    command(method, params = {}) {
      return new Promise((resolve, reject) => {
        const requestId = ++id;
        const timer = setTimeout(() => { pending.delete(requestId); reject(new Error(`Timed out: ${method}`)); }, 5000);
        pending.set(requestId, { resolve, reject, timer });
        socket.send(JSON.stringify({ id: requestId, method, params }));
      });
    },
    async close() {
      if (socket.readyState === WebSocket.CLOSED) return;
      const closed = once(socket, 'close');
      socket.close();
      await closed;
    },
  };
}
