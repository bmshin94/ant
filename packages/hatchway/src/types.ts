export interface EvaluationContext<T = unknown> {
  sessionId: string;
  contextId: number;
  data: T;
  /** Aborted on timeout, cancellation, or disconnect; callbacks must cooperate. */
  signal: AbortSignal;
}

export interface ContextOptions<T = unknown> {
  name: string;
  evaluate(input: string, context: EvaluationContext<T>): unknown;
  /** Used only for DevTools' speculative, side-effect-free evaluations. */
  preview?(input: string, context: EvaluationContext<T>): unknown;
  /** Top-level names offered to DevTools' JavaScript completion UI. */
  names?: readonly string[];
}

export interface HatchwayOptions<T = unknown> extends ContextOptions<T> {
  contexts?: readonly ContextOptions<T>[];
  maxObjects?: number;
  maxProperties?: number;
  maxConsoleEntries?: number;
  maxMessageBytes?: number;
  maxPendingEvaluations?: number;
  evaluationTimeout?: number;
}

export interface Transport {
  send(message: string): void;
  close?(): void;
}

export interface Session {
  readonly id: string;
  readonly closed: boolean;
  receive(message: string): Promise<void>;
  close(): void;
}

export type ConsoleLevel = 'log' | 'debug' | 'info' | 'warning' | 'error' | 'dir' | 'table';

export interface RemoteObject {
  type: 'object' | 'function' | 'undefined' | 'string' | 'number' | 'boolean' | 'symbol' | 'bigint';
  subtype?: string;
  className?: string;
  description?: string;
  value?: unknown;
  unserializableValue?: string;
  objectId?: string;
  preview?: {
    type: string;
    subtype?: string;
    description?: string;
    overflow: boolean;
    properties: { name: string; type: string; subtype?: string; value?: string }[];
  };
}

export class ProtocolError extends Error {
  constructor(public readonly code: number, message: string) {
    super(message);
    this.name = 'ProtocolError';
  }
}
