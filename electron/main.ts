import { app, BrowserWindow, ipcMain, dialog, Menu, shell } from 'electron';
import path from 'path';
import { fileURLToPath } from 'url';
import os from 'os';
import fs from 'fs';

// Increase JS heap limit to reduce OOM risk when scrolling many thumbnails
app.commandLine.appendSwitch('js-flags', '--max-old-space-size=3072');

let mainWindow: BrowserWindow | null = null;

const UI_HOST = process.env.UI_HOST || '127.0.0.1';
const UI_PORT = parseInt(process.env.UI_PORT || '5173', 10);

// Resolve preload path from main script location (robust for Bun dev/build; app.getAppPath can be wrong)
const mainDir = path.dirname(fileURLToPath(import.meta.url));
const PRELOAD_PATH = path.join(mainDir, 'preload.js');

// Ensure single instance
const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
    }
  });
}

async function createWindow() {
  const appPath = app.getAppPath();
  const distHtmlPath = path.join(appPath, 'dist', 'index.html');
  const IS_DEV = process.env.NODE_ENV !== 'production' && !fs.existsSync(distHtmlPath);

  if (!fs.existsSync(PRELOAD_PATH)) {
    dialog.showErrorBox('Preload Missing', `Preload script not found at:\n${PRELOAD_PATH}\n\nEnsure you've run: bun run bun:build (or npm run build)`);
  } else {
    console.log('[Electron] Preload path:', PRELOAD_PATH);
  }

  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    backgroundColor: '#0b132b',
    webPreferences: {
      preload: PRELOAD_PATH,
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true,
      webSecurity: false, // Required to load ES modules via file:// in production
      allowRunningInsecureContent: true,
    },
    titleBarStyle: 'hiddenInset',
  });

  // Configure session to allow connections to localhost
  const session = mainWindow.webContents.session;
  session.setPermissionRequestHandler((webContents, permission, callback) => {
    // Allow all permissions for localhost connections
    callback(true);
  });

  if (IS_DEV) {
    mainWindow.webContents.once('did-finish-load', () => {
      mainWindow?.webContents.openDevTools();
    });
  }

  if (IS_DEV) {
    await mainWindow.loadURL(`http://${UI_HOST}:${UI_PORT}`);
  } else {
    await mainWindow.loadFile(distHtmlPath);
  }

  // Build application menu with log reveal actions
  const isMac = process.platform === 'darwin';
  const template: Electron.MenuItemConstructorOptions[] = [
    ...(isMac ? [{
      label: app.name,
      submenu: [
        { role: 'about' as const },
        { type: 'separator' as const },
        { role: 'services' as const },
        { type: 'separator' as const },
        { role: 'hide' as const },
        { role: 'hideOthers' as const },
        { role: 'unhide' as const },
        { type: 'separator' as const },
        { role: 'quit' as const }
      ]
    }] : []),
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'forceReload' },
        { type: 'separator' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'togglefullscreen' }
      ]
    },
    {
      label: 'Help',
      submenu: [
        {
          label: 'Reveal Server Log',
          click: async () => {
            const p = '/tmp/dedup-server.log';
            const err = await shell.openPath(p);
            if (err) dialog.showErrorBox('Open Log Failed', `Could not open ${p}:\n${err}`);
          }
        },
        {
          label: 'Reveal App Log',
          click: async () => {
            const p = '/tmp/dedup-app.log';
            const err = await shell.openPath(p);
            if (err) dialog.showErrorBox('Open Log Failed', `Could not open ${p}:\n${err}`);
          }
        },
        { type: 'separator' },
        {
          label: 'Export Error Log',
          click: async () => {
            if (mainWindow) {
              try {
                const errors = await mainWindow.webContents.executeJavaScript(`
                  (() => {
                    if (typeof window.exportErrors === 'function') {
                      return window.exportErrors();
                    }
                    return 'Error logger not available. Open DevTools and run: exportErrors()';
                  })()
                `);
                
                const errorLogPath = path.join(os.homedir(), 'dedup-error-log.txt');
                fs.writeFileSync(errorLogPath, errors, 'utf8');
                
                const result = await dialog.showMessageBox(mainWindow, {
                  type: 'info',
                  title: 'Error Log Exported',
                  message: `Error log exported to:\n${errorLogPath}`,
                  buttons: ['Open File', 'OK'],
                  defaultId: 0,
                });
                
                if (result.response === 0) {
                  await shell.openPath(errorLogPath);
                }
              } catch (err) {
                const error = err as Error;
                dialog.showErrorBox('Export Failed', `Could not export error log:\n${error.message}`);
              }
            }
          }
        }
      ]
    }
  ];
  const menu = Menu.buildFromTemplate(template);
  Menu.setApplicationMenu(menu);
}

ipcMain.handle('utils:revealLog', async (_event, filePath: string) => {
  try {
    const err = await shell.openPath(filePath);
    if (err) throw new Error(err);
    return { ok: true };
  } catch (e) {
    const error = e as Error;
    dialog.showErrorBox('Open Log Failed', `${error.message}`);
    return { ok: false, error: error.message };
  }
});

ipcMain.handle('utils:toggleDevTools', () => {
  if (mainWindow) mainWindow.webContents.toggleDevTools();
});

ipcMain.handle('utils:exportErrorLog', async () => {
  if (!mainWindow) {
    return { ok: false, error: 'No main window' };
  }
  
  try {
    const errors = await mainWindow.webContents.executeJavaScript(`
      (() => {
        if (typeof window.exportErrors === 'function') {
          return window.exportErrors();
        }
        return null;
      })()
    `);
    
    if (!errors) {
      return { ok: false, error: 'Error logger not available' };
    }
    
    const errorLogPath = path.join(os.homedir(), 'dedup-error-log.txt');
    fs.writeFileSync(errorLogPath, errors, 'utf8');
    
    return { ok: true, path: errorLogPath };
  } catch (err) {
    const error = err as Error;
    return { ok: false, error: error.message };
  }
});

app.whenReady().then(async () => {
  app.on('web-contents-created', (_, contents) => {
    contents.on('did-finish-load', () => {
      contents.executeJavaScript(`
        if (!window.electronAPI && !window.vurioElectron) {
          console.error('[Vurio] Preload did not expose electronAPI - check preload script');
        }
      `).catch(() => {});
    });
  });
  await createWindow();

  app.on('activate', async () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      await createWindow();
    }
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

// Register IPC handlers at module load so they're ready before any renderer invokes them
ipcMain.handle('dialog:selectDirectory', async (_event, startPath?: string) => {
  // Use no parent window: standalone dialog works reliably on macOS when sheet (parented) may not appear
  const result = await dialog.showOpenDialog({
    properties: ['openDirectory'],
    title: 'Select a directory to scan',
    buttonLabel: 'Use This Folder',
    defaultPath: startPath && typeof startPath === 'string' ? startPath : undefined,
  });
  if (result.canceled || !result.filePaths || result.filePaths.length === 0) {
    return null;
  }
  return result.filePaths[0];
});
