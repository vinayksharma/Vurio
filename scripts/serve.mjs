#!/usr/bin/env node
/**
 * Serve script - uses Bun server if available, falls back to Node
 */

import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, resolve } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, '..');
const serverFile = resolve(projectRoot, 'server.ts');

/**
 * Check if bun is available
 */
async function hasBun() {
  return new Promise((resolve) => {
    const proc = spawn('bun', ['--version'], { stdio: 'pipe' });
    proc.on('close', (code) => resolve(code === 0));
    proc.on('error', () => resolve(false));
  });
}

async function main() {
  const bunAvailable = await hasBun();
  
  if (bunAvailable) {
    console.log('Starting server with Bun...');
    spawn('bun', ['run', serverFile], { 
      stdio: 'inherit', 
      cwd: projectRoot 
    });
  } else {
    // Fallback: use a simple Node.js static server
    console.log('Bun not available. Starting fallback Node.js server...');
    
    const { createServer } = await import('http');
    const { readFile, stat } = await import('fs/promises');
    const { extname, join } = await import('path');
    
    const DIST_ROOT = resolve(projectRoot, 'dist');
    const PORT = parseInt(process.env.UI_PORT || '1212', 10);
    const API_TARGET = process.env.API_BASE || 'http://localhost:8088';
    
    const MIME_TYPES = {
      '.html': 'text/html; charset=utf-8',
      '.js': 'application/javascript; charset=utf-8',
      '.css': 'text/css; charset=utf-8',
      '.json': 'application/json; charset=utf-8',
      '.png': 'image/png',
      '.jpg': 'image/jpeg',
      '.jpeg': 'image/jpeg',
      '.gif': 'image/gif',
      '.svg': 'image/svg+xml',
      '.ico': 'image/x-icon',
      '.webp': 'image/webp',
    };
    
    const server = createServer(async (req, res) => {
      const url = new URL(req.url, `http://localhost:${PORT}`);
      
      // CORS headers
      res.setHeader('Access-Control-Allow-Origin', '*');
      res.setHeader('Access-Control-Allow-Methods', 'GET,POST,PUT,PATCH,DELETE,OPTIONS');
      res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
      
      if (req.method === 'OPTIONS') {
        res.writeHead(204);
        res.end();
        return;
      }
      
      // API proxy
      if (url.pathname.startsWith('/api/')) {
        try {
          const targetUrl = new URL(url.pathname + url.search, API_TARGET);
          const proxyRes = await fetch(targetUrl.toString(), {
            method: req.method,
            headers: { 'Content-Type': 'application/json' },
            body: req.method !== 'GET' && req.method !== 'HEAD' 
              ? await new Promise((resolve) => {
                  let data = '';
                  req.on('data', chunk => data += chunk);
                  req.on('end', () => resolve(data));
                })
              : undefined,
          });
          
          res.writeHead(proxyRes.status, {
            'Content-Type': proxyRes.headers.get('content-type') || 'application/json',
          });
          res.end(await proxyRes.text());
        } catch (err) {
          res.writeHead(502, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'ProxyError', message: err.message }));
        }
        return;
      }
      
      // Static file serving
      let filePath = url.pathname === '/' ? '/index.html' : url.pathname;
      filePath = join(DIST_ROOT, filePath);
      
      try {
        await stat(filePath);
        const content = await readFile(filePath);
        const ext = extname(filePath).toLowerCase();
        res.writeHead(200, { 'Content-Type': MIME_TYPES[ext] || 'application/octet-stream' });
        res.end(content);
      } catch {
        // SPA fallback
        try {
          const indexPath = join(DIST_ROOT, 'index.html');
          const content = await readFile(indexPath);
          res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
          res.end(content);
        } catch {
          res.writeHead(404, { 'Content-Type': 'text/plain' });
          res.end('Not Found');
        }
      }
    });
    
    server.listen(PORT, '127.0.0.1', () => {
      console.log(`🚀 Vurio Desktop UI server running at http://127.0.0.1:${PORT}`);
      console.log(`   Mode: production (Node.js fallback)`);
      console.log(`   API proxy: ${API_TARGET}`);
    });
  }
}

main();
