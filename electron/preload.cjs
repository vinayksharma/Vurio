'use strict';

const { contextBridge, ipcRenderer } = require('electron');
const os = require('os');

const api = {
  selectDirectory: (startPath) => ipcRenderer.invoke('dialog:selectDirectory', startPath),
  getHomeDir: () => os.homedir(),
  revealLog: (filePath) => ipcRenderer.invoke('utils:revealLog', filePath),
  toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools'),
  exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog'),
};

// Expose the API to the renderer process via a secure bridge
contextBridge.exposeInMainWorld('electronAPI', api);
contextBridge.exposeInMainWorld('vurioElectron', api);
