import test from 'node:test';
import assert from 'node:assert/strict';
import {crc32, makeNavPacket, imagePackets} from '../protocol.mjs';
import {measureRoute, distanceMeters, closestOnRoute, turnAhead, worldPixel} from '../geometry.mjs';
test('CRC32 standard test vector',()=>assert.equal(crc32(new TextEncoder().encode('123456789')),0xcbf43926));
test('navigation fits 20-byte BLE with LE layout',()=>{
  const n=makeNavPacket({turnType:1,turnMeters:120,remainingMeters:3200,speedKmh:18.5,heading:270});
  assert.equal(n.length,12);assert.equal(n[0],5);assert.equal(n[1],1);
  const dv=new DataView(n.buffer);
  assert.equal(dv.getUint16(2,true),120);assert.equal(dv.getUint32(4,true),3200);
  assert.equal(dv.getUint16(8,true),185);assert.equal(dv.getUint16(10,true),270);
});
test('image framing and reassembly 180 and 20 bytes',()=>{
  const b=new Uint8Array(1223).map((_,i)=>i%241);
  for(const n of [180,120,64,20]){
    const chunks=imagePackets(b,n);assert.equal(chunks[0][0],2);assert.equal(chunks.at(-1)[0],4);
    assert.equal(new DataView(chunks[0].buffer).getUint32(1,true),1223);
    assert.equal(new DataView(chunks[0].buffer).getUint32(5,true),crc32(b));
    assert.ok(chunks.every(c=>c.length<=n));
    const contents=chunks.slice(1,-1).flatMap(c=>[...c.slice(1)]);
    assert.deepEqual(contents,[...b]);
  }
});
test('route progress and next turn',()=>{
  const route=[[53.0,8.0],[53.001,8.0],[53.002,8.0]];
  const d=measureRoute(route);assert.ok(d[2]>200 && d[2]<230);
  const near=closestOnRoute([53.0005,8],route,d);
  assert.ok(Math.abs(near.progress-d[1]/2)<2);
  const t=turnAhead([{type:1,way_points:[1,2],instruction:'Turn right'}],d,near.progress);
  assert.equal(t.turnType,1);assert.ok(t.turnMeters>50 && t.turnMeters<60);
});
test('mercator is finite',()=>{
  assert.ok(Number.isFinite(worldPixel(53.07,8.8,16).x));
  assert.ok(distanceMeters([53,8],[53,8.001])>60);
});
