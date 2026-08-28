import type { PlaybackData, PlaybackVehicle, VehicleFrameGeoJSON } from './types'

// Binary-search the samples and linearly interpolate at simulation time t.
// Outside the sampled interval the nearest endpoint is held; visibility is
// handled separately from interpolation by buildVehicleFrame.
export function interpolateAt(
  samples: PlaybackVehicle['samples'],
  t: number,
): [number, number] | null {
  if (samples.length === 0) {
    return null
  }
  const first = samples[0]
  const last = samples[samples.length - 1]
  if (t <= first[0]) return [first[1], first[2]]
  if (t >= last[0]) return [last[1], last[2]]
  let low = 0
  let high = samples.length - 1
  while (high - low > 1) {
    const mid = (low + high) >> 1
    if (samples[mid][0] <= t) {
      low = mid
    } else {
      high = mid
    }
  }
  const a = samples[low]
  const b = samples[high]
  const span = b[0] - a[0]
  if (span <= 1e-9) {
    return [a[1], a[2]]
  }
  const ratio = (t - a[0]) / span
  return [a[1] + (b[1] - a[1]) * ratio, a[2] + (b[2] - a[2]) * ratio]
}

// All visible vehicles at time t as a point collection for the map layer.
export function buildVehicleFrame(
  data: PlaybackData,
  t: number,
): VehicleFrameGeoJSON {
	const vehicleState = new Map<number, { held: boolean; speedFactor: number }>()
	for (const control of data.controls ?? []) {
		if (control.scope !== 'vehicle' || control.effective_s > t + 1e-9) continue
		const state = vehicleState.get(control.target_id) ?? { held: false, speedFactor: 1 }
		if (control.action === 'hold') state.held = true
		if (control.action === 'release') state.held = false
		if (control.action === 'speed_factor') state.speedFactor = control.value
		vehicleState.set(control.target_id, state)
	}
  return {
    type: 'FeatureCollection',
    features: data.vehicles.flatMap((vehicle) => {
      const depart = vehicle.depart_s ?? Number.POSITIVE_INFINITY
      const end = vehicle.arrive_s ?? data.duration_s
      if (t < depart - 1e-9 || t > end + 1e-9) return []
      const position = interpolateAt(vehicle.samples, t)
      if (!position) {
        return []
      }
		const state = vehicleState.get(vehicle.id) ?? { held: false, speedFactor: 1 }
      return [{
        type: 'Feature' as const,
		properties: {
			VEHICLE_ID: vehicle.id,
			HELD: state.held,
			SPEED_FACTOR: state.speedFactor,
		},
        geometry: { type: 'Point' as const, coordinates: position },
      }]
    }),
  }
}
