import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import readline from 'node:readline';
import { setTimeout as wait } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

test('a guest follows the host, cannot edit the room, and observes its closure', { timeout: 10_000 }, async t => {
  const server = spawn(process.execPath, [fileURLToPath(new URL('./server.mjs', import.meta.url))], {
    env: { ...process.env, LITEMUSIC_ROOMS_BIND: '127.0.0.1', LITEMUSIC_ROOMS_PORT: '0' },
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  t.after(() => server.kill());
  const lines = readline.createInterface({ input: server.stdout });
  const [line] = await once(lines, 'line');
  const port = line.match(/:(\d+)$/)?.[1];
  assert.ok(port);
  async function post(path, body = {}) {
    const response = await fetch(`http://127.0.0.1:${port}/v1/rooms${path}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    return { status: response.status, data: await response.json() };
  }
  const room = await post('');
  assert.equal(room.status, 201);
  const { code, host_key: key } = room.data;
  const route = '/' + code;
  const track = { id: 'mWB22', title: 'Ambient Snow', artist: 'Blop', position_ms: 12_000, playing: true };
  const initial = await post(route + '/read');
  assert.equal(initial.data.state, null);
  assert.equal(Object.hasOwn(initial.data, 'host_key'), false);
  assert.equal((await post(route + '/update', { state: track })).status, 403);
  assert.equal((await post(route + '/update', { host_key: key, state: track })).status, 200);
  await wait(70);
  const playing = await post(route + '/read');
  assert.equal(playing.data.state.id, track.id);
  assert.equal(playing.data.state.playing, true);
  assert.ok(playing.data.state.position_ms > track.position_ms);
  assert.equal((await post(route + '/update', {
    host_key: key, state: { ...track, position_ms: 25_000, playing: false },
  })).status, 200);
  await wait(70);
  const paused = await post(route + '/read');
  assert.equal(paused.data.revision, playing.data.revision + 1);
  assert.equal(paused.data.state.position_ms, 25_000);
  assert.equal(paused.data.state.playing, false);
  assert.equal((await post(route + '/close', { host_key: key })).status, 200);
  assert.equal((await post(route + '/read')).status, 404);
});
