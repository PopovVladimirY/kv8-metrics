// kv8zoom/frontend/src/renderer/FlightTrail.ts
// Manages a growing Three.js line geometry that represents the flight path.

import * as THREE from 'three';
import type { TrailPoint } from '../bridge/Protocol';
import { CoordPlane } from './CoordPlane';

const MAX_TRAIL_POINTS = 4096;

export class FlightTrail {
  private geometry:   THREE.BufferGeometry;
  private positions:  Float32Array;
  private colors:     Float32Array;
  private line:       THREE.Line;

  constructor(scene: THREE.Scene) {
    this.positions = new Float32Array(MAX_TRAIL_POINTS * 3);
    this.colors    = new Float32Array(MAX_TRAIL_POINTS * 3);

    this.geometry = new THREE.BufferGeometry();
    this.geometry.setAttribute(
      'position',
      new THREE.BufferAttribute(this.positions, 3).setUsage(THREE.DynamicDrawUsage)
    );
    this.geometry.setAttribute(
      'color',
      new THREE.BufferAttribute(this.colors, 3).setUsage(THREE.DynamicDrawUsage)
    );

    const mat = new THREE.LineBasicMaterial({ vertexColors: true, linewidth: 2 });
    this.line = new THREE.Line(this.geometry, mat);
    this.line.frustumCulled = false;
    scene.add(this.line);
  }

  /**
   * Replace the trail with the simplified set of points sent by the bridge.
   * The colour fades from dark cyan (oldest) to bright white (newest).
   */
  setPoints(pts: TrailPoint[], plane: CoordPlane): void {
    const n = Math.min(pts.length, MAX_TRAIL_POINTS);

    for (let i = 0; i < n; ++i) {
      const enu = plane.toENU(pts[i].lat, pts[i].lon, pts[i].alt);
      const base = i * 3;
      this.positions[base]     = enu.east;
      this.positions[base + 1] = enu.up;
      this.positions[base + 2] = -enu.north; // Three.js Z is southward in ENU

      const t = n > 1 ? i / (n - 1) : 1;
      this.colors[base]     = 0.1 + 0.9 * t;
      this.colors[base + 1] = 0.4 + 0.6 * t;
      this.colors[base + 2] = 0.6 + 0.4 * t;
    }

    this.geometry.setDrawRange(0, n);
    (this.geometry.attributes['position'] as THREE.BufferAttribute).needsUpdate = true;
    (this.geometry.attributes['color']    as THREE.BufferAttribute).needsUpdate = true;
  }

  setVisible(v: boolean): void { this.line.visible = v; }

  dispose(): void {
    this.geometry.dispose();
    (this.line.material as THREE.Material).dispose();
  }
}
