import { ProtocolError, type RemoteObject } from './types.js';

interface Handle {
  value: object | symbol;
  group: string;
}

function dataProperty(value: object, key: PropertyKey): unknown {
  const visited = new Set<object>();
  for (let current: object | null = value; current && !visited.has(current); current = Object.getPrototypeOf(current)) {
    visited.add(current);
    const descriptor = Object.getOwnPropertyDescriptor(current, key);
    if (descriptor) return 'value' in descriptor ? descriptor.value : undefined;
    if (visited.size >= 32) break;
  }
}

/** Never call an application getter, toString, toJSON, or custom inspector. */
function describe(value: unknown): RemoteObject {
  if (value === null) return { type: 'object', subtype: 'null', value: null, description: 'null' };
  switch (typeof value) {
    case 'undefined': return { type: 'undefined' };
    case 'string': return { type: 'string', value };
    case 'boolean': return { type: 'boolean', value };
    case 'number': {
      const description = Object.is(value, -0) ? '-0' : String(value);
      return Number.isFinite(value) && !Object.is(value, -0)
        ? { type: 'number', value, description }
        : { type: 'number', unserializableValue: description, description };
    }
    case 'bigint': return { type: 'bigint', unserializableValue: `${value}n`, description: `${value}n` };
    case 'symbol': return { type: 'symbol', description: String(value) };
    case 'function': {
      const name = dataProperty(value, 'name');
      return { type: 'function', className: 'Function', description: `function ${typeof name === 'string' ? name : ''}()` };
    }
    case 'object': {
      if (Array.isArray(value)) return {
        type: 'object', subtype: 'array', className: 'Array', description: `Array(${dataProperty(value, 'length')})`,
      };
      if (value instanceof Error) {
        const name = dataProperty(value, 'name');
        const message = dataProperty(value, 'message');
        return { type: 'object', subtype: 'error', className: 'Error', description:
          `${typeof name === 'string' ? name : 'Error'}${typeof message === 'string' ? `: ${message}` : ''}` };
      }
      // Calling intrinsics avoids overridable instance accessors and methods.
      if (value instanceof Date) return {
        type: 'object', subtype: 'date', className: 'Date', description: Date.prototype.toString.call(value),
      };
      if (value instanceof Map) return { type: 'object', subtype: 'map', className: 'Map', description: 'Map' };
      if (value instanceof Set) return { type: 'object', subtype: 'set', className: 'Set', description: 'Set' };
      if (value instanceof Promise) return { type: 'object', subtype: 'promise', className: 'Promise', description: 'Promise' };
      return { type: 'object', className: 'Object', description: 'Object' };
    }
  }
  return { type: 'undefined' };
}

function byValue(value: unknown, seen = new Set<object>(), budget = { remaining: 10000 }, depth = 0): unknown {
  if (--budget.remaining < 0 || depth > 32) throw new ProtocolError(-32000, 'Value exceeds serialization limits');
  if (value === null || typeof value !== 'object') {
    if (typeof value === 'bigint') throw new ProtocolError(-32000, 'Nested BigInt cannot be returned by value');
    if (typeof value === 'function' || typeof value === 'symbol' || typeof value === 'undefined') return undefined;
    return value;
  }
  if (seen.has(value)) throw new ProtocolError(-32000, 'Circular value cannot be returned by value');
  seen.add(value);
  const result: Record<string, unknown> | unknown[] = Array.isArray(value) ? [] : Object.create(null);
  for (const key of Object.keys(value)) {
    const descriptor = Object.getOwnPropertyDescriptor(value, key);
    if (!descriptor || !('value' in descriptor)) throw new ProtocolError(-32000, 'Accessors cannot be returned by value');
    Object.defineProperty(result, key, {
      value: byValue(descriptor.value, seen, budget, depth + 1), enumerable: true, configurable: true, writable: true,
    });
  }
  if (Array.isArray(result) && Array.isArray(value)) {
    if (value.length > 10000) throw new ProtocolError(-32000, 'Array exceeds serialization limits');
    result.length = value.length;
  }
  seen.delete(value);
  return result;
}

export class Objects {
  private readonly handles = new Map<string, Handle>();
  private readonly ids = new Map<object | symbol, Map<string, string>>();
  private nextId = 0;

  constructor(private readonly prefix: string, private readonly limit: number, private readonly propertyLimit: number) {}

  remote(value: unknown, group = '', preview = false, returnByValue = false): RemoteObject {
    let result: RemoteObject;
    try { result = describe(value); }
    catch { result = { type: typeof value === 'function' ? 'function' : 'object', description: 'Uninspectable object' }; }
    if ((typeof value !== 'object' || value === null) && typeof value !== 'function' && typeof value !== 'symbol') return result;
    if (returnByValue) {
      result.value = byValue(value);
      return result;
    }
    const reference = value as object | symbol;
    let id = this.ids.get(reference)?.get(group);
    if (!id) {
      if (this.handles.size >= this.limit) throw new ProtocolError(-32000, 'Object limit reached; release an object group');
      id = `${this.prefix}:${++this.nextId}`;
      this.handles.set(id, { value: reference, group });
      let groups = this.ids.get(reference);
      if (!groups) { groups = new Map(); this.ids.set(reference, groups); }
      groups.set(group, id);
    }
    result.objectId = id;
    if (preview && typeof value === 'object' && value !== null) result.preview = this.preview(value, result);
    return result;
  }

  private preview(value: object, description: RemoteObject): NonNullable<RemoteObject['preview']> {
    const preview: NonNullable<RemoteObject['preview']> = {
      type: description.type, subtype: description.subtype, description: description.description, overflow: false, properties: [],
    };
    try {
      const keys = Object.keys(value);
      preview.overflow = keys.length > 5;
      for (const key of keys.slice(0, 5)) {
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        if (!descriptor) continue;
        if (!('value' in descriptor)) {
          preview.properties.push({ name: key, type: 'accessor' });
        } else {
          const child = describe(descriptor.value);
          preview.properties.push({ name: key, type: child.type, subtype: child.subtype,
            value: String(child.description ?? child.value ?? child.type).slice(0, 200) });
        }
      }
    } catch { preview.overflow = true; }
    return preview;
  }

  get(id: string): Handle {
    const handle = this.handles.get(id);
    if (!handle) throw new ProtocolError(-32000, 'Object handle is unknown or released');
    return handle;
  }

  properties(id: string, params: Record<string, unknown>): Record<string, unknown> {
    const { value, group } = this.get(id);
    if (typeof value === 'symbol') return { result: [], internalProperties: [] };
    const result: Record<string, unknown>[] = [];
    const keys = new Set<PropertyKey>();
    const visited = new Set<object>();
    for (let current: object | null = value; current && !visited.has(current); current = Object.getPrototypeOf(current)) {
      visited.add(current);
      if (visited.size > 32) throw new ProtocolError(-32000, 'Prototype chain exceeds inspection limits');
      for (const key of Reflect.ownKeys(current)) {
        if (keys.has(key)) continue;
        keys.add(key);
        if (params.nonIndexedPropertiesOnly && typeof key === 'string' && /^(0|[1-9]\d*)$/.test(key)) continue;
        const descriptor = Object.getOwnPropertyDescriptor(current, key);
        if (!descriptor || (params.accessorPropertiesOnly && 'value' in descriptor)) continue;
        if (result.length >= this.propertyLimit) throw new ProtocolError(-32000, 'Object has too many properties');
        const property: Record<string, unknown> = {
          name: String(key), configurable: !!descriptor.configurable, enumerable: !!descriptor.enumerable, isOwn: current === value,
        };
        if (typeof key === 'symbol') property.symbol = this.remote(key, group);
        if ('value' in descriptor) {
          property.value = this.remote(descriptor.value, group, params.generatePreview === true);
          property.writable = !!descriptor.writable;
        } else {
          property.get = this.remote(descriptor.get, group);
          property.set = this.remote(descriptor.set, group);
        }
        result.push(property);
      }
      if (params.ownProperties) break;
    }
    const internalProperties: Record<string, unknown>[] = [];
    if (!params.accessorPropertiesOnly) {
      internalProperties.push({ name: '[[Prototype]]', value: this.remote(Object.getPrototypeOf(value), group) });
      if (value instanceof Map || value instanceof Set) {
        const entries = [];
        const iterator = value instanceof Map ? Map.prototype.entries.call(value) : Set.prototype.values.call(value);
        for (const item of iterator) {
          if (entries.length >= this.propertyLimit) break;
          entries.push(value instanceof Map ? { key: item[0], value: item[1] } : { value: item });
        }
        internalProperties.push({ name: '[[Entries]]', value: this.remote(entries, group, true) });
      }
    }
    return { result, internalProperties };
  }

  release(id: string): void {
    const handle = this.handles.get(id);
    if (!handle) return;
    this.handles.delete(id);
    const groups = this.ids.get(handle.value)!;
    groups.delete(handle.group);
    if (!groups.size) this.ids.delete(handle.value);
  }
  releaseGroup(group: string): void {
    for (const [id, handle] of this.handles) if (handle.group === group) this.release(id);
  }
  clear(): void { this.handles.clear(); this.ids.clear(); }
}
