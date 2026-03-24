// kv8zoom/frontend/src/components/SessionPicker.tsx
// Displays available sessions and lets the user select one.

import React from 'react';
import type { SessionInfo } from '../bridge/Protocol';

interface Props {
  sessions:      SessionInfo[];
  activeSession: string;
  onSelect:      (id: string) => void;
}

function formatTs(ts_ms: number): string {
  if (!ts_ms) return '';
  const d = new Date(ts_ms);
  return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' })
    + ' ' + d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
}

export default function SessionPicker({ sessions, activeSession, onSelect }: Props): React.ReactElement {
  if (sessions.length === 0) {
    return (
      <div className="session-picker">
        <div className="session-picker-empty">No sessions found</div>
      </div>
    );
  }

  return (
    <div className="session-picker">
      <div className="session-picker-header">Sessions</div>
      <ul className="session-list">
        {sessions.map((s) => (
          <li
            key={s.id}
            className={`session-item${s.id === activeSession ? ' active' : ''}`}
            onClick={() => onSelect(s.id)}
            title={s.prefix}
          >
            <span className="session-name">
              {s.name || s.id}
            </span>
            {s.is_live && (
              <span className="session-live-badge">LIVE</span>
            )}
            <span className="session-meta">
              {s.feeds > 0 && <span className="session-feeds">{s.feeds}F</span>}
              {s.ts_start > 0 && (
                <span className="session-ts">{formatTs(s.ts_start)}</span>
              )}
            </span>
          </li>
        ))}
      </ul>
    </div>
  );
}
