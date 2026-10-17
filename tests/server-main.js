// 启动测试 SSH 服务器并写出主机指纹，供 PowerShell 测试脚本读取
const fs = require('fs');
const path = require('path');
const { startServer } = require('./server-lib');

(async () => {
  const { fingerprint } = await startServer(__dirname);
  fs.writeFileSync(path.join(__dirname, 'fingerprint.txt'), fingerprint);
  console.log('LISTENING 127.0.0.1:2222');
  console.log('FINGERPRINT=' + fingerprint);
})();
