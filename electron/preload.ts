import { contextBridge, ipcRenderer } from 'electron';

contextBridge.exposeInMainWorld('electronAPI', {
  selectDirectory: (startPath?: string) => ipcRenderer.invoke('dialog:selectDirectory', startPath),
  revealLog: (filePath: string) => ipcRenderer.invoke('utils:revealLog', filePath),
  toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools'),
  exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog')
});
