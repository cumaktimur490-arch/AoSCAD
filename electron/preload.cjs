const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('aoscadDesktop', Object.freeze({
  saveFile: (file) => ipcRenderer.invoke('save-file', file),
}));
