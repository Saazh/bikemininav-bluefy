const R = 6371000;
export function distanceMeters(a, b) {
  const lat1 = a[0] * Math.PI / 180;
  const lat2 = b[0] * Math.PI / 180;
  const dLat = lat2 - lat1;
  const dLon = (b[1] - a[1]) * Math.PI / 180;
  const q = Math.sin(dLat/2)**2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon/2)**2;
  return 2 * R * Math.atan2(Math.sqrt(q), Math.sqrt(Math.max(0, 1-q)));
}
export function measureRoute(coords) {
  const cumulative = [0];
  for (let i = 1; i < coords.length; i++) {
    cumulative.push(cumulative[i-1] + distanceMeters(coords[i-1], coords[i]));
  }
  return cumulative;
}
export function closestOnRoute(latlon, coords, cumulative = measureRoute(coords)) {
  if (!coords.length) return {progress:0, crossTrack:Infinity, segment:0};
  if (coords.length === 1) return {progress:0, crossTrack:distanceMeters(latlon,coords[0]),segment:0};
  const latScale = Math.PI / 180 * R;
  const lonScale = Math.cos(latlon[0]*Math.PI/180) * latScale;
  let best = {progress:0, crossTrack:Infinity, segment:0};
  for (let i = 0; i < coords.length-1; i++) {
    const ax = (coords[i][1]-latlon[1])*lonScale;
    const ay = (coords[i][0]-latlon[0])*latScale;
    const dx = (coords[i+1][1]-coords[i][1])*lonScale;
    const dy = (coords[i+1][0]-coords[i][0])*latScale;
    const f = Math.max(0, Math.min(1, -(ax*dx+ay*dy)/Math.max(1e-9, dx*dx+dy*dy)));
    const err = Math.hypot(ax+f*dx, ay+f*dy);
    if (err < best.crossTrack) best = {
      progress: cumulative[i]+ f*(cumulative[i+1]-cumulative[i]), crossTrack:err, segment:i
    };
  }
  return best;
}
export function turnAhead(steps, cumulative, progress) {
  for (const s of steps ?? []) {
    const startIndex = s.way_points?.[0] ?? 0;
    const startDistance = cumulative[Math.min(Math.max(startIndex, 0), cumulative.length-1)] ?? 0;
    if (startDistance > progress + 3 && (s.type ?? 6) !== 11) {
      return {turnType: s.type ?? 6, turnMeters: Math.round(startDistance-progress),
              instruction:s.instruction || 'Follow route'};
    }
  }
  const end = cumulative.at(-1) || 0;
  return {turnType:10, turnMeters:Math.max(0,Math.round(end-progress)),instruction:'Destination'};
}
export function worldPixel(lat, lon, zoom) {
  const n = Math.pow(2, zoom) * 256;
  const clamped = Math.max(-85.05112878, Math.min(85.05112878, lat));
  const s = Math.sin(clamped * Math.PI / 180);
  return { x: (lon + 180) / 360 * n, y: (0.5 - Math.log((1+s)/(1-s))/(4*Math.PI)) * n };
}
