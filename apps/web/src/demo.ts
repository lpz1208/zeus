import type { RoadGeoJSON, RoadProperties } from './types'

const center: [number, number] = [116.391, 39.907]

function road(
  roadId: string,
  source: string,
  roadClass: string,
  coordinates: [number, number][],
  speed = 40,
): GeoJSON.Feature<GeoJSON.LineString, RoadProperties> {
  return {
    type: 'Feature',
    properties: {
      ROAD_ID: roadId,
      SOURCE_ID: source,
      CLASS: roadClass,
      EDGE_IDS: '',
      DIRECTION: 'both',
      LENGTH_M: 0,
      SPEED_KPH: speed,
      Z_LEVEL: 0,
    },
    geometry: { type: 'LineString', coordinates },
  }
}

export const demoNetwork: RoadGeoJSON = {
  type: 'FeatureCollection',
  features: [
    road('d-01', 'DEMO-A1', 'arterial', [[116.354, 39.895], [116.373, 39.901], [116.391, 39.907], [116.414, 39.915], [116.432, 39.92]], 60),
    road('d-02', 'DEMO-A2', 'arterial', [[116.36, 39.928], [116.377, 39.918], [116.391, 39.907], [116.404, 39.894], [116.421, 39.884]], 60),
    road('d-03', 'DEMO-R1', 'ring', [[116.362, 39.888], [116.38, 39.881], [116.405, 39.883], [116.425, 39.895], [116.429, 39.911], [116.418, 39.928], [116.395, 39.936], [116.373, 39.93], [116.36, 39.914], [116.362, 39.888]], 80),
    road('d-04', 'DEMO-L1', 'local', [[116.373, 39.901], [116.37, 39.912], [116.373, 39.93]], 30),
    road('d-05', 'DEMO-L2', 'local', [[116.377, 39.918], [116.392, 39.921], [116.418, 39.928]], 30),
    road('d-06', 'DEMO-L3', 'local', [[116.38, 39.881], [116.385, 39.896], [116.391, 39.907], [116.392, 39.921], [116.395, 39.936]], 30),
    road('d-07', 'DEMO-L4', 'local', [[116.362, 39.888], [116.377, 39.891], [116.404, 39.894], [116.425, 39.895]], 30),
    road('d-08', 'DEMO-L5', 'local', [[116.36, 39.914], [116.37, 39.912], [116.391, 39.907], [116.414, 39.915], [116.429, 39.911]], 30),
    road('d-09', 'DEMO-L6', 'local', [[116.373, 39.901], [116.377, 39.891], [116.38, 39.881]], 30),
    road('d-10', 'DEMO-L7', 'local', [[116.405, 39.883], [116.404, 39.894], [116.414, 39.915], [116.418, 39.928]], 30),
    road('d-11', 'DEMO-X1', 'collector', [[116.354, 39.912], [116.37, 39.912], [116.392, 39.921], [116.413, 39.935]], 45),
    road('d-12', 'DEMO-X2', 'collector', [[116.37, 39.88], [116.385, 39.896], [116.404, 39.91], [116.433, 39.93]], 45),
  ],
}

export const demoCenter = center
