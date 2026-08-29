import ws from 'k6/ws'; // WebSocket
import { check } from 'k6';
export const options = {
  stages: [{ duration: '30s', target: 1000 }, { duration: '1m', target: 10000 }, { duration: '30s', target: 0 }],
  thresholds: { ws_session_duration: ['p(99)<5000'] },
};
export default function () {
  const url = __ENV.WS_URL || 'ws://localhost:8081/ws/1';
  const res = ws.connect(url, null, function (socket) {
    socket.on('open', function () { socket.send('["ping",null]'); });
    socket.on('message', function (data) { check(data, { 'pong': (d) => d.includes('pong') }); socket.close(); });
    socket.setTimeout(function(){ socket.close(); }, 5000);
  });
  check(res, { 'connected': (r) => r && r.status === 101 });
}
