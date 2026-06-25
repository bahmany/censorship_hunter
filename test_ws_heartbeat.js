const WebSocket = require('ws');
const ws = new WebSocket('ws://hunter-backend-dev:7802');
ws.on('open', () => {
  console.log('CONNECTED');
  setTimeout(() => {
    ws.close();
    process.exit(0);
  }, 3000);
});
ws.on('message', (d) => {
  const msg = d.toString().substring(0, 200);
  console.log('MSG:', msg);
});
ws.on('ping', () => {
  console.log('PING received');
});
ws.on('pong', () => {
  console.log('PONG received');
});
ws.on('error', (e) => {
  console.log('ERROR:', e.message);
  process.exit(1);
});
