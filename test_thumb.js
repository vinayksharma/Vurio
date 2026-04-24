const http = require('http');
const fs = require('fs');

const options = {
  hostname: '127.0.0.1',
  port: 8080,
  path: '/api/v1/thumbnails?path=%2FUsers%2Fvinaysharma%2Fdeveloper%2Fvurio%2Ftest_images%2Fportrait.jpg&size=128',
  method: 'GET'
};

const req = http.request(options, res => {
  console.log(`STATUS: ${res.statusCode}`);
  const dest = fs.createWriteStream('./test_thumb.jpg');
  res.pipe(dest);
});

req.on('error', error => {
  console.error(error);
});

req.end();
