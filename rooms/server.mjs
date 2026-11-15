import { createServer } from 'node:http';
import { randomBytes, timingSafeEqual } from 'node:crypto';

const rooms = new Map();
const codePattern = /^[0-9A-F]{12}$/;
const trackPattern = /^[a-zA-Z0-9]{1,32}$/;
const idleMs = 90_000;
const maxPosition = 86_400_000;

function send(response, status, value) {
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
  });
  response.end(JSON.stringify(value));
}

async function readBody(request) {
  const chunks = [];
  let length = 0;
  for await (const chunk of request) {
    length += chunk.length;
    if (length > 4096) throw new Error('Body too large');
    chunks.push(chunk);
  }
  const value = JSON.parse(Buffer.concat(chunks).toString());
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid body');
  return value;
}

function prune(now) {
  for (const [code, room] of rooms) {
    if (room.expiresAt <= now) rooms.delete(code);
  }
}

function authorize(room, key) {
  return typeof key === 'string' && /^[a-f0-9]{48}$/.test(key) &&
    timingSafeEqual(Buffer.from(key, 'hex'), room.key);
}

function decodeState(state) {
  if (state === null) return null;
  const { id, title, artist, position_ms, playing } = state ?? {};
  if (typeof id !== 'string' || !trackPattern.test(id) ||
      typeof title !== 'string' || title.length < 1 || title.length > 180 ||
      typeof artist !== 'string' || artist.length > 180 ||
      !Number.isInteger(position_ms) || position_ms < 0 || position_ms > maxPosition ||
      typeof playing !== 'boolean') throw new Error('Invalid playback state');
  return { id, title, artist, position_ms, playing };
}

function snapshot(room, now) {
  const state = room.state;
  return {
    revision: room.revision,
    state: state && {
      ...state,
      position_ms: Math.min(maxPosition, state.position_ms + (state.playing ? Math.max(0, now - room.updatedAt) : 0)),
    },
  };
}

const server = createServer({ requestTimeout: 10_000, headersTimeout: 5000 }, async (request, response) => {
  const now = Date.now();
  prune(now);
  if (request.method === 'GET' && request.url === '/health') return send(response, 200, { ok: true });
  if (request.method !== 'POST') return send(response, 404, { error: 'not_found' });
  try {
    const body = await readBody(request);
    if (request.url === '/v1/rooms') {
      if (rooms.size >= 500) return send(response, 503, { error: 'capacity' });
      let code;
      do {
        code = randomBytes(6).toString('hex').toUpperCase();
      } while (rooms.has(code));
      const key = randomBytes(24);
      const room = { key, state: null, updatedAt: now, expiresAt: now + idleMs, revision: 0 };
      rooms.set(code, room);
      return send(response, 201, { code, host_key: key.toString('hex') });
    }
    const [, , resource, code, action, extra] = request.url.split('/');
    if (resource !== 'rooms' || !request.url.startsWith('/v1/') || !codePattern.test(code ?? '') || extra !== undefined) {
      return send(response, 404, { error: 'not_found' });
    }
    const room = rooms.get(code);
    if (!room || room.expiresAt <= now) return send(response, 404, { error: 'room_closed' });
    if (action === 'read') return send(response, 200, snapshot(room, now));
    if (!authorize(room, body.host_key)) return send(response, 403, { error: 'host_only' });
    if (action === 'close') {
      rooms.delete(code);
      return send(response, 200, { ok: true });
    }
    if (action === 'update') {
      if (Object.hasOwn(body, 'state')) {
        room.state = decodeState(body.state);
        room.updatedAt = now;
        room.revision++;
      }
      room.expiresAt = now + idleMs;
      return send(response, 200, { ok: true });
    }
    return send(response, 404, { error: 'not_found' });
  } catch {
    if (!response.headersSent) send(response, 400, { error: 'bad_request' });
  }
});

// Bind to loopback by default. Use a reverse proxy with HTTPS for public access.
const port = Number(process.env.LITEMUSIC_ROOMS_PORT ?? 49370);
const host = process.env.LITEMUSIC_ROOMS_BIND ?? '127.0.0.1';
server.listen(port, host, () => console.log(`LiteMusic rooms listening on ${host}:${server.address().port}`));
