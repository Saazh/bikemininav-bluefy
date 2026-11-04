import { SERVICE_UUID, WRITE_UUID, MAP_SIZE, makeNavPacket, imagePackets, TURN_LABELS } from './protocol.mjs';
import { distanceMeters, measureRoute, closestOnRoute, turnAhead } from './geometry.mjs';
import { MiniMapRenderer } from './map-render.mjs';

const $ = id => document.getElementById(id);
let map, gpsLayer, destinationLayer, routeLayer, device, characteristic;
let position = null, destination = null, route = null;
let watchId = null, lastNav = null, transferBusy = false, lastImageAt = 0;
let lastImagePosition = null, lastNavAt = 0, routeVersion = 0, sentVersion = -1;
let newestStatus = '';
// HeiGIT replaced the old api.openrouteservice.org host in 2026.
// Both geocoding and directions now use different versioned service prefixes.
const ORS_GEOCODE_ENDPOINT = 'https://api.heigit.org/pelias/v1/search';
const ORS_CYCLING_ENDPOINT = 'https://api.heigit.org/openrouteservice/v2/directions/cycling-regular/geojson';
const renderer = new MiniMapRenderer($('mini'));

function log(msg) {
  const stamp = new Date().toLocaleTimeString();
  $('log').textContent = `[${stamp}] ${msg}\n` + $('log').textContent.slice(0, 1800);
}
function status(message, isError=false) {
  newestStatus = message;
  $('status').textContent = message;
  $('status').style.color = isError ? '#f4a3a6' : '#e8f4fa';
  log(message);
}
function fmtMeters(m) { return m < 1000 ? `${Math.round(m)} m` : `${(m/1000).toFixed(1)} km`; }
function key() { return $('key').value.trim(); }
function requireKey() {
  if (!key()) throw new Error('Enter your openrouteservice / HeiGIT API key above. Both place search and cycling routes require it.');
  return key();
}
function ll(geoCoord) { return [geoCoord.latitude, geoCoord.longitude]; }
function currentCenter() { return position?.latlon || map.getCenter() && [map.getCenter().lat,map.getCenter().lng] || [53.0788,8.8024]; }

function init() {
  if (!window.L) { status('Leaflet did not load. Check your internet connection and reload.',true); return; }
  map = L.map('map',{zoomControl:true}).setView([53.0788,8.8024],15);
  L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom:19, attribution:'© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap contributors</a>',
    crossOrigin:'anonymous'
  }).addTo(map);
  map.on('click',event => chooseDestination([event.latlng.lat,event.latlng.lng],'Map pin'));
  if(localStorage.getItem('bikemininav-ors-key')) {
    $('key').value = localStorage.getItem('bikemininav-ors-key'); $('remember-key').checked = true;
  }
  $('key').addEventListener('change', () => {
    if($('remember-key').checked) localStorage.setItem('bikemininav-ors-key',key());
  });
  $('remember-key').addEventListener('change', () => {
    if($('remember-key').checked) localStorage.setItem('bikemininav-ors-key',key());
    else localStorage.removeItem('bikemininav-ors-key');
  });
  $('connect').onclick = () => guard(connectBLE);
  $('test').onclick = () => guard(sendTest);
  $('gps').onclick = () => guard(startGPS);
  $('search').onclick = () => guard(searchDestination);
  $('destination').addEventListener('keydown', e => { if (e.key === 'Enter') guard(searchDestination); });
  $('results').addEventListener('change', () => {
    const value=$('results').value;
    if (value) {
      const [lat,lon]=value.split(',').map(Number);
      chooseDestination([lat,lon], $('results').selectedOptions[0].textContent);
    }
  });
  $('route').onclick = () => guard(planRoute);
  $('stop').onclick = () => { route=null;routeVersion++;if(routeLayer)routeLayer.remove();routeLayer=null;refreshStats();status('Route stopped.'); };
  $('chunks').addEventListener('change', () => log(`BLE packet size set to ${$('chunks').value} bytes`));
  setInterval(() => guard(syncNavigation), 3000);
  $('mini').getContext('2d').fillStyle='#1c3543';
  $('mini').getContext('2d').fillRect(0,0,MAP_SIZE,MAP_SIZE);
  refreshPreview().catch(e=>status(e.message,true));
}
async function guard(fn) { try { await fn(); } catch (error) { status(error.message || String(error),true); } }

async function connectBLE() {
  if (!navigator.bluetooth) throw new Error('Web Bluetooth unavailable. Open your HTTPS GitHub Pages URL in Bluefy, not Safari.');
  status('Select the BikeMiniNav ESP32 in the Bluetooth popup…');
  device = await navigator.bluetooth.requestDevice({filters:[{namePrefix:'BikeMiniNav'}],optionalServices:[SERVICE_UUID]});
  device.addEventListener('gattserverdisconnected',()=>{
    characteristic=null;$('ble-state').textContent='Disconnected';
    log('BLE disconnected; tap Connect to reconnect');
  });
  const gatt = await device.gatt.connect();
  characteristic = await (await gatt.getPrimaryService(SERVICE_UUID)).getCharacteristic(WRITE_UUID);
  $('ble-state').textContent='Connected';
  status(`Connected to ${device.name || 'BikeMiniNav'}. Try Send test mini-map.`);
}
async function writePacket(bytes) {
  if (!characteristic) throw new Error('Connect the Waveshare over BLE first.');
  if(characteristic.writeValueWithResponse) await characteristic.writeValueWithResponse(bytes);
  else await characteristic.writeValue(bytes); // Older Web Bluetooth implementations.
}

async function sendJpeg(jpeg) {
  let size = Number($('chunks').value);
  const sizes = [180,120,64,20].filter(n=>n <= size);
  for (let attempt=0; attempt<sizes.length;attempt++) {
    size=sizes[attempt];
    const frames=imagePackets(jpeg,size);
    try {
      $('transfer-status').textContent=`${Math.round(jpeg.length/1024)} KB`;
      for(let i=0;i<frames.length;i++) {
        await writePacket(frames[i]);
        if(i % 30 === 0 || i===frames.length-1) {
          $('transfer-status').textContent=`${Math.round(100*i/(frames.length-1))}%`;
        }
      }
      $('chunks').value=String(size);
      $('transfer-status').textContent='Delivered';
      log(`JPEG ${jpeg.length} bytes sent as ${frames.length} BLE writes (${size}-byte packets)`);
      return;
    } catch(err) {
      log(`${size}-byte BLE packet attempt failed: ${err.message}`);
      if(!characteristic || !device?.gatt?.connected) throw new Error('BLE disconnected during transfer. Reconnect and retry.');
      if(attempt===sizes.length-1) throw err;
      // Next START packet resets the unfinished transfer on the ESP32.
    }
  }
}

function getNav() {
  if(!position) return {turnType:6,turnMeters:0,remainingMeters:0,speedKmh:0,heading:0};
  const base={speedKmh:position.speedKmh||0,heading:position.heading||0};
  if(!route) return {turnType:6,turnMeters:0,remainingMeters:0,...base};
  const near=closestOnRoute(position.latlon,route.coords,route.cumulative);
  const turn=turnAhead(route.steps,route.cumulative,near.progress);
  return {...base,...turn,remainingMeters:Math.max(0,(route.cumulative.at(-1)||0)-near.progress),crossTrack:near.crossTrack};
}
function refreshStats() {
  const s=getNav(); lastNav=s;
  $('turn').textContent=route ? (TURN_LABELS[s.turnType]||'Follow route') : '—';
  $('turn-distance').textContent=route ? fmtMeters(s.turnMeters):'—';
  $('remain').textContent=route ? fmtMeters(s.remainingMeters):'—';
  $('speed').textContent=position ? `${s.speedKmh.toFixed(1)} km/h` : '—';
  if (route && s.crossTrack > 60) {
    $('gps-state').textContent='Off-route?';
  } else if(position) $('gps-state').textContent='GPS active';
}
async function sendTest() {
  if(transferBusy) { status('Another map transfer is in progress.');return; }
  if(!characteristic) throw new Error('Connect to BikeMiniNav first.');
  transferBusy=true;
  try {
    const center=currentCenter();
    const demoRoute = [[center[0]-.0012,center[1]-.0015],[center[0]-.00035,center[1]-.0003],
      [center[0]+.0002,center[1]+.0002],[center[0]+.0012,center[1]+.0002]];
    await writePacket(makeNavPacket({turnType:1,turnMeters:120,remainingMeters:3200,speedKmh:18.2,heading:10}));
    await renderer.render({center,route:demoRoute,heading:10,destination:demoRoute.at(-1)});
    const jpg=await renderer.jpeg(.36);
    await sendJpeg(jpg);
    status('Test map sent. The Waveshare should show local streets and “Right 120 m”. Test data is simulated.');
  } finally {transferBusy=false;}
}

function chooseDestination(latlon, title='Destination') {
  destination={latlon,title};
  if(destinationLayer) destinationLayer.remove();
  destinationLayer=L.circleMarker(latlon,{radius:9,color:'#fff',weight:2,fillColor:'#e5426e',fillOpacity:1}).addTo(map);
  map.panTo(latlon);
  status(`Destination selected: ${title}. Tap Plan bicycle route.`);
}
async function searchDestination() {
  const term=$('destination').value.trim();
  if(!term) throw new Error('Enter a destination or tap the map.');
  // Useful for directly pasting "latitude, longitude" without geocoding.
  const parsed=term.match(/^\s*(-?\d+(?:\.\d+)?)\s*[,;]\s*(-?\d+(?:\.\d+)?)\s*$/);
  if(parsed) {
    const lat=+parsed[1],lon=+parsed[2];
    if(Math.abs(lat)>90||Math.abs(lon)>180) throw new Error('Invalid latitude or longitude.');
    chooseDestination([lat,lon],term); return;
  }
  const api=requireKey();
  const url=new URL(ORS_GEOCODE_ENDPOINT);
  url.searchParams.set('api_key',api);url.searchParams.set('text',term);url.searchParams.set('size','5');
  status(`Searching for “${term}”…`);
  let resp;
  try {
    resp = await fetch(url.toString());
  } catch (err) {
    throw new Error(`Place search could not reach HeiGIT (${err.message}). Check internet access in Bluefy.`);
  }
  if (!resp.ok) {
    const detail = (await resp.text()).slice(0, 180);
    const hint = resp.status === 401 || resp.status === 403
      ? 'Check your HeiGIT API key and enabled services.'
      : resp.status === 429 ? 'API rate limit reached; retry later.'
      : 'Check the service availability and connection.';
    throw new Error(`Place search HTTP ${resp.status}: ${hint} ${detail}`);
  }
  const json=await resp.json();
  const features=json.features || [];
  if(!features.length) throw new Error('No matching destinations. Try a more specific place or tap the map.');
  $('results').replaceChildren(new Option('Select a result',''));
  for(const f of features) {
    const [lon,lat]=f.geometry.coordinates;
    $('results').add(new Option(f.properties?.label || f.properties?.name || `${lat}, ${lon}`, `${lat},${lon}`));
  }
  // Convenient default is first result, but all alternatives remain selectable.
  $('results').selectedIndex=1;
  const chosen=features[0];
  const [lon,lat]=chosen.geometry.coordinates;
  chooseDestination([lat,lon],chosen.properties?.label || term);
  status(`Found ${features.length} match(es); change the search result if needed, then plan route.`);
}
async function planRoute() {
  if(!position) throw new Error('Enable GPS and wait for a fix first.');
  if(!destination) throw new Error('Choose a destination by searching or tapping the map.');
  const api=requireKey();
  const coords=[[position.latlon[1],position.latlon[0]], [destination.latlon[1],destination.latlon[0]]];
  status('Requesting cycling-regular directions from openrouteservice…');
  let resp;
  try {
    resp = await fetch(ORS_CYCLING_ENDPOINT,{
    method:'POST',headers:{'Authorization':api,'Content-Type':'application/json'},
    body:JSON.stringify({coordinates:coords,instructions:true,language:'en'})
    });
  } catch (err) {
    throw new Error(`Cycling route could not reach HeiGIT (${err.message}). Check internet access in Bluefy.`);
  }
  if(!resp.ok) {
    const body=(await resp.text()).slice(0,250);
    throw new Error(`Routing HTTP ${resp.status}: ${body || 'Check your HeiGIT key, service permissions and quota'}`);
  }
  const data=await resp.json();
  const feat=data.features?.[0];
  if(!feat?.geometry?.coordinates?.length) throw new Error('Routing response contains no route geometry.');
  const path=feat.geometry.coordinates.map(c=>[c[1],c[0]]);
  const steps=(feat.properties?.segments || []).flatMap(s=>s.steps || []);
  route={coords:path,steps,cumulative:measureRoute(path)};
  routeVersion++; sentVersion=-1;lastImageAt=0;
  if(routeLayer)routeLayer.remove();
  routeLayer=L.polyline(path,{color:'#05b7ba',weight:6,opacity:.95}).addTo(map);
  map.fitBounds(routeLayer.getBounds(),{padding:[24,24]});
  refreshStats();
  status(`Cycling route ready (${fmtMeters(feat.properties.summary?.distance || route.cumulative.at(-1))}). Live updates start while Bluefy stays foreground.`);
  await syncNavigation();
}
function startGPS() {
  if(!navigator.geolocation) throw new Error('Geolocation unavailable in this browser.');
  if(watchId!==null) return;
  $('gps-state').textContent='Searching…';
  watchId=navigator.geolocation.watchPosition(fix=>{
    const latlon=ll(fix.coords);
    let newSpeed=(fix.coords.speed!==null && fix.coords.speed>=0)?fix.coords.speed*3.6:0;
    if (position && !(fix.coords.speed>=0)) {
      const seconds=(fix.timestamp-position.timestamp)/1000;
      if(seconds>0.5) newSpeed=Math.min(70,3.6*distanceMeters(position.latlon,latlon)/seconds);
    }
    let heading=(fix.coords.heading!==null && Number.isFinite(fix.coords.heading)) ? fix.coords.heading : (position?.heading || 0);
    if(position && !newSpeed && distanceMeters(position.latlon,latlon)>5) {
      const dy=(latlon[0]-position.latlon[0])*Math.PI/180;
      const dx=(latlon[1]-position.latlon[1])*Math.PI/180*Math.cos(latlon[0]*Math.PI/180);
      heading=(Math.atan2(dx,dy)*180/Math.PI+360)%360;
    }
    position={latlon,speedKmh:newSpeed,heading,accuracy:fix.coords.accuracy, timestamp:fix.timestamp};
    if(!gpsLayer) {
      gpsLayer=L.circleMarker(latlon,{radius:8,color:'#fff',weight:3,fillColor:'#1a65ee',fillOpacity:1}).addTo(map);
      if(!route)map.setView(latlon,16);
    } else gpsLayer.setLatLng(latlon);
    refreshStats();
    if(!route && !transferBusy) guard(refreshPreview);
  },error=>{
    $('gps-state').textContent='GPS error';
    status(`GPS ${error.code}: ${error.message}. Allow location access in Bluefy and iOS Settings.`,true);
  },{enableHighAccuracy:true,maximumAge:1000,timeout:20000});
  status('GPS started. Allow location access in Bluefy.');
}
async function refreshPreview() {
  const center=currentCenter();
  await renderer.render({center,route:route?.coords || [],heading:position?.heading||0,destination:destination?.latlon || null});
}
async function syncNavigation() {
  if(!characteristic || !position || transferBusy) return;
  if(!device?.gatt?.connected) return;
  const now=Date.now();
  const s=getNav();
  if(now-lastNavAt >= 2500) {
    try { await writePacket(makeNavPacket(s));lastNavAt=now; }
    catch(err) { status(`Navigation packet failed: ${err.message}`,true);return; }
  }
  // Avoid re-sending the same map at rest; still send periodically while moving.
  const moved= !lastImagePosition || distanceMeters(position.latlon,lastImagePosition)>12;
  if((now-lastImageAt < 12000 || (!moved && sentVersion===routeVersion && now-lastImageAt<60000))) return;
  transferBusy=true;
  try {
    await refreshPreview();
    const jpeg=await renderer.jpeg(.37);
    await sendJpeg(jpeg);
    lastImageAt=Date.now();lastImagePosition=[...position.latlon];sentVersion=routeVersion;
    if(route && s.crossTrack>60) status('GPS is >60 m away from the route. Stop and recalculate if needed.');
  } catch(err) { status(`Map sync failed: ${err.message}`,true); }
  finally {transferBusy=false;}
}
if(document.readyState==='loading') document.addEventListener('DOMContentLoaded',init);
else init();
