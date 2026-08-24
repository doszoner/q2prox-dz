import test from 'node:test';
import assert from 'node:assert/strict';
import { ManagedReadinessProbe, quakeOob, quakeOobCommand } from './managed-readiness-probe.mjs';

test('encodes and parses Quake II OOB ping/ack', () => {
  assert.equal(quakeOobCommand(quakeOob('ping')), 'ping');
  assert.equal(quakeOobCommand(quakeOob('ack')), 'ack');
  assert.equal(quakeOobCommand(new Uint8Array([1,2,3])), null);
});

test('declares ready only after ack and consumes only an awaited ack', () => {
  const timers=[]; const sent=[]; let ready=0;
  const p=new ManagedReadinessProbe({send:x=>sent.push(x),onReady:()=>ready++,onRoomClosed:()=>{},setTimer:f=>(timers.push(f),timers.length-1),clearTimer:()=>{}});
  assert.equal(p.packetReceived(quakeOob('ack')), false);
  p.channelOpened(); assert.equal(sent.length,1); assert.equal(ready,0);
  assert.equal(p.packetReceived(quakeOob('ack')), true); assert.equal(ready,1);
});

test('retries until the DataChannel accepts the first probe', () => {
  const timers=[]; let attempts=0; let ready=0;
  const p=new ManagedReadinessProbe({send:()=>++attempts>=3,onReady:()=>ready++,onRoomClosed:()=>{},setTimer:f=>(timers.push(f),timers.length-1),clearTimer:()=>{}});
  p.channelOpened();
  assert.equal(p.packetReceived(quakeOob('ack')), false);
  timers.shift()();
  assert.equal(p.packetReceived(quakeOob('ack')), false);
  timers.shift()();
  assert.equal(p.packetReceived(quakeOob('ack')), true);
  assert.equal(ready,1);
});

test('closes once after readiness when every later probe send returns false', () => {
  const timers=[]; let sends=0; let closed=0;
  const p=new ManagedReadinessProbe({send:()=>++sends===1,onReady:()=>{},onRoomClosed:()=>closed++,setTimer:f=>(timers.push(f),timers.length-1),clearTimer:()=>{},maxMisses:3});
  p.channelOpened();
  assert.equal(p.packetReceived(quakeOob('ack')), true);
  timers.shift()();
  timers.shift()();
  timers.shift()();
  timers.shift()();
  assert.equal(closed,1);
});

test('closes once after readiness when every later probe send throws', () => {
  const timers=[]; let sends=0; let closed=0;
  const p=new ManagedReadinessProbe({send:()=>{ if (++sends > 1) throw new Error('closed'); return true; },onReady:()=>{},onRoomClosed:()=>closed++,setTimer:f=>(timers.push(f),timers.length-1),clearTimer:()=>{},maxMisses:3});
  p.channelOpened();
  assert.equal(p.packetReceived(quakeOob('ack')), true);
  timers.shift()();
  timers.shift()();
  timers.shift()();
  timers.shift()();
  assert.equal(closed,1);
});

test('resets post-ready misses only after a successful probe ack', () => {
  const timers=[]; let sends=0; let closed=0;
  const setTimer=f=>{ const timer={f,active:true}; timers.push(timer); return timer; };
  const runTimer=()=>{ const timer=timers.find(x=>x.active); assert.ok(timer); timer.active=false; timer.f(); };
  const p=new ManagedReadinessProbe({send:()=>++sends===1 || sends===3 || sends===4,onReady:()=>{},onRoomClosed:()=>closed++,setTimer,clearTimer:timer=>{timer.active=false;},maxMisses:3});
  p.channelOpened();
  assert.equal(p.packetReceived(quakeOob('ack')), true);
  runTimer();
  assert.equal(p.misses,1);
  runTimer();
  runTimer();
  assert.equal(p.misses,2);
  assert.equal(closed,0);
  assert.equal(p.packetReceived(quakeOob('ack')), true);
  assert.equal(p.misses,0);
  runTimer();
  assert.equal(closed,0);
  runTimer();
  assert.equal(closed,0);
  runTimer();
  assert.equal(closed,1);
});

test('treats peer-validated gameplay as post-ready liveness', () => {
  const timers=[]; let closed=0;
  const setTimer=f=>{ const timer={f,active:true}; timers.push(timer); return timer; };
  const clearTimer=timer=>{ timer.active=false; };
  const nextTimer=()=>{
    const timer=timers.find(x=>x.active);
    assert.ok(timer);
    timer.active=false;
    timer.f();
  };
  const gameplay=new Uint8Array([0x01,0x02,0x03,0x04]);
  const p=new ManagedReadinessProbe({send:()=>true,onReady:()=>{},onRoomClosed:()=>closed++,setTimer,clearTimer,maxMisses:3});

  p.channelOpened();
  assert.equal(p.packetReceived(quakeOob('ack')), true);
  for (let i=0; i<4; i++) {
    nextTimer();
    assert.equal(p.pending, true);
    assert.equal(p.packetReceived(gameplay), false);
    assert.equal(p.pending, false);
    assert.equal(p.misses, 0);
  }
  assert.equal(closed, 0);
});

test('closes after bounded explicit probe failures, not gameplay idleness', () => {
  const timers=[]; let closed='';
  const p=new ManagedReadinessProbe({send:()=>{},onReady:()=>{},onRoomClosed:r=>closed=r,setTimer:f=>(timers.push(f),timers.length-1),clearTimer:()=>{},maxMisses:3});
  p.channelOpened(); timers.shift()(); timers.shift()(); timers.shift()();
  assert.equal(closed,'probe_timeout');
});
