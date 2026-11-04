// Pure presentation helpers. Kept separate so route previewing can be tested on Linux.
export const TURN_SYMBOLS = ['↰','↱','↶','↷','↖','↗','↑','⤵','⤴','↩','◆','↑','↖','↗'];
export function turnSymbol(type) { return TURN_SYMBOLS[type] || '↑'; }
export function formatDistance(meters) {
  const m = Math.max(0, Number(meters) || 0);
  return m < 1000 ? `${Math.round(m)} m` : `${(m/1000).toFixed(1)} km`;
}
export function formatDuration(seconds) {
  const mins = Math.max(0, Math.round((Number(seconds)||0)/60));
  return mins >= 60 ? `${Math.floor(mins/60)} h ${mins%60} min` : `${mins} min`;
}
export function getManeuvers(steps, coords, cumulative) {
  if (!Array.isArray(steps) || !Array.isArray(coords) || !coords.length) return [];
  return steps.flatMap((step, stepIndex) => {
    const type = Number(step.type ?? 6);
    // Only intentional maneuver points are useful as map badges.
    if (type === 6 || type === 11) return [];
    const vertex = Math.max(0, Math.min(coords.length - 1, Number(step.way_points?.[0] ?? 0)));
    const latlon = coords[vertex];
    if (!Array.isArray(latlon) || !Number.isFinite(latlon[0]) || !Number.isFinite(latlon[1])) return [];
    return [{stepIndex, type, icon:turnSymbol(type), latlon,
      instruction: String(step.instruction || (type === 10 ? 'Arrive at destination' : 'Continue the route')),
      distanceFromStart: cumulative?.[vertex] || 0}];
  });
}
