// kv8zoom/frontend/src/hooks/useZoomBridge.ts
// React hook: subscribes to the kv8zoom WebSocket bridge and exposes live state.

import { useCallback, useEffect, useReducer, useRef } from 'react';
import { WsClient, type ConnectionStatus } from '../bridge/WsClient';
import type {
  BridgeMsg,
  NavMsg, AttMsg, MotMsg,
  TrailPoint, SessionInfo, TimelineMsg,
  PauseMsg, PlayMsg,
} from '../bridge/Protocol';

// ── State ──────────────────────────────────────────────────────────────────

export interface BridgeNav {
  lat:  number; lon:  number; alt:  number;
  vx:   number; vy:   number; vz:   number;
  fix:  number; sats: number; hdop: number; baro: number;
  ts:   number;
}

export interface BridgeAtt {
  qw: number; qx: number; qy: number; qz: number;
  rx: number; ry: number; rz: number;
  ax: number; ay: number; az: number;
  ts: number;
}

export interface BridgeMot {
  batt: number;
  m:    [number, number, number, number];
  ts:   number;
}

export interface BridgeTimeline {
  ts_start:   number; // microseconds
  ts_end:     number;
  ts_current: number;
  is_live:    boolean;
  is_paused:  boolean;
}

export interface BridgeState {
  nav:           BridgeNav | null;
  att:           BridgeAtt | null;
  mot:           BridgeMot | null;
  trail:         TrailPoint[];
  sessions:      SessionInfo[];
  activeSession: string;
  timeline:      BridgeTimeline | null;
}

const initialState: BridgeState = {
  nav: null, att: null, mot: null, trail: [],
  sessions: [], activeSession: '', timeline: null,
};

// ── Reducer ────────────────────────────────────────────────────────────────

type Action =
  | { type: 'nav';              payload: NavMsg   }
  | { type: 'att';              payload: AttMsg   }
  | { type: 'mot';              payload: MotMsg   }
  | { type: 'trail';            payload: TrailPoint[] }
  | { type: 'sessions';         payload: { list: SessionInfo[]; current: string } }
  | { type: 'session_selected'; payload: { id: string } }
  | { type: 'timeline';         payload: TimelineMsg }
  | { type: 'reset' };

function reducer(state: BridgeState, action: Action): BridgeState {
  switch (action.type) {
    case 'nav': {
      const nav = {
        lat: action.payload.lat, lon: action.payload.lon, alt: action.payload.alt,
        vx:  action.payload.vx,  vy:  action.payload.vy,  vz:  action.payload.vz,
        fix: action.payload.fix, sats: action.payload.sats,
        hdop: action.payload.hdop, baro: action.payload.baro,
        ts:  action.payload.ts,
      };
      // Append nav point to trail for instant live-trail updates.
      // The server sends a full simplified trail every ~5 s which replaces this.
      const newPt: TrailPoint = {
        ts: action.payload.ts,
        lat: action.payload.lat,
        lon: action.payload.lon,
        alt: action.payload.alt,
      };
      const MAX_LIVE_TRAIL = 4096;
      const trail = state.trail.length >= MAX_LIVE_TRAIL
        ? [...state.trail.slice(1), newPt]
        : [...state.trail, newPt];
      return { ...state, nav, trail };
    }
    case 'att':
      return {
        ...state,
        att: {
          qw: action.payload.qw, qx: action.payload.qx,
          qy: action.payload.qy, qz: action.payload.qz,
          rx: action.payload.rx, ry: action.payload.ry, rz: action.payload.rz,
          ax: action.payload.ax, ay: action.payload.ay, az: action.payload.az,
          ts: action.payload.ts,
        },
      };
    case 'mot':
      return {
        ...state,
        mot: { batt: action.payload.batt, m: action.payload.m, ts: action.payload.ts },
      };
    case 'trail':
      return { ...state, trail: action.payload };
    case 'timeline':
      return { ...state, timeline: {
        ts_start:   action.payload.ts_start,
        ts_end:     action.payload.ts_end,
        ts_current: action.payload.ts_current,
        is_live:    action.payload.is_live,
        is_paused:  action.payload.is_paused,
      }};
    case 'sessions':
      return { ...state, sessions: action.payload.list, activeSession: action.payload.current };
    case 'session_selected':
      // Session switched: reset live data, keep session list, update active.
      return {
        ...initialState,
        sessions:      state.sessions,
        activeSession: action.payload.id,
        timeline:      null,
      };
    case 'reset':
      return initialState;
    default:
      return state;
  }
}

// ── Hook ───────────────────────────────────────────────────────────────────

export interface ZoomBridgeResult {
  state:             BridgeState;
  status:            ConnectionStatus;
  sendZoom:          (zoom: number) => void;
  sendSelectSession: (id: string) => void;
  sendSeek:          (ts_us: number) => void;
  sendPause:         (paused: boolean) => void;
}

export function useZoomBridge(url: string): ZoomBridgeResult {
  const [state,  dispatch] = useReducer(reducer, initialState);
  const statusRef = useRef<ConnectionStatus>('connecting');
  const [statusTick, forceUpdate] = useReducer((n: number) => n + 1, 0);

  const clientRef = useRef<WsClient | null>(null);

  useEffect(() => {
    const client = new WsClient(
      url,
      (msg: BridgeMsg) => {
        switch (msg.type) {
          case 'nav':   dispatch({ type: 'nav',   payload: msg }); break;
          case 'att':   dispatch({ type: 'att',   payload: msg }); break;
          case 'mot':   dispatch({ type: 'mot',   payload: msg }); break;
          case 'trail': dispatch({ type: 'trail', payload: msg.pts }); break;
          case 'sessions':
            dispatch({ type: 'sessions', payload: { list: msg.list, current: msg.current } });
            break;
          case 'session_selected':
            dispatch({ type: 'session_selected', payload: { id: msg.id } });
            break;
          case 'timeline':
            dispatch({ type: 'timeline', payload: msg });
            break;
          case 'snap': {
            if (msg.nav) dispatch({ type: 'nav',   payload: { type: 'nav', ...msg.nav } });
            if (msg.att) dispatch({ type: 'att',   payload: { type: 'att', ...msg.att } });
            if (msg.mot) dispatch({ type: 'mot',   payload: { type: 'mot', ...msg.mot } });
            break;
          }
          default: break;
        }
      },
      (s: ConnectionStatus) => {
        statusRef.current = s;
        forceUpdate();
        if (s === 'disconnected') dispatch({ type: 'reset' });
      }
    );
    clientRef.current = client;
    return () => { client.close(); clientRef.current = null; };
  }, [url]);

  const sendZoom = useCallback((zoom: number) => {
    clientRef.current?.send({ type: 'camera', zoom });
  }, []);

  const sendSelectSession = useCallback((id: string) => {
    clientRef.current?.send({ type: 'select_session', id });
  }, []);

  const sendSeek = useCallback((ts_us: number) => {
    clientRef.current?.send({ type: 'seek', ts_us });
  }, []);

  const sendPause = useCallback((paused: boolean) => {
    const msg: PauseMsg | PlayMsg = paused ? { type: 'pause' } : { type: 'play' };
    clientRef.current?.send(msg);
  }, []);

  void statusTick; // used only to trigger re-render on status change
  return { state, status: statusRef.current, sendZoom, sendSelectSession, sendSeek, sendPause };
}
