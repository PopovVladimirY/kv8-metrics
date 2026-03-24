// kv8zoom/frontend/src/components/EnuReadout.tsx
// ENU coordinate readout relative to the home position (first NAV fix).

import React, { useEffect, useRef, useState } from 'react';
import type { BridgeNav } from '../hooks/useZoomBridge';
import { CoordPlane } from '../renderer/CoordPlane';

interface Props {
  nav?: BridgeNav | null;
}

export default function EnuReadout({ nav }: Props): React.ReactElement {
  const planeRef = useRef(new CoordPlane());
  const [enu, setEnu] = useState<{ east: number; north: number; up: number } | null>(null);

  useEffect(() => {
    if (!nav) return;
    if (!planeRef.current.isReady()) {
      planeRef.current.setHome(nav.lat, nav.lon, nav.alt);
    }
    setEnu(planeRef.current.toENU(nav.lat, nav.lon, nav.alt));
  }, [nav]);

  const range = enu
    ? Math.sqrt(enu.east ** 2 + enu.north ** 2 + enu.up ** 2).toFixed(1)
    : '--';

  return (
    <div className="panel-card">
      <h3>ENU (from home)</h3>
      {enu ? (
        <>
          <div className="panel-row">
            <span className="label">East</span>
            <span className="value">{enu.east.toFixed(1)} m</span>
          </div>
          <div className="panel-row">
            <span className="label">North</span>
            <span className="value">{enu.north.toFixed(1)} m</span>
          </div>
          <div className="panel-row">
            <span className="label">Up</span>
            <span className="value">{enu.up.toFixed(1)} m</span>
          </div>
          <div className="panel-row">
            <span className="label">Range</span>
            <span className="value">{range} m</span>
          </div>
        </>
      ) : (
        <div className="panel-row"><span className="label">Waiting for home fix...</span></div>
      )}
    </div>
  );
}
