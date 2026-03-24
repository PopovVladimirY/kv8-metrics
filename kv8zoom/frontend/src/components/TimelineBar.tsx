// kv8zoom/frontend/src/components/TimelineBar.tsx
// Absolute-positioned overlay at the bottom of the scene area.
// Displays session progress, allows scrubbing, pause/play, and "Jump to Live".

import React from 'react';
import type { BridgeTimeline } from '../hooks/useZoomBridge';

interface Props {
  timeline:       BridgeTimeline | null;
  onSeek:         (ts_us: number) => void;
  onPause:        (paused: boolean) => void;
}

function formatDuration(us: number): string {
  const totalSec = Math.floor(Math.abs(us) / 1_000_000);
  const m        = Math.floor(totalSec / 60);
  const s        = totalSec % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

export default function TimelineBar({ timeline, onSeek, onPause }: Props): React.ReactElement | null {
  if (!timeline || timeline.ts_start === 0 || timeline.ts_end === 0) return null;

  const duration  = timeline.ts_end   - timeline.ts_start;
  const elapsed   = timeline.ts_current - timeline.ts_start;
  // Clamp: elapsed may briefly exceed duration for a live session.
  const pct       = duration > 0 ? Math.max(0, Math.min(1, elapsed / duration)) : 0;
  // "Near live" = current position is within 2 s of the reported end.
  const nearLive  = timeline.is_live &&
                    (timeline.ts_end - timeline.ts_current) < 2_000_000;
  const showLive  = nearLive && !timeline.is_paused;

  const handleTrackClick = (e: React.MouseEvent<HTMLDivElement>) => {
    const rect = e.currentTarget.getBoundingClientRect();
    const rel  = (e.clientX - rect.left) / rect.width;
    const seekTs = timeline.ts_start + Math.max(0, Math.min(1, rel)) * duration;
    onSeek(Math.floor(seekTs));
    // Seeking always resumes playback (server clears pause on seek).
  };

  const handleJumpToLive = () => {
    onSeek(timeline.ts_end);
    onPause(false);
  };

  return (
    <div className="timeline-bar">
      <div className="timeline-controls-row">
        <button
          className={`timeline-pause-btn${timeline.is_paused ? ' paused' : ''}`}
          onClick={() => onPause(!timeline.is_paused)}
          title={timeline.is_paused ? 'Resume playback' : 'Pause playback'}
        >
          {timeline.is_paused ? '\u25B6' : '\u23F8'}
        </button>
        <div className="timeline-track" onClick={handleTrackClick}>
          <div className="timeline-fill" style={{ width: `${pct * 100}%` }} />
          <div className="timeline-thumb" style={{ left: `${pct * 100}%` }} />
        </div>
      </div>
      <div className="timeline-labels">
        <span className="timeline-elapsed">0:00</span>
        <span className={`timeline-status ${timeline.is_paused ? 'paused' : showLive ? 'live' : 'replay'}`}>
          {timeline.is_paused ? 'PAUSED' : showLive ? 'LIVE' : 'REPLAY'}
          {' '}&ndash;{' '}{formatDuration(elapsed)}
        </span>
        <span className="timeline-total">{formatDuration(duration)}</span>
      </div>
      {timeline.is_live && !nearLive && (
        <button
          className="timeline-live-btn"
          onClick={handleJumpToLive}
        >
          Jump to Live
        </button>
      )}
    </div>
  );
}

