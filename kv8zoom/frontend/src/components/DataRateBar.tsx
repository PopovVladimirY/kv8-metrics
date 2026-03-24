// kv8zoom/frontend/src/components/DataRateBar.tsx
// Connection status indicator at the top of the side panel.

import React from 'react';
import type { ConnectionStatus } from '../bridge/WsClient';

interface Props {
  status: ConnectionStatus;
}

const STATUS_COLOR: Record<ConnectionStatus, string> = {
  connecting:   '#f39c12',
  connected:    '#2ecc71',
  disconnected: '#e74c3c',
};

const STATUS_LABEL: Record<ConnectionStatus, string> = {
  connecting:   'Connecting...',
  connected:    'Connected',
  disconnected: 'Disconnected',
};

export default function DataRateBar({ status }: Props): React.ReactElement {
  return (
    <div className="panel-card" style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
      <span
        className="status-dot"
        style={{ background: STATUS_COLOR[status] }}
      />
      <span style={{ color: STATUS_COLOR[status], fontWeight: 'bold', fontSize: 11 }}>
        {STATUS_LABEL[status]}
      </span>
      <span style={{ color: '#445', marginLeft: 'auto', fontSize: 10 }}>
        ws://localhost:9001
      </span>
    </div>
  );
}
