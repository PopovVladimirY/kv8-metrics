// kv8zoom/frontend/src/components/IsometricView.tsx
// Main Three.js canvas component with OrbitControls and scene management.

import React, { useCallback, useEffect, useRef } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import type { BridgeNav, BridgeAtt, BridgeMot } from '../hooks/useZoomBridge';
import type { TrailPoint } from '../bridge/Protocol';
import { CoordPlane } from '../renderer/CoordPlane';
import { FlightTrail } from '../renderer/FlightTrail';
import { LiveModel }   from '../renderer/LiveModel';
import { VelocityVector } from '../renderer/VelocityVector';
import { useCamera }  from '../hooks/useCamera';
import backgroundUrl from '../../assets/background.jpg';

interface Props {
  nav?:           BridgeNav | null;
  att?:           BridgeAtt | null;
  mot?:           BridgeMot | null;
  trail?:         TrailPoint[];
  onZoomChange?:  (zoom: number) => void;
}

// Expose preset camera positions globally so ViewControls can trigger them.
type PresetFn = (controls: OrbitControls, camera: THREE.PerspectiveCamera) => void;
export const cameraPresets = new Map<string, PresetFn>();

export default function IsometricView({ nav, att, mot, trail, onZoomChange }: Props) {
  const canvasRef  = useRef<HTMLCanvasElement>(null);
  const sceneRef   = useRef<{
    renderer:  THREE.WebGLRenderer;
    scene:     THREE.Scene;
    camera:    THREE.PerspectiveCamera;
    controls:  OrbitControls;
    plane:     CoordPlane;
    trailObj:  FlightTrail;
    model:     LiveModel;
    velVec:    VelocityVector;
    animId:    number;
    lastTime:  number;
  } | null>(null);

  const { onDistanceChange } = useCamera(onZoomChange ?? (() => {}));

  // Initialise scene once on mount.
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    // Renderer
    const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: false });
    renderer.setPixelRatio(window.devicePixelRatio);
    renderer.setSize(canvas.clientWidth, canvas.clientHeight);
    renderer.shadowMap.enabled = true;
    renderer.setClearColor(0x0a0a14); // fallback until texture loads

    // Scene
    const scene = new THREE.Scene();

    // Background image dimmed to 0.2 opacity via an offscreen canvas.
    new THREE.TextureLoader().load(backgroundUrl, (tex) => {
      const img = tex.image as HTMLImageElement;
      const cvs = document.createElement('canvas');
      cvs.width  = img.naturalWidth  || 1920;
      cvs.height = img.naturalHeight || 1080;
      const ctx = cvs.getContext('2d')!;
      ctx.fillStyle = '#0a0a14';
      ctx.fillRect(0, 0, cvs.width, cvs.height);
      ctx.globalAlpha = 0.2;
      ctx.drawImage(img, 0, 0);
      scene.background = new THREE.CanvasTexture(cvs);
    });

    // Camera
    const camera = new THREE.PerspectiveCamera(
      45,
      canvas.clientWidth / canvas.clientHeight,
      0.1,
      100000
    );
    camera.position.set(0, 500, 400);
    camera.lookAt(0, 0, 0);

    // Lighting
    scene.add(new THREE.AmbientLight(0x334466, 0.8));
    const sun = new THREE.DirectionalLight(0xffffff, 1.2);
    sun.position.set(200, 400, 100);
    sun.castShadow = true;
    scene.add(sun);

    // Grid (XZ plane = horizontal ground)
    const grid = new THREE.GridHelper(2000, 100, 0x1a2a3a, 0x1a2a3a);
    scene.add(grid);

    // Axes helper (small)
    scene.add(new THREE.AxesHelper(20));

    // OrbitControls
    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping  = true;
    controls.dampingFactor  = 0.08;
    controls.screenSpacePanning = false;
    controls.minDistance    = 2;
    controls.maxDistance    = 80000;

    // Register preset cameras
    cameraPresets.set('H', (c, cam) => {
      cam.position.set(0, 500, 400);
      c.target.set(0, 0, 0);
      c.update();
    });
    cameraPresets.set('T', (c, cam) => {
      cam.position.set(0, 800, 0.001);
      c.target.set(0, 0, 0);
      c.update();
    });
    cameraPresets.set('F', (c, cam) => {
      cam.position.set(0, 20, 400);
      c.target.set(0, 0, 0);
      c.update();
    });
    cameraPresets.set('L', (c, cam) => {
      cam.position.set(-400, 20, 0);
      c.target.set(0, 0, 0);
      c.update();
    });
    cameraPresets.set('B', (c, cam) => {
      cam.position.set(0, 20, -400);
      c.target.set(0, 0, 0);
      c.update();
    });
    cameraPresets.set('R', (c, cam) => {
      cam.position.set(400, 20, 0);
      c.target.set(0, 0, 0);
      c.update();
    });

    // Coordinate plane & scene objects
    const plane   = new CoordPlane();
    const trailObj = new FlightTrail(scene);
    const model   = new LiveModel(scene);
    const velVec  = new VelocityVector(scene);

    // Animation loop
    let lastTime = performance.now();
    let animId   = 0;

    const animate = (now: number) => {
      animId = requestAnimationFrame(animate);
      const dt = Math.min((now - lastTime) / 1000, 0.1);
      lastTime = now;

      controls.update();
      onDistanceChange(camera.position.length());
      renderer.render(scene, camera);
      void dt;
    };
    animId = requestAnimationFrame(animate);

    // Resize observer
    const ro = new ResizeObserver(() => {
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      renderer.setSize(w, h);
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
    });
    ro.observe(canvas);

    sceneRef.current = { renderer, scene, camera, controls, plane, trailObj, model, velVec, animId, lastTime };

    return () => {
      ro.disconnect();
      cancelAnimationFrame(animId);
      trailObj.dispose();
      model.dispose();
      velVec.dispose();
      controls.dispose();
      renderer.dispose();
      sceneRef.current = null;
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Update model pose when NAV/ATT arrive.
  useEffect(() => {
    const s = sceneRef.current;
    if (!s || !nav) return;

    if (!s.plane.isReady()) s.plane.setHome(nav.lat, nav.lon, nav.alt);
    if (att) {
      s.model.updatePose(nav, att, s.plane);
      s.velVec.update(nav, s.plane);
      s.model.setVisible(true);
      s.velVec.setVisible(true);
    }
  }, [nav, att]);

  // Update motors.
  useEffect(() => {
    if (!sceneRef.current || !mot) return;
    sceneRef.current.model.updateMotors(mot, 0.016);
  }, [mot]);

  // Update trail.
  useEffect(() => {
    const s = sceneRef.current;
    if (!s || !trail || trail.length === 0 || !s.plane.isReady()) return;
    s.trailObj.setPoints(trail, s.plane);
  }, [trail]);

  // Keyboard shortcuts for camera presets.
  const onKeyDown = useCallback((e: React.KeyboardEvent) => {
    const s = sceneRef.current;
    if (!s) return;
    const key = e.key.toUpperCase();
    if (key === ' ') {
      // Follow mode: move camera target to drone position
      if (s.plane.isReady() && nav) {
        const enu = s.plane.toENU(nav.lat, nav.lon, nav.alt);
        s.controls.target.set(enu.east, enu.up, -enu.north);
        s.controls.update();
      }
      return;
    }
    const preset = cameraPresets.get(key);
    if (preset) preset(s.controls, s.camera);
  }, [nav]);

  return (
    <canvas
      ref={canvasRef}
      style={{ width: '100%', height: '100%', display: 'block', outline: 'none' }}
      tabIndex={0}
      onKeyDown={onKeyDown}
    />
  );
}
