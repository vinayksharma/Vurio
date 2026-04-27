/**
 * UI Server for VURIO
 * Serves the built React app and proxies API requests to the backend server
 * Can be used for browser-based access without Electron
 */

import { resolve, extname, join } from 'path';

const DEFAULT_HOST = process.env.UI_HOST || '127.0.0.1';
const DEFAULT_PORT = parseInt(process.env.UI_PORT || '1212', 10);
const DIST_ROOT = resolve(import.meta.dir, 'dist');
const API_TARGET = process.env.API_BASE || 'http://localhost:8088';
const VITE_PORT = parseInt(process.env.VITE_PORT || '5173', 10);
const IS_DEV = process.env.NODE_ENV !== 'production';

const MIME_TYPES: Record<string, string> = {
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
  '.map': 'application/json; charset=utf-8',
};

function resolveContentType(filePath: string): string {
  const ext = extname(filePath).toLowerCase();
  return MIME_TYPES[ext] || 'application/octet-stream';
}

function corsHeaders(): Record<string, string> {
  return {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET,POST,PUT,PATCH,DELETE,OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Requested-With',
  };
}

async function handleApiProxy(req: Request, url: URL): Promise<Response> {
  if (req.method === 'OPTIONS') {
    return new Response(null, {
      status: 204,
      headers: corsHeaders(),
    });
  }

  try {
    const targetUrl = new URL(url.pathname + url.search, API_TARGET);
    
    const proxyResponse = await fetch(targetUrl.toString(), {
      method: req.method,
      headers: {
        ...Object.fromEntries(req.headers.entries()),
        host: targetUrl.host,
      },
      body: req.method !== 'GET' && req.method !== 'HEAD' ? await req.text() : undefined,
    });

    const responseHeaders = new Headers(proxyResponse.headers);
    responseHeaders.delete('content-security-policy');
    Object.entries(corsHeaders()).forEach(([key, value]) => {
      responseHeaders.set(key, value);
    });

    return new Response(proxyResponse.body, {
      status: proxyResponse.status,
      statusText: proxyResponse.statusText,
      headers: responseHeaders,
    });
  } catch (err) {
    const error = err as Error;
    return new Response(JSON.stringify({ error: 'ProxyError', message: error.message }), {
      status: 502,
      headers: {
        'Content-Type': 'application/json; charset=utf-8',
        ...corsHeaders(),
      },
    });
  }
}

async function serveStaticFile(filePath: string): Promise<Response> {
  try {
    const file = Bun.file(filePath);
    const exists = await file.exists();
    
    if (!exists) {
      // Try serving index.html for SPA routing
      const indexPath = join(DIST_ROOT, 'index.html');
      const indexFile = Bun.file(indexPath);
      const indexExists = await indexFile.exists();
      
      if (indexExists) {
        return new Response(indexFile, {
          headers: {
            'Content-Type': 'text/html; charset=utf-8',
            ...corsHeaders(),
          },
        });
      }
      
      return new Response('Not Found', {
        status: 404,
        headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      });
    }

    return new Response(file, {
      headers: {
        'Content-Type': resolveContentType(filePath),
        ...corsHeaders(),
      },
    });
  } catch (err) {
    const error = err as Error;
    return new Response(`Failed to read file: ${error.message}`, {
      status: 500,
      headers: {
        'Content-Type': 'text/plain; charset=utf-8',
        ...corsHeaders(),
      },
    });
  }
}

async function proxyToVite(req: Request, url: URL): Promise<Response> {
  try {
    const viteUrl = new URL(url.pathname + url.search, `http://127.0.0.1:${VITE_PORT}`);
    
    const proxyResponse = await fetch(viteUrl.toString(), {
      method: req.method,
      headers: {
        ...Object.fromEntries(req.headers.entries()),
        host: `127.0.0.1:${VITE_PORT}`,
      },
      body: req.method !== 'GET' && req.method !== 'HEAD' ? await req.text() : undefined,
    });

    const responseHeaders = new Headers(proxyResponse.headers);
    responseHeaders.delete('content-security-policy');
    Object.entries(corsHeaders()).forEach(([key, value]) => {
      responseHeaders.set(key, value);
    });

    return new Response(proxyResponse.body, {
      status: proxyResponse.status,
      statusText: proxyResponse.statusText,
      headers: responseHeaders,
    });
  } catch (err) {
    // Vite not available, fall back to static files
    let requestedPath = decodeURIComponent(url.pathname);
    if (requestedPath === '/' || requestedPath === '') {
      requestedPath = '/index.html';
    }
    const filePath = join(DIST_ROOT, requestedPath);
    
    // Ensure the path is within DIST_ROOT
    if (!filePath.startsWith(DIST_ROOT)) {
      return new Response('Forbidden', {
        status: 403,
        headers: { 'Content-Type': 'text/plain; charset=utf-8', ...corsHeaders() },
      });
    }
    
    return serveStaticFile(filePath);
  }
}

async function handleRequest(req: Request): Promise<Response> {
  const url = new URL(req.url);
  
  // Handle API proxy
  if (url.pathname.startsWith('/api/')) {
    return handleApiProxy(req, url);
  }

  // In development, try to proxy to Vite dev server
  if (IS_DEV) {
    return proxyToVite(req, url);
  }

  // In production, serve static files
  let requestedPath = decodeURIComponent(url.pathname);
  if (requestedPath === '/' || requestedPath === '') {
    requestedPath = '/index.html';
  }

  const filePath = join(DIST_ROOT, requestedPath);
  
  // Ensure the path is within DIST_ROOT
  if (!filePath.startsWith(DIST_ROOT)) {
    return new Response('Forbidden', {
      status: 403,
      headers: { 'Content-Type': 'text/plain; charset=utf-8', ...corsHeaders() },
    });
  }

  return serveStaticFile(filePath);
}

// Start the server
const server = Bun.serve({
  port: DEFAULT_PORT,
  hostname: DEFAULT_HOST,
  fetch: handleRequest,
});

console.log(`🚀 VURIO UI server running at http://${server.hostname}:${server.port}`);
console.log(`   Mode: ${IS_DEV ? 'development' : 'production'}`);
console.log(`   API proxy: ${API_TARGET}`);

if (IS_DEV) {
  console.log(`   Vite proxy: http://127.0.0.1:${VITE_PORT}`);
}

export { server };
