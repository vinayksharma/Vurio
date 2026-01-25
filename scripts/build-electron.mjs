#!/usr/bin/env node
/**
 * Build script for Electron main/preload files
 * Uses Bun bundler if available, falls back to esbuild
 */

import { spawn } from 'child_process';
import { existsSync, mkdirSync } from 'fs';

const DIST_DIR = 'dist-electron';

// Ensure dist directory exists
if (!existsSync(DIST_DIR)) {
  mkdirSync(DIST_DIR, { recursive: true });
}

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
  await run('bun', [
    'build', 'electron/preload.ts',
    '--outdir', DIST_DIR,
    '--target', 'node',
    '--packages', 'external'
  ]);
}

async function buildWithEsbuild() {
  console.log('Building Electron files with esbuild...');
  await run('npx', [
    'esbuild', 'electron/main.ts',
    '--bundle',
    '--platform=node',
    `--outfile=${DIST_DIR}/main.js`,
    '--external:electron'
  ]);
  await run('npx', [
    'esbuild', 'electron/preload.ts',
    '--bundle',
    '--platform=node',
    `--outfile=${DIST_DIR}/preload.js`,
    '--external:electron'
  ]);
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
