// kv8zoom/frontend/src/components/NavStatus.tsx
// GPS fix and position readout panel.

import React from 'react';
import type { BridgeNav } from '../hooks/useZoomBridge';

interface Props {
  nav?: BridgeNav | null;
}

const FIX_LABELS: Record<number, string> = {
  0: 'No fix',
  1: 'Dead reck.',
  2: '2D',
  3: '3D',
  4: 'GNSS+DR',
  5: 'Time only',
};

export default function NavStatus({ nav }: Props): React.ReactElement {
  if (!nav) {
    return (
      <div className="panel-card">
        <h3>Navigation</h3>
        <div className="panel-row"><span className="label">Waiting for data...</span></div>
      </div>
    );
  }

  const speed3d = Math.sqrt(nav.vx ** 2 + nav.vy ** 2 + nav.vz ** 2);
  const speed2d = Math.sqrt(nav.vx ** 2 + nav.vy ** 2);

  return (
    <div className="panel-card">
      <h3>Navigation</h3>
      <div className="panel-row">
        <span className="label">Fix</span>
        <span className="value">{FIX_LABELS[nav.fix] ?? nav.fix} / {nav.sats} sats</span>
      </div>
      <div className="panel-row">
        <span className="label">Lat</span>
        <span className="value">{nav.lat.toFixed(6)}&deg;</span>
      </div>
      <div className="panel-row">
        <span className="label">Lon</span>
        <span className="value">{nav.lon.toFixed(6)}&deg;</span>
      </div>
      <div className="panel-row">
        <span className="label">Alt GPS</span>
        <span className="value">{nav.alt.toFixed(1)} m</span>
      </div>
      <div className="panel-row">
        <span className="label">Alt Baro</span>
        <span className="value">{nav.baro.toFixed(1)} m</span>
      </div>
      <div className="panel-row">
        <span className="label">Speed 3D</span>
        <span className="value">{speed3d.toFixed(1)} m/s</span>
      </div>
      <div className="panel-row">
        <span className="label">Speed 2D</span>
        <span className="value">{speed2d.toFixed(1)} m/s</span>
      </div>
      <div className="panel-row">
        <span className="label">Vx/Vy/Vz</span>
        <span className="value">{nav.vx.toFixed(1)}/{nav.vy.toFixed(1)}/{nav.vz.toFixed(1)}</span>
      </div>
      <div className="panel-row">
        <span className="label">HDOP</span>
        <span className="value">{nav.hdop.toFixed(2)}</span>
      </div>
    </div>
  );
}
