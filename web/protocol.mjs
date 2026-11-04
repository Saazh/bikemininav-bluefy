// BikeMiniNav BLE packets: shared with firmware/src/main.cpp.
export const SERVICE_UUID = '7e9d1000-2a39-4b25-b3e4-cb7d1fc2ac10';
export const WRITE_UUID = '7e9d1001-2a39-4b25-b3e4-cb7d1fc2ac10';
export const MAX_JPEG = 90000;
export const MAP_SIZE = 384;

// ORS step types; firmware shares these numeric codes.
export const TURN_LABELS = [
  'Left', 'Right', 'Sharp left', 'Sharp right', 'Slight left',
  'Slight right', 'Straight', 'Roundabout', 'Exit roundabout',
  'U-turn', 'Destination', 'Start', 'Keep left', 'Keep right'
];

export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    c ^= bytes[i];
    for (let bit = 0; bit < 8; bit++) c = (c >>> 1) ^ ((c & 1) ? 0xedb88320 : 0);
  }
  return (c ^ 0xffffffff) >>> 0;
}
const bounded = (v, limit) => Math.max(0, Math.min(limit, Math.round(Number(v) || 0)));

// 12-byte navigation packet safely fits even a 20-byte GATT write:
// opcode, ORS turn type, turn_m u16, remain_m u32, speed_dkmh u16, heading_deg u16.
export function makeNavPacket({turnType = 6, turnMeters = 0, remainingMeters = 0,
                                speedKmh = 0, heading = 0} = {}) {
  const bytes = new Uint8Array(12);
  const dv = new DataView(bytes.buffer);
  dv.setUint8(0, 0x05);
  dv.setUint8(1, bounded(turnType, 13));
  dv.setUint16(2, bounded(turnMeters, 65535), true);
  dv.setUint32(4, bounded(remainingMeters, 4294967295), true);
  dv.setUint16(8, bounded(speedKmh * 10, 65535), true);
  dv.setUint16(10, bounded(heading, 360), true);
  return bytes;
}
export function imagePackets(jpeg, chunkSize = 180) {
  if (!(jpeg instanceof Uint8Array)) throw new TypeError('jpeg must be Uint8Array');
  if (!jpeg.length || jpeg.length > MAX_JPEG) throw new Error('JPEG exceeds 90,000 bytes');
  if (!Number.isInteger(chunkSize) || chunkSize < 2 || chunkSize > 244) {
    throw new Error('Packet size must be between 2 and 244 bytes');
  }
  const start = new Uint8Array(9);
  const dv = new DataView(start.buffer);
  start[0] = 0x02;
  dv.setUint32(1, jpeg.length, true);
  dv.setUint32(5, crc32(jpeg), true);
  const packets = [start];
  const payloadSize = chunkSize - 1;
  for (let off = 0; off < jpeg.length; off += payloadSize) {
    const n = Math.min(payloadSize, jpeg.length - off);
    const p = new Uint8Array(n + 1);
    p[0] = 0x03;
    p.set(jpeg.subarray(off, off + n), 1);
    packets.push(p);
  }
  packets.push(new Uint8Array([0x04]));
  return packets;
}
