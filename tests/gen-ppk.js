// 生成 PuTTY PPK v3 格式的 ed25519 私钥（供 -i 公钥认证测试）
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const dir = __dirname;
const kp = crypto.generateKeyPairSync('ed25519');
const jwk = kp.privateKey.export({ format: 'jwk' });

function b64urlToBuf(s) {
  return Buffer.from(s.replace(/-/g, '+').replace(/_/g, '/'), 'base64');
}
function sshString(b) {
  const buf = Buffer.isBuffer(b) ? b : Buffer.from(b);
  const len = Buffer.alloc(4);
  len.writeUInt32BE(buf.length);
  return Buffer.concat([len, buf]);
}

const seed = b64urlToBuf(jwk.d); // 32 bytes
const pub = b64urlToBuf(jwk.x);  // 32 bytes
const priv = Buffer.concat([seed, pub]); // PuTTY ed25519 private blob = seed || pub

const publicBlob = Buffer.concat([sshString('ssh-ed25519'), sshString(pub)]);
const privateBlob = sshString(priv);

const comment = 'test-key';
const macData = Buffer.concat([
  sshString('ssh-ed25519'),
  sshString('none'),
  sshString(comment),
  sshString(publicBlob),
  sshString(privateBlob),
]);
const mac = crypto.createHmac('sha256', '').update(macData).digest('hex');

function wrap(b64) {
  const lines = [];
  for (let i = 0; i < b64.length; i += 64) lines.push(b64.slice(i, i + 64));
  return lines.join('\n');
}

const pubB64 = publicBlob.toString('base64');
const privB64 = privateBlob.toString('base64');
const ppk = [
  'PuTTY-User-Key-File-3: ssh-ed25519',
  'Encryption: none',
  'Comment: ' + comment,
  'Public-Lines: ' + Math.ceil(pubB64.length / 64),
  wrap(pubB64),
  'Private-Lines: ' + Math.ceil(privB64.length / 64),
  wrap(privB64),
  'Private-MAC: ' + mac,
  '',
].join('\n');

fs.writeFileSync(path.join(dir, 'id_ed25519.ppk'), ppk);
console.log('PPK written:', path.join(dir, 'id_ed25519.ppk'));
console.log('Public key (OpenSSH): ssh-ed25519 ' + publicBlob.toString('base64') + ' ' + comment);
