export function target(name: string, id: string, websocketUrl: string) {
  const url = new URL(websocketUrl);
  const parameter = url.protocol === 'wss:' ? 'wss' : 'ws';
  const endpoint = `${url.host}${url.pathname}${url.search}`;
  return {
    id, type: 'node', title: name, description: 'Hatchway application console', url: 'hatchway://app',
    devtoolsFrontendUrl: `devtools://devtools/bundled/js_app.html?v8only=true&${parameter}=${encodeURIComponent(endpoint)}`,
    webSocketDebuggerUrl: websocketUrl,
  };
}

export const version = { Browser: 'Hatchway/0.1.0', 'Protocol-Version': '1.3', 'V8-Version': 'hatchway' };

export function originAllowed(origin: string | null | undefined, allowed: readonly string[] = []): boolean {
  return !origin || origin === 'devtools://devtools' || origin === 'chrome-devtools://devtools' || allowed.includes(origin);
}
