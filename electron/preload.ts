import { contextBridge, ipcRenderer } from 'electron';
import os from 'os';

contextBridge.exposeInMainWorld('electronAPI', {
  selectDirectory: (startPath?: string) => ipcRenderer.invoke('dialog:selectDirectory', startPath),
  getHomeDir: () => os.homedir(),
  revealLog: (filePath: string) => ipcRenderer.invoke('utils:revealLog', filePath),
  toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools'),
  exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog')
});
