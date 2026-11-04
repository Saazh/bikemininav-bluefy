import test from 'node:test';
import assert from 'node:assert/strict';
import {getManeuvers,turnSymbol,formatDistance,formatDuration} from '../navigation-ui.mjs';
import {measureRoute} from '../geometry.mjs';
test('turn labels and distances are stable',()=>{
  assert.equal(turnSymbol(1),'↱');
  assert.equal(turnSymbol(123),'↑');
  assert.equal(formatDistance(150),'150 m');
  assert.equal(formatDistance(2345),'2.3 km');
  assert.equal(formatDuration(3900),'1 h 5 min');
});
test('route step browser maps ORS vertex offsets to real positions',()=>{
  const coords=[[53,8],[53.001,8],[53.002,8.001],[53.003,8.002]];
  const distances=measureRoute(coords);
  const steps=[
    {type:11,way_points:[0,1],instruction:'Start'},
    {type:6,way_points:[0,1],instruction:'Continue'},
    {type:1,way_points:[1,2],instruction:'Turn right'},
    {type:0,way_points:[2,3],instruction:'Turn left'},
    {type:10,way_points:[3,3],instruction:'Arrive'}
  ];
  const turns=getManeuvers(steps,coords,distances);
  assert.equal(turns.length,3);
  assert.deepEqual(turns[0].latlon,[53.001,8]);
  assert.equal(turns[0].icon,'↱');
  assert.equal(turns[1].distanceFromStart,distances[2]);
  assert.equal(turns[2].type,10);
});
test('empty or malformed route never builds map markers',()=>{
  assert.deepEqual(getManeuvers(null,[],[]),[]);
  assert.deepEqual(getManeuvers([{type:1,way_points:[0,0]}],[[NaN,8]],[0]),[]);
});
