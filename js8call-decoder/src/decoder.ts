/**
 * decoder.ts — public API for JS8Call browser decoding.
 *
 * Usage:
 *   import { Js8CallDecoder } from './decoder.js';
 *
 *   const dec = new Js8CallDecoder({ wsUrl: 'ws://sdr.example.com:8073/audio' });
 *
 *   dec.on('decode', ({ freq_hz, snr_db, message, utc }) => {
 *     console.log(`${utc}  ${freq_hz.toFixed(0)} Hz  ${snr_db.toFixed(1)} dB  ${message}`);
 *   });
 *
 *   await dec.start();
 *   dec.setMode('fast');
 *   dec.stop();
 */

export type SpeedMode = 'slow' | 'normal' | 'fast' | 'turbo';

const MODE_ID: Record<SpeedMode, number> = {
  slow: 0, normal: 1, fast: 2, turbo: 3,
};

export interface DecodeOptions {
  /** WebSocket URL of the SDR audio source (binary Int16 PCM, mono). */
  wsUrl:    string;
  /** JS8Call speed mode (default: 'normal'). */
  mode?:    SpeedMode;
  /** Low edge of frequency search band in Hz (default: 200). */
  freqMin?: number;
  /** High edge of frequency search band in Hz (default: 3000). */
  freqMax?: number;
}

export interface DecodeResult {
  /** Carrier frequency of the decoded signal in Hz. */
  freq_hz: number;
  /** Estimated SNR in dB. */
  snr_db:  number;
  /** Decoded message text. */
  message: string;
  /** UTC timestamp of the decode (ISO 8601). */
  utc:     string;
}

type Events = {
  decode:       (result: DecodeResult) => void;
  connected:    () => void;
  disconnected: () => void;
  error:        (message: string) => void;
};

export class Js8CallDecoder {
  private opts:      DecodeOptions;
  private worker:    Worker | null = null;
  private handlers:  Map<string, Set<Function>> = new Map();

  constructor(opts: DecodeOptions) {
    this.opts = opts;
  }

  /**
   * Start the decoder.  Spawns a Worker, connects to the WebSocket, and
   * begins streaming audio into the WASM decoder.
   * Returns a Promise that resolves once the WASM module is initialised.
   */
  async start(): Promise<void> {
    this.worker = new Worker(
      new URL('./decoder-worker.ts', import.meta.url),
      { type: 'module' },
    );
    this.worker.addEventListener('message', (e) => this._dispatch(e.data));

    await this._waitForReady();

    this.worker.postMessage({
      type:    'start',
      url:     this.opts.wsUrl,
      mode:    MODE_ID[this.opts.mode ?? 'normal'],
      freqMin: this.opts.freqMin ?? 200,
      freqMax: this.opts.freqMax ?? 3000,
    });
  }

  /** Stop decoding and close the WebSocket. */
  stop(): void {
    this.worker?.postMessage({ type: 'stop' });
    this.worker?.terminate();
    this.worker = null;
  }

  /** Switch speed mode at runtime without reconnecting. */
  setMode(mode: SpeedMode): void {
    this.opts.mode = mode;
    this.worker?.postMessage({ type: 'set-mode', mode: MODE_ID[mode] });
  }

  on<K extends keyof Events>(event: K, cb: Events[K]): this {
    if (!this.handlers.has(event)) this.handlers.set(event, new Set());
    this.handlers.get(event)!.add(cb);
    return this;
  }

  off<K extends keyof Events>(event: K, cb: Events[K]): this {
    this.handlers.get(event)?.delete(cb);
    return this;
  }

  private _dispatch(msg: any): void {
    switch (msg.type) {
      case 'decode':       this._emit('decode',       msg as DecodeResult); break;
      case 'connected':    this._emit('connected');    break;
      case 'disconnected': this._emit('disconnected'); break;
      case 'error':        this._emit('error',         msg.message);        break;
    }
  }

  private _emit(event: string, ...args: any[]): void {
    this.handlers.get(event)?.forEach(cb => cb(...args));
  }

  private _waitForReady(): Promise<void> {
    return new Promise((resolve, reject) => {
      const handler = (e: MessageEvent) => {
        if (e.data?.type === 'ready') {
          this.worker?.removeEventListener('message', handler);
          resolve();
        } else if (e.data?.type === 'error') {
          this.worker?.removeEventListener('message', handler);
          reject(new Error(e.data.message));
        }
      };
      this.worker!.addEventListener('message', handler);
    });
  }
}
