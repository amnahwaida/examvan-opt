import ws from 'k6/ws';
import { check } from 'k6';

// F0 steady-state: bukti p99 ws_connecting <500ms pada beban BERKELANJUTAN.
// Berbeda dengan k6_ws.js (reconnect-storm 10k), di sini VU memegang koneksi
// terbuka selama durasi tes dan hanya ping/pong — meniru perilaku nyata
// siswa/pengawas yang tetap terhubung selama ujian berjalan.
export const options = {
  scenarios: {
    steady: {
      executor: 'ramping-vus',
      startVUs: 0,
      stages: [
        { duration: '20s', target: Number(__ENV.TARGET_VUS || 3000) },
        { duration: '60s', target: Number(__ENV.TARGET_VUS || 3000) },
        { duration: '10s', target: 0 },
      ],
      gracefulRampDown: '5s',
    },
  },
  thresholds: {
    ws_connecting: ['p(99)<500'],
    ws_session_duration: ['p(99)<90000'],
  },
};

export default function () {
  const url = __ENV.WS_URL || 'ws://webui-cpp:5000/ws/1';
  const holdMs = Number(__ENV.HOLD_MS || 60000);
  ws.connect(url, {}, function (socket) {
    let ponged = false;
    socket.on('open', () => {
      socket.send('["ping",null]');
      socket.setInterval(() => socket.send('["ping",null]'), 5000);
      socket.setTimeout(() => socket.close(), holdMs);
    });
    socket.on('message', (data) => {
      if (data.includes('pong')) ponged = true;
      check(data, { 'pong': (d) => d.includes('pong') });
    });
    socket.on('close', () => {});
  });
}
