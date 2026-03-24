// kv8zoom/frontend/src/renderer/LiveModel.ts
// Positions a drone model (or fallback geometry) and applies attitude quaternion.

import * as THREE from 'three';
import type { BridgeAtt, BridgeNav } from '../hooks/useZoomBridge';
import type { BridgeMot } from '../hooks/useZoomBridge';
import { CoordPlane } from './CoordPlane';

const ARM_LENGTH = 0.18;   // motor arm length in Three.js units (scene scale)
const PROP_RADIUS = 0.06;

export class LiveModel {
  private root:     THREE.Group;
  private motorRings: THREE.Mesh[] = [];
  private propAngles  = [0, 0, 0, 0];

  constructor(scene: THREE.Scene) {
    this.root = new THREE.Group();

    // Build a simple cross-shaped drone body as fallback (no GLTF dependency).
    const bodyGeo  = new THREE.BoxGeometry(0.05, 0.02, 0.05);
    const bodyMat  = new THREE.MeshPhongMaterial({ color: 0x222244 });
    this.root.add(new THREE.Mesh(bodyGeo, bodyMat));

    // Four arms
    const armDirs: [number, number][] = [
      [ ARM_LENGTH,  ARM_LENGTH],
      [-ARM_LENGTH,  ARM_LENGTH],
      [ ARM_LENGTH, -ARM_LENGTH],
      [-ARM_LENGTH, -ARM_LENGTH],
    ];

    const armGeo = new THREE.CylinderGeometry(0.005, 0.005, ARM_LENGTH, 6);
    const armMat = new THREE.MeshPhongMaterial({ color: 0x334455 });

    for (let i = 0; i < 4; ++i) {
      const armPivot = new THREE.Group();
      armPivot.rotation.z = Math.atan2(armDirs[i][1], armDirs[i][0]);
      armPivot.position.set(
        armDirs[i][0] * 0.5,
        0,
        armDirs[i][1] * 0.5
      );
      armPivot.add(new THREE.Mesh(armGeo, armMat));
      this.root.add(armPivot);

      // Motor ring (spinning propeller indicator)
      const ringGeo = new THREE.TorusGeometry(PROP_RADIUS, 0.006, 6, 24);
      const ringMat = new THREE.MeshPhongMaterial({
        color: i % 2 === 0 ? 0x44aa88 : 0x8844aa,
        transparent: true, opacity: 0.75
      });
      const ring = new THREE.Mesh(ringGeo, ringMat);
      ring.position.set(armDirs[i][0], 0.01, armDirs[i][1]);
      ring.rotation.x = Math.PI / 2;
      this.root.add(ring);
      this.motorRings.push(ring);
    }

    scene.add(this.root);
  }

  updatePose(nav: BridgeNav, att: BridgeAtt, plane: CoordPlane): void {
    const enu = plane.toENU(nav.lat, nav.lon, nav.alt);
    this.root.position.set(enu.east, enu.up, -enu.north);

    // Apply quaternion (bridge: w,x,y,z in aerospace NED -> Three.js ENU).
    // The bridge uses the same frame so map directly: x=East, y=Up, z=-North.
    this.root.quaternion.set(att.qx, att.qz, -att.qy, att.qw);
  }

  updateMotors(mot: BridgeMot, dt: number): void {
    for (let i = 0; i < 4; ++i) {
      const rpm = mot.m[i] * 12000; // normalised [0,1] -> ~12 000 rpm max
      const angularSpeed = (rpm / 60) * 2 * Math.PI; // rad/s
      this.propAngles[i] += angularSpeed * dt;
      this.motorRings[i].rotation.z = this.propAngles[i];
    }
  }

  setVisible(v: boolean): void { this.root.visible = v; }

  dispose(): void {
    this.root.traverse((obj) => {
      if (obj instanceof THREE.Mesh) {
        obj.geometry.dispose();
        if (Array.isArray(obj.material)) obj.material.forEach(m => m.dispose());
        else obj.material.dispose();
      }
    });
  }
}
