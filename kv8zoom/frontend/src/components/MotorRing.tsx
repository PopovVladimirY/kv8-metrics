// kv8zoom/frontend/src/components/MotorRing.tsx
// Four motor throttle bars with battery voltage.

import React from 'react';
import type { BridgeMot } from '../hooks/useZoomBridge';

interface Props {
  mot?: BridgeMot | null;
}

function ThrottleBar({ value, label }: { value: number; label: string }) {
  const pct = Math.round(Math.min(1, Math.max(0, value)) * 100);
  const hue  = 120 - pct * 1.2; // green -> yellow -> red
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 3 }}>
      <span style={{ color: '#888', width: 16, fontSize: 10 }}>{label}</span>
      <div style={{
        flex: 1, height: 8, background: '#1a2030',
        borderRadius: 2, overflow: 'hidden'
      }}>
        <div style={{
          width: `${pct}%`, height: '100%',
          background: `hsl(${hue}, 80%, 50%)`,
          transition: 'width 0.1s ease',
        }} />
      </div>
      <span style={{ color: '#aac', width: 28, fontSize: 10, textAlign: 'right' }}>
        {pct}%
      </span>
    </div>
  );
}

export default function MotorRing({ mot }: Props): React.ReactElement {
  return (
    <div className="panel-card">
      <h3>Motors</h3>
      {mot ? (
        <>
          <div className="panel-row" style={{ marginBottom: 4 }}>
            <span className="label">Battery</span>
            <span className="value" style={{
              color: mot.batt < 13.0 ? '#e74c3c' : mot.batt < 14.0 ? '#f39c12' : '#2ecc71'
            }}>
              {mot.batt.toFixed(2)} V
            </span>
          </div>
          <ThrottleBar value={mot.m[0]} label="M1" />
          <ThrottleBar value={mot.m[1]} label="M2" />
          <ThrottleBar value={mot.m[2]} label="M3" />
          <ThrottleBar value={mot.m[3]} label="M4" />
        </>
      ) : (
        <div className="panel-row"><span className="label">No data</span></div>
      )}
    </div>
  );
}
