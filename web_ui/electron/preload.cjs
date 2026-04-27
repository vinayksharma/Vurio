'use strict';

const { contextBridge, ipcRenderer } = require('electron');

const api = {
  selectDirectory: (startPath) => ipcRenderer.invoke('dialog:selectDirectory', startPath),
  revealLog: (filePath) => ipcRenderer.invoke('utils:revealLog', filePath),
  toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools'),
  exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog'),
  openPath: (path) => ipcRenderer.invoke('utils:openPath', path),
  showItemInFolder: (path) => ipcRenderer.invoke('utils:showItemInFolder', path),
  openViewer: (options) => ipcRenderer.invoke('window:openViewer', options),
};

// Expose the API to the renderer process via a secure bridge
contextBridge.exposeInMainWorld('electronAPI', api);
contextBridge.exposeInMainWorld('vurioElectron', api);
