import { MAP_SIZE } from './protocol.mjs';
import { worldPixel } from './geometry.mjs';

/** 288x288 pixel map sent to the watch. Loads only currently visible OSM tiles.
 * Browser caching honors OSM response headers. No offline caching or prefetching.
 */
export class MiniMapRenderer {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', {alpha:false});
    this.images = new Map();
  }
  tile(z, x, y) {
    const n = 2**z;
    if (y < 0 || y >= n) return Promise.resolve(null);
    x = ((x % n) + n) % n;
    const key = `${z}/${x}/${y}`;
    if (!this.images.has(key)) {
      const promise = new Promise((resolve, reject) => {
        const img = new Image();
        img.crossOrigin = 'anonymous'; // Required before src, so canvas isn't tainted.
        img.onload = () => resolve(img);
        img.onerror = () => reject(new Error(`Cannot load OSM tile ${key} (CORS/network)`));
        img.src = `https://tile.openstreetmap.org/${key}.png`;
      });
      this.images.set(key, promise);
      // Keep a small memory cache; the browser's HTTP cache handles older tiles.
      if (this.images.size > 45) this.images.delete(this.images.keys().next().value);
      promise.catch(() => { if (this.images.get(key) === promise) this.images.delete(key); });
    }
    return this.images.get(key);
  }
  async render({center, route = [], heading = 0, zoom = 16, destination = null}) {
    const ctx = this.ctx, W = MAP_SIZE;
    const at = worldPixel(center[0], center[1], zoom);
    const left = at.x-W/2, top = at.y-W/2;
    ctx.fillStyle = '#e2e6e1'; ctx.fillRect(0,0,W,W);
    const pending = [];
    for (let y = Math.floor(top/256); y <= Math.floor((top+W)/256); y++) {
      for (let x = Math.floor(left/256); x <= Math.floor((left+W)/256); x++) {
        pending.push(this.tile(zoom,x,y).then(img => ({img,x,y})));
      }
    }
    const tiles = await Promise.all(pending);
    for (const {img,x,y} of tiles) {
      if (img) ctx.drawImage(img, Math.round(x*256-left), Math.round(y*256-top), 256,256);
    }
    if (route.length >= 2) {
      ctx.lineCap='round'; ctx.lineJoin='round';
      for (const [width,color] of [[10,'#ffffff'],[6,'#00a7bc']]) {
        ctx.beginPath();
        route.forEach((p,i) => {
          const xy = worldPixel(p[0],p[1],zoom), x=xy.x-left, y=xy.y-top;
          i ? ctx.lineTo(x,y) : ctx.moveTo(x,y);
        });
        ctx.strokeStyle=color; ctx.lineWidth=width; ctx.stroke();
      }
    }
    if (destination) {
      const p=worldPixel(destination[0],destination[1],zoom);
      const x=p.x-left, y=p.y-top;
      ctx.fillStyle='#e5426e'; ctx.beginPath(); ctx.arc(x,y,5.5,0,Math.PI*2); ctx.fill();
      ctx.lineWidth=2;ctx.strokeStyle='white';ctx.stroke();
    }
    // Location marker: triangular orientation (heading 0° = north).
    ctx.save(); ctx.translate(W/2,W/2); ctx.rotate((heading||0)*Math.PI/180);
    ctx.fillStyle='#155ee9'; ctx.strokeStyle='white';ctx.lineWidth=3;
    ctx.beginPath();ctx.moveTo(0,-14);ctx.lineTo(10,12);ctx.lineTo(0,7);ctx.lineTo(-10,12);ctx.closePath();
    ctx.fill();ctx.stroke();ctx.restore();
    // Device also has separate persistent attribution at the bottom of the AMOLED.
    ctx.font='11px system-ui';ctx.textAlign='right';ctx.fillStyle='rgba(0,0,0,.77)';
    ctx.fillText('© OpenStreetMap contributors',W-6,W-7);
    return this.canvas;
  }
  async jpeg(quality=0.4) {
    const blob = await new Promise((res,rej) => this.canvas.toBlob(x=>x?res(x):rej(new Error('JPEG encoding failed')),'image/jpeg',quality));
    return new Uint8Array(await blob.arrayBuffer());
  }
}
