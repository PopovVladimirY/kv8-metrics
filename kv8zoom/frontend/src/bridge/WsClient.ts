// kv8zoom/frontend/src/bridge/WsClient.ts
// Auto-reconnecting WebSocket wrapper.

import type { BridgeMsg, CameraMsg, SelectSessionMsg, SeekMsg, PauseMsg, PlayMsg } from './Protocol';

export type ConnectionStatus = 'connecting' | 'connected' | 'disconnected';

export type OnMessageFn  = (msg: BridgeMsg) => void;
export type OnStatusFn   = (status: ConnectionStatus) => void;

const RECONNECT_DELAY_MS = 2000;
const MAX_RECONNECT_ATTEMPTS = Number.MAX_SAFE_INTEGER; // reconnect indefinitely

export class WsClient {
  private url:     string;
  private socket:  WebSocket | null = null;
  private stopped  = false;
  private attempts = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  private onMessage: OnMessageFn;
  private onStatus:  OnStatusFn;

  constructor(url: string, onMessage: OnMessageFn, onStatus: OnStatusFn) {
    this.url       = url;
    this.onMessage = onMessage;
    this.onStatus  = onStatus;
    this.connect();
  }

  private connect(): void {
    if (this.stopped || this.attempts >= MAX_RECONNECT_ATTEMPTS) return;
    this.attempts++;
    this.onStatus('connecting');

    const ws = new WebSocket(this.url);
    this.socket = ws;

    ws.onopen = () => {
      this.attempts = 0;
      this.onStatus('connected');
    };

    ws.onmessage = (ev: MessageEvent) => {
      try {
        const msg = JSON.parse(ev.data as string) as BridgeMsg;
        this.onMessage(msg);
      } catch {
        // Ignore unparseable messages.
      }
    };

    ws.onclose = () => {
      this.socket = null;
      if (!this.stopped) {
        this.onStatus('disconnected');
        this.scheduleReconnect();
      }
    };

    ws.onerror = () => {
      // onclose fires after onerror; no separate handling needed.
    };
  }

  private scheduleReconnect(): void {
    if (this.stopped) return;
    this.reconnectTimer = setTimeout(() => this.connect(), RECONNECT_DELAY_MS);
  }

  send(msg: CameraMsg | SelectSessionMsg | SeekMsg | PauseMsg | PlayMsg): void {
    if (this.socket?.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(msg));
    }
  }

  close(): void {
    this.stopped = true;
    if (this.reconnectTimer !== null) clearTimeout(this.reconnectTimer);
    this.socket?.close();
    this.socket = null;
  }
}
