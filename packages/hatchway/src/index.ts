import { Objects } from './objects.js';
import {
  ProtocolError, type ConsoleLevel, type ContextOptions,
  type HatchwayOptions, type Session, type Transport,
} from './types.js';

export type { ConsoleLevel, ContextOptions, EvaluationContext, HatchwayOptions, RemoteObject, Session, Transport } from './types.js';

interface ConsoleEntry {
  type: ConsoleLevel;
  args: unknown[];
  contextId: number;
  timestamp: number;
}

interface Limits {
  objects: number;
  properties: number;
  console: number;
  bytes: number;
  pending: number;
  timeout: number;
}

function positive(value: number | undefined, fallback: number): number {
  if (value === undefined) return fallback;
  if (!Number.isSafeInteger(value) || value <= 0) throw new TypeError('Limits must be positive integers');
  return value;
}

function string(params: Record<string, unknown>, name: string): string {
  if (typeof params[name] !== 'string') throw new ProtocolError(-32602, `${name} must be a string`);
  return params[name];
}

function object(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

let instanceCount = 0;

export class Hatchway<T = unknown> {
  readonly name: string;
  readonly id = `hatchway-${++instanceCount}`;
  private readonly contexts: readonly ContextOptions<T>[];
  private readonly sessions = new Set<Connection<T>>();
  private readonly history: ConsoleEntry[] = [];
  private readonly limits: Limits;
  private nextSession = 0;
  private disposed = false;

  readonly console = {
    log: (...args: unknown[]) => this.emitConsole('log', args),
    debug: (...args: unknown[]) => this.emitConsole('debug', args),
    info: (...args: unknown[]) => this.emitConsole('info', args),
    warn: (...args: unknown[]) => this.emitConsole('warning', args),
    error: (...args: unknown[]) => this.emitConsole('error', args),
    dir: (...args: unknown[]) => this.emitConsole('dir', args),
    table: (...args: unknown[]) => this.emitConsole('table', args),
    clear: () => this.clearConsole(),
  };

  constructor(options: HatchwayOptions<T>) {
    this.contexts = [options, ...(options.contexts ?? [])].map(context => {
      if (!context.name || typeof context.evaluate !== 'function') throw new TypeError('Each context needs a name and evaluate callback');
      return { ...context, names: [...(context.names ?? [])] };
    });
    this.name = options.name;
    this.limits = {
      objects: positive(options.maxObjects, 10000),
      properties: positive(options.maxProperties, 1000),
      console: positive(options.maxConsoleEntries, 100),
      bytes: positive(options.maxMessageBytes, 1024 * 1024),
      pending: positive(options.maxPendingEvaluations, 16),
      timeout: positive(options.evaluationTimeout, 30000),
    };
  }

  /** Wire receive() and close() to the transport's incoming messages and disconnect. */
  connect(transport: Transport, data?: T): Session {
    if (this.disposed) throw new Error('Hatchway is disposed');
    const session = new Connection(
      `${this.id}:${++this.nextSession}`, this.contexts, this.limits, transport, data as T,
      () => [...this.history], () => this.clearConsole(), () => this.sessions.delete(session),
    );
    this.sessions.add(session);
    return session;
  }

  emitConsole(type: ConsoleLevel, args: unknown[], contextId = 1): void {
    if (this.disposed) return;
    if (!this.contexts[contextId - 1]) throw new RangeError('Unknown execution context');
    const entry = { type, args: [...args], contextId, timestamp: Date.now() };
    this.history.push(entry);
    if (this.history.length > this.limits.console) this.history.shift();
    for (const session of this.sessions) session.console(entry);
  }

  clearConsole(): void {
    this.history.length = 0;
    for (const session of this.sessions) session.clearConsole();
  }

  dispose(): void {
    this.disposed = true;
    for (const session of this.sessions) session.close();
    this.history.length = 0;
  }
}

class Connection<T> implements Session {
  closed = false;
  private enabled = false;
  private consoleEnabled = false;
  private replayed = false;
  private readonly objects: Objects;
  private readonly requests = new Set<number>();
  private readonly pending = new Set<AbortController>();
  private nextException = 0;

  constructor(
    readonly id: string,
    private readonly contexts: readonly ContextOptions<T>[],
    private readonly limits: Limits,
    private readonly transport: Transport,
    private readonly data: T,
    private readonly history: () => ConsoleEntry[],
    private readonly discard: () => void,
    private readonly detached: () => void,
  ) {
    this.objects = new Objects(id, limits.objects, limits.properties);
  }

  private send(message: Record<string, unknown>): void {
    if (this.closed) return;
    try { this.transport.send(JSON.stringify(message)); }
    catch { this.close(); }
  }

  private event(method: string, params: Record<string, unknown>): void { this.send({ method, params }); }

  async receive(message: string): Promise<void> {
    if (this.closed) return;
    let request: unknown;
    if (typeof message !== 'string' || new TextEncoder().encode(message).byteLength > this.limits.bytes) {
      this.send({ id: null, error: { code: -32600, message: 'Message exceeds size limit or is not text' } });
      this.close();
      return;
    }
    try { request = JSON.parse(message); }
    catch { this.send({ id: null, error: { code: -32700, message: 'Invalid JSON' } }); return; }
    if (!object(request) || !Number.isSafeInteger(request.id) || typeof request.method !== 'string' ||
        (request.params !== undefined && !object(request.params))) {
      this.send({ id: object(request) && Number.isSafeInteger(request.id) ? request.id : null,
        error: { code: -32600, message: 'Expected a CDP command with an integer id, method, and optional params object' } });
      return;
    }
    const id = request.id as number;
    if (this.requests.has(id)) {
      this.send({ id, error: { code: -32600, message: 'Duplicate pending command id' } });
      return;
    }
    this.requests.add(id);
    try {
      const result = await this.route(request.method, (request.params ?? {}) as Record<string, unknown>);
      this.send({ id, result });
    } catch (error) {
      this.send({ id, error: { code: error instanceof ProtocolError ? error.code : -32000,
        message: error instanceof Error ? error.message : 'Command failed' } });
    } finally { this.requests.delete(id); }
  }

  private context(params: Record<string, unknown>): { context: ContextOptions<T>; contextId: number } {
    if (params.contextId !== undefined && params.uniqueContextId !== undefined) throw new ProtocolError(-32602, 'Context selectors are mutually exclusive');
    const contextId = params.uniqueContextId !== undefined
      ? this.contexts.findIndex((_, i) => params.uniqueContextId === `${this.id}:context:${i + 1}`) + 1
      : params.contextId ?? params.executionContextId ?? 1;
    if (typeof contextId !== 'number' || !Number.isSafeInteger(contextId) || !this.contexts[contextId - 1]) {
      throw new ProtocolError(-32602, 'Unknown execution context');
    }
    return { context: this.contexts[contextId - 1]!, contextId };
  }

  private async route(method: string, params: Record<string, unknown>): Promise<Record<string, unknown>> {
    switch (method) {
      case 'Runtime.enable':
        if (!this.enabled) {
          this.enabled = true;
          this.contexts.forEach((context, i) => this.event('Runtime.executionContextCreated', {
            context: { id: i + 1, uniqueId: `${this.id}:context:${i + 1}`, name: context.name,
              origin: '', auxData: { isDefault: i === 0 } },
          }));
        }
        this.replay();
        return {};
      case 'Runtime.disable': this.enabled = false; return {};
      case 'Console.enable': this.consoleEnabled = true; this.replay(); return {};
      case 'Console.disable': this.consoleEnabled = false; return {};
      case 'Runtime.evaluate': return this.evaluate(params);
      case 'Runtime.awaitPromise': {
        const handle = this.objects.get(string(params, 'promiseObjectId'));
        if (!(handle.value instanceof Promise)) throw new ProtocolError(-32602, 'Handle does not reference a Promise');
        return this.evaluate({ ...params, expression: '', objectGroup: handle.group }, () => handle.value);
      }
      case 'Runtime.getProperties': return this.objects.properties(string(params, 'objectId'), params);
      case 'Runtime.releaseObject': this.objects.release(string(params, 'objectId')); return {};
      case 'Runtime.releaseObjectGroup': this.objects.releaseGroup(string(params, 'objectGroup')); return {};
      case 'Runtime.discardConsoleEntries': case 'Console.clearMessages': this.discard(); return {};
      case 'Runtime.globalLexicalScopeNames': return { names: this.context(params).context.names ?? [] };
      case 'Runtime.getIsolateId': return { id: this.id };
      case 'Runtime.terminateExecution':
        for (const controller of this.pending) controller.abort(new Error('Evaluation cancelled'));
        return {};
      case 'Runtime.compileScript':
        // DevTools uses this as an Enter-key completeness probe. Application
        // command grammars need no JavaScript parsing and must not execute here.
        string(params, 'expression');
        if (params.persistScript) throw new ProtocolError(-32000, 'Application commands cannot be compiled as JavaScript');
        return {};
      case 'Debugger.enable': return { debuggerId: this.id };
      case 'Schema.getDomains': return { domains: ['Runtime', 'Console', 'Debugger'].map(name => ({ name, version: '1.3' })) };
      case 'Runtime.runIfWaitingForDebugger':
      case 'Runtime.setCustomObjectFormatterEnabled':
      case 'Runtime.setMaxCallStackSizeToCapture':
      case 'Runtime.setAsyncCallStackDepth':
      case 'Debugger.disable':
      case 'Debugger.setPauseOnExceptions':
      case 'Debugger.setAsyncCallStackDepth':
      case 'Debugger.setBlackboxPatterns':
      case 'Debugger.setBlackboxedRanges':
      case 'Debugger.setSkipAllPauses':
      case 'Debugger.setBreakpointsActive':
      case 'Log.enable':
      case 'Log.disable':
      case 'Inspector.enable': return {};
      default: throw new ProtocolError(-32601, `${method} is not supported by Hatchway`);
    }
  }

  private async evaluate(params: Record<string, unknown>, operation?: () => unknown): Promise<Record<string, unknown>> {
    const input = string(params, 'expression');
    const { context, contextId } = this.context(params);
    if (params.serializationOptions !== undefined) throw new ProtocolError(-32602, 'serializationOptions is not supported');
    const callback = operation ?? (params.throwOnSideEffect ? context.preview : context.evaluate);
    if (!callback) return {
      result: { type: 'undefined' },
      exceptionDetails: {
        exceptionId: ++this.nextException, text: 'Uncaught', lineNumber: 0, columnNumber: 0,
        executionContextId: contextId, exception: { type: 'object', subtype: 'error', className: 'EvalError',
          description: 'EvalError: Possible side-effect in debug-evaluate' },
      },
    };
    if (this.pending.size >= this.limits.pending) throw new ProtocolError(-32000, 'Too many pending evaluations');
    const group = typeof params.objectGroup === 'string' ? params.objectGroup : '';
    let timeout = this.limits.timeout;
    if (params.timeout !== undefined) {
      if (typeof params.timeout !== 'number' || !Number.isFinite(params.timeout) || params.timeout <= 0) {
        throw new ProtocolError(-32602, 'timeout must be positive');
      }
      timeout = Math.min(timeout, params.timeout);
    }
    const controller = new AbortController();
    this.pending.add(controller);
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      const cancelled = new Promise<never>((_, reject) => {
        controller.signal.addEventListener('abort', () => reject(controller.signal.reason), { once: true });
        timer = setTimeout(() => controller.abort(new Error('Evaluation timed out')), timeout);
      });
      const result = await Promise.race([
        Promise.resolve().then(() => {
          controller.signal.throwIfAborted();
          return callback(input, { sessionId: this.id, contextId, data: this.data, signal: controller.signal });
        }),
        cancelled,
      ]);
      if (this.closed) return {};
      return { result: this.objects.remote(result, group, params.generatePreview === true, params.returnByValue === true) };
    } catch (error) {
      if (this.closed) return {};
      if (error instanceof ProtocolError) throw error;
      return this.exception(error, contextId, group);
    } finally {
      clearTimeout(timer);
      this.pending.delete(controller);
    }
  }

  private exception(error: unknown, contextId: number, group: string): Record<string, unknown> {
    let remote;
    try { remote = this.objects.remote(error, group); }
    catch { remote = { type: 'string' as const, value: 'Application command failed' }; }
    return { result: remote, exceptionDetails: {
      exceptionId: ++this.nextException, text: 'Uncaught', lineNumber: 0, columnNumber: 0,
      executionContextId: contextId, exception: remote,
    } };
  }

  console(entry: ConsoleEntry): void {
    if ((!this.enabled && !this.consoleEnabled) || this.closed) return;
    let args;
    try { args = entry.args.map(value => this.objects.remote(value, 'console', true)); }
    catch { args = [{ type: 'string', value: '[Hatchway object limit reached; clear the console]' }]; }
    this.event('Runtime.consoleAPICalled', {
      type: entry.type, args, executionContextId: entry.contextId, timestamp: entry.timestamp,
    });
  }

  private replay(): void {
    if (this.replayed) return;
    this.replayed = true;
    for (const entry of this.history()) this.console(entry);
  }

  clearConsole(): void {
    this.objects.releaseGroup('console');
    if (this.enabled || this.consoleEnabled) this.event('Runtime.consoleAPICalled', {
      type: 'clear', args: [], executionContextId: 1, timestamp: Date.now(),
    });
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    for (const controller of this.pending) controller.abort(new Error('Inspector disconnected'));
    this.objects.clear();
    this.requests.clear();
    this.detached();
    try { this.transport.close?.(); } catch { /* Transport may already be closed. */ }
  }
}

export function createHatchway<T = unknown>(options: HatchwayOptions<T>): Hatchway<T> {
  return new Hatchway(options);
}
