// kv8zoom/frontend/src/hooks/useCamera.ts
// Computes the slippy-map zoom equivalent from Three.js camera distance.

import { useCallback, useRef } from 'react';

const EARTH_CIRCUMFERENCE_M = 40_075_016;
const FOV_DEG               = 45;
const FOV_RAD               = (FOV_DEG * Math.PI) / 180;

/**
 * Convert a Three.js camera distance (metres in ENU space) to an approximate
 * slippy-map zoom level using the standard zoom formula:
 *   zoom = log2(C / (2 * dist * tan(fov/2))) - 1
 *
 * Result is clamped to [0, 22].
 */
export function distanceToZoom(distM: number): number {
  if (distM <= 0) return 22;
  const zoom = Math.log2(EARTH_CIRCUMFERENCE_M / (2 * distM * Math.tan(FOV_RAD / 2))) - 1;
  return Math.max(0, Math.min(22, Math.round(zoom)));
}

/**
 * Hook that tracks the last-sent zoom level and returns a debounced sender.
 * The caller provides the onChange callback to forward zoom to the bridge.
 */
export function useCamera(onChange: (zoom: number) => void) {
  const lastZoom = useRef(-1);

  const onDistanceChange = useCallback((distM: number) => {
    const zoom = distanceToZoom(distM);
    if (zoom !== lastZoom.current) {
      lastZoom.current = zoom;
      onChange(zoom);
    }
  }, [onChange]);

  return { onDistanceChange };
}
