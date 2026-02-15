#!/usr/bin/env node
/**
 * Build script for Electron main/preload files
 * Uses Bun bundler if available, falls back to esbuild
 * Preload: copied as-is (CommonJS) - Bun/ESM bundles can fail to expose in Electron preload context
 */

import { spawn } from 'child_process';
import { existsSync, mkdirSync, copyFileSync } from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const DIST_DIR = path.join(ROOT, 'dist-electron');

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

/**
 * Run a command and return a promise
 */
function run(cmd, args) {
  return new Promise((resolve, reject) => {
    console.log(`> ${cmd} ${args.join(' ')}`);
    const proc = spawn(cmd, args, { stdio: 'inherit', shell: true });
    proc.on('close', (code) => {
      if (code === 0) resolve();
      else reject(new Error(`Command failed with code ${code}`));
    });
    proc.on('error', reject);
  });
}

async function buildWithBun() {
  console.log('Building Electron files with Bun bundler...');
  await run('bun', [
    'build', 'electron/main.ts',
    '--outdir', DIST_DIR,
    '--target', 'node',
    '--packages', 'external'
  ]);
  // Preload: copy unbundled CJS - Bun ESM bundle fails to expose contextBridge in Electron preload
  copyFileSync(path.join(ROOT, 'electron', 'preload.cjs'), path.join(DIST_DIR, 'preload.js'));
  console.log('Copied electron/preload.cjs -> dist-electron/preload.js');
}

async function buildWithEsbuild() {
  console.log('Building Electron files with esbuild...');
  await run('npx', [
    'esbuild', 'electron/main.ts',
    '--bundle',
    '--platform=node',
    `--outfile=${path.join(DIST_DIR, 'main.js')}`,
    '--external:electron'
  ]);
  // Preload: copy unbundled CJS
  copyFileSync(path.join(ROOT, 'electron', 'preload.cjs'), path.join(DIST_DIR, 'preload.js'));
  console.log('Copied electron/preload.cjs -> dist-electron/preload.js');
}

async function main() {
  try {
    const bunAvailable = await hasBun();
    
    if (bunAvailable) {
      await buildWithBun();
    } else {
      await buildWithEsbuild();
    }
    
    console.log('✅ Electron build complete');
  } catch (error) {
    console.error('❌ Build failed:', error.message);
    process.exit(1);
  }
}

main();
