// kv8zoom/frontend/src/components/AttitudeIndicator.tsx
// SVG artificial horizon showing pitch and roll from the attitude quaternion.

import React, { useMemo } from 'react';
import type { BridgeAtt } from '../hooks/useZoomBridge';

interface Props {
  att?: BridgeAtt | null;
}

function quatToEuler(qw: number, qx: number, qy: number, qz: number): { roll: number; pitch: number } {
  // roll (x-axis rotation)
  const sinrCosp = 2 * (qw * qx + qy * qz);
  const cosrCosp = 1 - 2 * (qx * qx + qy * qy);
  const roll = Math.atan2(sinrCosp, cosrCosp);

  // pitch (y-axis rotation)
  const sinp = 2 * (qw * qy - qz * qx);
  const pitch = Math.abs(sinp) >= 1
    ? Math.sign(sinp) * (Math.PI / 2)
    : Math.asin(sinp);

  return { roll: roll * 180 / Math.PI, pitch: pitch * 180 / Math.PI };
}

export default function AttitudeIndicator({ att }: Props): React.ReactElement {
  const { roll, pitch } = useMemo(() => {
    if (!att) return { roll: 0, pitch: 0 };
    return quatToEuler(att.qw, att.qx, att.qy, att.qz);
  }, [att]);

  const size = 120;
  const cx   = size / 2;
  const cy   = size / 2;
  const r    = size / 2 - 4;

  // Horizon Y offset based on pitch (1 px per degree, capped)
  const pitchOffset = Math.max(-r, Math.min(r, pitch));
  const horizonY    = cy + pitchOffset;

  return (
    <div className="panel-card">
      <h3>Attitude</h3>
      <svg width={size} height={size} style={{ display: 'block', margin: '0 auto' }}>
        <defs>
          <clipPath id="ai-clip">
            <circle cx={cx} cy={cy} r={r} />
          </clipPath>
        </defs>

        {/* Sky */}
        <g clipPath="url(#ai-clip)" transform={`rotate(${-roll}, ${cx}, ${cy})`}>
          <rect x={0} y={0} width={size} height={horizonY} fill="#1a3a6a" />
          {/* Ground */}
          <rect x={0} y={horizonY} width={size} height={size} fill="#6a3a1a" />
          {/* Horizon line */}
          <line x1={0} y1={horizonY} x2={size} y2={horizonY}
                stroke="#ffffff" strokeWidth={1.5} />
          {/* Pitch lines (5 deg each) */}
          {[-20, -10, 10, 20].map(deg => {
            const y = cy + pitchOffset - deg;
            const w = deg % 10 === 0 ? 30 : 16;
            return (
              <line key={deg} x1={cx - w/2} y1={y} x2={cx + w/2} y2={y}
                    stroke="#ffffff80" strokeWidth={1} />
            );
          })}
        </g>

        {/* Fixed aircraft symbol */}
        <line x1={cx - 24} y1={cy} x2={cx - 8} y2={cy} stroke="#ffff00" strokeWidth={2} />
        <line x1={cx + 8}  y1={cy} x2={cx + 24} y2={cy} stroke="#ffff00" strokeWidth={2} />
        <circle cx={cx} cy={cy} r={3} fill="#ffff00" />

        {/* Roll indicator arc */}
        <circle cx={cx} cy={cy} r={r} fill="none" stroke="#334455" strokeWidth={1} />
      </svg>

      <div className="panel-row">
        <span className="label">Roll</span>
        <span className="value">{roll.toFixed(1)}&deg;</span>
      </div>
      <div className="panel-row">
        <span className="label">Pitch</span>
        <span className="value">{pitch.toFixed(1)}&deg;</span>
      </div>
    </div>
  );
}
