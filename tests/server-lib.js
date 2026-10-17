// 本地临时 SSH 测试服务器（同进程启动，仅监听 127.0.0.1）
const { Server, utils } = require('ssh2');
const crypto = require('crypto');
const fs = require('fs');
const net = require('net');
const http = require('http');
const path = require('path');

function startServer(dir) {
  return new Promise((resolve) => {
    const keyFile = path.join(dir, 'host_ed25519.pem');
    const pubFile = path.join(dir, 'host_ed25519.pub');
    let pem, publicKey;
    if (fs.existsSync(keyFile) && fs.existsSync(pubFile)) {
      pem = fs.readFileSync(keyFile);
      publicKey = fs.readFileSync(pubFile, 'utf8').trim();
    } else {
      const kp = utils.generateKeyPairSync('ed25519');
      pem = kp.private;
      publicKey = kp.public;
      fs.writeFileSync(keyFile, pem);
      fs.writeFileSync(pubFile, publicKey + '\n');
    }

    const blob = Buffer.from(publicKey.split(' ')[1], 'base64');
    const fingerprint = 'SHA256:' + crypto.createHash('sha256').update(blob).digest('base64').replace(/=+$/, '');

    const server = new Server({ hostKeys: [pem] }, (client) => {
      client.on('error', () => {});
      client.on('authentication', (ctx) => {
        if (ctx.method === 'password' && ctx.username === 'testuser' && ctx.password === 'testpass') {
          return ctx.accept();
        }
        if (ctx.method === 'publickey') {
          return ctx.accept();
        }
        ctx.reject(['password', 'publickey']);
      });

      client.on('ready', () => {
        client.on('session', (accept) => {
          const session = accept();
          session.on('pty', (accept) => accept && accept());
          session.on('exec', (accept, reject, info) => {
            const stream = accept();
            stream.write('HELLO_FROM_PUTTY\r\n');
            stream.write('中文显示测试：你好，世界\r\n');
            stream.write('COMMAND=' + info.command + '\r\n');
            stream.exit(0);
            stream.end();
          });
          session.on('shell', (accept) => {
            const stream = accept();
            stream.write('WELCOME_SHELL\r\n$ ');
            stream.on('data', (d) => {
              const s = d.toString();
              if (s.includes('exit')) {
                stream.write('BYE\r\n');
                stream.exit(0);
                stream.end();
              } else {
                stream.write('GOT:' + s.trim() + '\r\n$ ');
              }
            });
          });
        });

        client.on('tcpip', (accept) => {
          const stream = accept();
          const sock = net.connect(18081, '127.0.0.1');
          sock.on('error', () => stream.end());
          stream.pipe(sock).pipe(stream);
        });
      });
    });

    server.listen(2222, '127.0.0.1', () => {
      const httpServer = http.createServer((req, res) => {
        res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('HTTP_ECHO_OK');
      }).listen(18081, '127.0.0.1');
      resolve({ server, httpServer, fingerprint });
    });
  });
}

module.exports = { startServer };
