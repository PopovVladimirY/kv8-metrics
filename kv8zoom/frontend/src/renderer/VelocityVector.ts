// kv8zoom/frontend/src/renderer/VelocityVector.ts
// Renders an arrow in the scene representing the current velocity vector.

import * as THREE from 'three';
import type { BridgeNav } from '../hooks/useZoomBridge';
import { CoordPlane } from './CoordPlane';

const SCALE = 0.5;    // metres per m/s (visual scale factor)

export class VelocityVector {
  private arrow: THREE.ArrowHelper;

  constructor(scene: THREE.Scene) {
    this.arrow = new THREE.ArrowHelper(
      new THREE.Vector3(0, 0, -1),
      new THREE.Vector3(0, 0,  0),
      1.0,
      0x00ffaa,
      0.15,
      0.08
    );
    this.arrow.visible = false;
    scene.add(this.arrow);
  }

  update(nav: BridgeNav, plane: CoordPlane): void {
    const enu = plane.toENU(nav.lat, nav.lon, nav.alt);
    this.arrow.position.set(enu.east, enu.up, -enu.north);

    const speed = Math.sqrt(nav.vx * nav.vx + nav.vy * nav.vy + nav.vz * nav.vz);
    if (speed < 0.1) {
      this.arrow.visible = false;
      return;
    }

    // ENU velocity: east=vx, north=vy, up=vz  ->  Three.js: x=east, y=up, z=-north
    const dir = new THREE.Vector3(nav.vx, nav.vz, -nav.vy).normalize();
    this.arrow.setDirection(dir);
    this.arrow.setLength(speed * SCALE, Math.min(0.3, speed * SCALE * 0.2), 0.08);
    this.arrow.visible = true;
  }

  setVisible(v: boolean): void { this.arrow.visible = v; }

  dispose(): void {
    this.arrow.line.geometry.dispose();
    this.arrow.cone.geometry.dispose();
    (this.arrow.line.material as THREE.Material).dispose();
    (this.arrow.cone.material as THREE.Material).dispose();
  }
}
