// kv8zoom/frontend/src/bridge/Protocol.ts
// Wire-format types matching the JSON produced by Serializer.cpp.

export interface NavMsg {
  type: 'nav';
  ts:   number;
  lat:  number;
  lon:  number;
  alt:  number;
  vx:   number;
  vy:   number;
  vz:   number;
  fix:  number;
  sats: number;
  hdop: number;
  baro: number;
}

export interface AttMsg {
  type: 'att';
  ts:   number;
  qw:   number;
  qx:   number;
  qy:   number;
  qz:   number;
  rx:   number;
  ry:   number;
  rz:   number;
  ax:   number;
  ay:   number;
  az:   number;
}

export interface MotMsg {
  type: 'mot';
  ts:   number;
  batt: number;
  m:    [number, number, number, number];
}

export interface WxMsg {
  type: 'wx';
  ts:   number;
  temp: number;
  hum:  number;
  pres: number;
  ws:   number;
  wd:   number;
  rain: number;
  uv:   number;
}

export interface TrailPoint {
  ts:  number;
  lat: number;
  lon: number;
  alt: number;
}

export interface TrailMsg {
  type: 'trail';
  pts:  TrailPoint[];
}

export interface SnapNavData {
  ts:   number;
  lat:  number;
  lon:  number;
  alt:  number;
  vx:   number;
  vy:   number;
  vz:   number;
  fix:  number;
  sats: number;
  hdop: number;
  baro: number;
}

export interface SnapAttData {
  ts: number;
  qw: number; qx: number; qy: number; qz: number;
  rx: number; ry: number; rz: number;
  ax: number; ay: number; az: number;
}

export interface SnapMotData {
  ts:   number;
  batt: number;
  m:    [number, number, number, number];
}

export interface SnapWxData {
  ts:   number;
  temp: number; hum: number; pres: number;
  ws:   number; wd:  number; rain: number; uv: number;
}

export interface SnapMsg {
  type: 'snap';
  nav?: SnapNavData;
  att?: SnapAttData;
  mot?: SnapMotData;
  wx?:  SnapWxData;
}

export interface MetaMsg {
  type:    'meta';
  channel: string;
  feeds:   number;
  zoom:    number;
}

export interface ErrorMsg {
  type: 'error';
  msg:  string;
}

// ── Session management messages ──────────────────────────────────────────────

export interface SessionInfo {
  id:       string;
  name:     string;
  prefix:   string;
  ts_start: number; // ms since Unix epoch, 0 if unknown
  feeds:    number;
  is_live:  boolean;
}

export interface SessionsMsg {
  type:    'sessions';
  list:    SessionInfo[];
  current: string;
}

export interface SessionSelectedMsg {
  type: 'session_selected';
  id:   string;
  name: string;
}

// ── Outbound messages (client -> server) ─────────────────────────────────────

export interface CameraMsg {
  type: 'camera';
  zoom: number;
}

export interface SelectSessionMsg {
  type: 'select_session';
  id:   string;
}

export interface SeekMsg {
  type:  'seek';
  ts_us: number; // microseconds; 0 = replay from beginning
}

export interface PauseMsg { type: 'pause'; }
export interface PlayMsg  { type: 'play';  }

// ── Inbound timeline message ──────────────────────────────────────────────────

export interface TimelineMsg {
  type:       'timeline';
  ts_start:   number; // microseconds
  ts_end:     number;
  ts_current: number;
  is_live:    boolean;
  is_paused:  boolean;
}

export type BridgeMsg =
  | NavMsg | AttMsg | MotMsg | WxMsg
  | TrailMsg | SnapMsg | MetaMsg | ErrorMsg
  | SessionsMsg | SessionSelectedMsg | TimelineMsg;
