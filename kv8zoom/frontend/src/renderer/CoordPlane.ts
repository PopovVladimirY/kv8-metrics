// kv8zoom/frontend/src/renderer/CoordPlane.ts
// ENU (East-North-Up) reference frame helpers.

const DEG2RAD          = Math.PI / 180;
const EARTH_RADIUS_M   = 6_371_000;

export interface ENUPoint {
  east:  number; // metres
  north: number; // metres
  up:    number; // metres
}

/**
 * Converts geographic coordinates to ENU metres relative to a home reference
 * using a flat-earth (small-angle) approximation, valid up to ~50 km from home.
 */
export class CoordPlane {
  private homeLat = 0;
  private homeLon = 0;
  private homeAlt = 0;
  private hasHome = false;

  setHome(lat: number, lon: number, alt: number): void {
    this.homeLat = lat;
    this.homeLon = lon;
    this.homeAlt = alt;
    this.hasHome = true;
  }

  isReady(): boolean {
    return this.hasHome;
  }

  toENU(lat: number, lon: number, alt: number): ENUPoint {
    const cosLat = Math.cos(this.homeLat * DEG2RAD);
    return {
      east:  EARTH_RADIUS_M * cosLat * (lon - this.homeLon) * DEG2RAD,
      north: EARTH_RADIUS_M           * (lat - this.homeLat) * DEG2RAD,
      up:    alt - this.homeAlt,
    };
  }

  /** Return the ENU scale factor in metres per pixel at a given camera distance. */
  static metersPerPixel(cameraDistM: number, canvasHeightPx: number, fovDeg = 45): number {
    const halfHeight = cameraDistM * Math.tan((fovDeg * DEG2RAD) / 2);
    return (2 * halfHeight) / canvasHeightPx;
  }
}
