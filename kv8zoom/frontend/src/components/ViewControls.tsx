// kv8zoom/frontend/src/components/ViewControls.tsx
// Seven preset-view buttons with keyboard shortcut labels.

import React from 'react';
import { cameraPresets } from './IsometricView';

interface Preset {
  key: string;
  label: string;
  title: string;
}

const PRESETS: Preset[] = [
  { key: 'H', label: 'H',   title: 'Home (isometric)' },
  { key: 'T', label: 'T',   title: 'Top (nadir)' },
  { key: 'F', label: 'F',   title: 'Front' },
  { key: 'L', label: 'L',   title: 'Left' },
  { key: 'B', label: 'B',   title: 'Back' },
  { key: 'R', label: 'R',   title: 'Right' },
  { key: ' ', label: '[Spc]', title: 'Follow drone' },
];

export default function ViewControls(): React.ReactElement {
  const handleClick = (key: string) => {
    // Trigger by dispatching a keyboard event on the canvas (handled in IsometricView).
    const canvas = document.querySelector('canvas');
    if (canvas) {
      canvas.focus();
      canvas.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true }));
    }
    void cameraPresets; // silence unused import warning
  };

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', gap: 4,
      pointerEvents: 'auto'
    }}>
      {PRESETS.map(p => (
        <button
          key={p.key}
          title={p.title}
          onClick={() => handleClick(p.key)}
          style={{
            background: '#1a2a3a',
            color: '#88ccee',
            border: '1px solid #2a4a6a',
            borderRadius: 3,
            padding: '3px 8px',
            fontFamily: 'monospace',
            fontSize: 11,
            cursor: 'pointer',
            minWidth: 44,
          }}
        >
          {p.label}
        </button>
      ))}
    </div>
  );
}
