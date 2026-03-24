import React from 'react';
import IsometricView from './components/IsometricView';
import ViewControls  from './components/ViewControls';
import NavStatus     from './components/NavStatus';
import AttitudeIndicator from './components/AttitudeIndicator';
import MotorRing     from './components/MotorRing';
import DataRateBar   from './components/DataRateBar';
import EnuReadout    from './components/EnuReadout';
import SessionPicker from './components/SessionPicker';
import TimelineBar   from './components/TimelineBar';
import { useZoomBridge } from './hooks/useZoomBridge';

const WS_URL = 'ws://localhost:9001';

export default function App(): React.ReactElement {
  const { state, status, sendZoom, sendSelectSession, sendSeek, sendPause } = useZoomBridge(WS_URL);

  return (
    <div className="app">
      {/* 3-D scene */}
      <div className="scene-area">
        <IsometricView
          nav={state.nav}
          att={state.att}
          mot={state.mot}
          trail={state.trail}
          onZoomChange={sendZoom}
        />
        <div className="overlay-controls">
          <ViewControls />
        </div>
        <TimelineBar timeline={state.timeline} onSeek={sendSeek} onPause={sendPause} />
      </div>

      {/* Right-hand instrumentation panel */}
      <aside className="side-panel">
        <DataRateBar status={status} />
        <SessionPicker
          sessions={state.sessions}
          activeSession={state.activeSession}
          onSelect={sendSelectSession}
        />
        <NavStatus   nav={state.nav} />
        <AttitudeIndicator att={state.att} />
        <MotorRing   mot={state.mot} />
        <EnuReadout  nav={state.nav} />
      </aside>
    </div>
  );
}
