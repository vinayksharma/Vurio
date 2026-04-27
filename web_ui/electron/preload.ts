import { contextBridge, ipcRenderer } from 'electron';

contextBridge.exposeInMainWorld('electronAPI', {
  selectDirectory: (startPath?: string) => ipcRenderer.invoke('dialog:selectDirectory', startPath),
  revealLog: (filePath: string) => ipcRenderer.invoke('utils:revealLog', filePath),
  showItemInFolder: (filePath: string) => ipcRenderer.invoke('utils:showItemInFolder', filePath),
  toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools'),
  exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog'),
  clearWebCache: () => ipcRenderer.invoke('utils:clearWebCache'),
  openViewer: (options: { filePath: string; details: string }) => ipcRenderer.invoke('window:openViewer', options),
  onSuspend: (callback: () => void) => ipcRenderer.on('app:suspend', () => callback()),
  onResume: (callback: () => void) => ipcRenderer.on('app:resume', () => callback())
});
