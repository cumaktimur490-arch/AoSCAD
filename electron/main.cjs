const { app, BrowserWindow, protocol, Menu, shell, dialog, ipcMain } = require('electron');
const { readFile, writeFile } = require('node:fs/promises');
const { basename, extname, resolve, sep } = require('node:path');

protocol.registerSchemesAsPrivileged([
  {
    scheme: 'aoscad',
    privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true },
  },
]);

const contentTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

async function serveApp(request) {
  try {
    const requestUrl = new URL(request.url);
    if (requestUrl.hostname !== 'app') return new Response('Not found', { status: 404 });
    const appRoot = resolve(app.getAppPath(), 'dist');
    const pathname = decodeURIComponent(requestUrl.pathname).replace(/^\/+/, '');
    const filePath = resolve(appRoot, pathname || 'index.html');
    if (filePath !== appRoot && !filePath.startsWith(`${appRoot}${sep}`)) {
      return new Response('Forbidden', { status: 403 });
    }
    const body = await readFile(filePath);
    return new Response(request.method === 'HEAD' ? null : body, {
      headers: {
        'Content-Type': contentTypes[extname(filePath).toLowerCase()] || 'application/octet-stream',
        'X-Content-Type-Options': 'nosniff',
        'Cache-Control': 'no-store',
      },
    });
  } catch {
    return new Response('Not found', { status: 404 });
  }
}

function createWindow() {
  const window = new BrowserWindow({
    width: 1480,
    height: 940,
    minWidth: 920,
    minHeight: 640,
    backgroundColor: '#11171d',
    autoHideMenuBar: true,
    show: false,
    icon: resolve(__dirname, '../assets/icon.ico'),
    webPreferences: {
      preload: resolve(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
    },
  });

  window.once('ready-to-show', () => window.show());
  window.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith('https://')) shell.openExternal(url);
    return { action: 'deny' };
  });
  window.loadURL('aoscad://app/index.html');
  return window;
}

const hasLock = app.requestSingleInstanceLock();
if (!hasLock) {
  app.quit();
} else {
  app.on('second-instance', () => {
    const window = BrowserWindow.getAllWindows()[0];
    if (window) {
      if (window.isMinimized()) window.restore();
      window.focus();
    }
  });

  app.whenReady().then(() => {
    Menu.setApplicationMenu(null);
    ipcMain.handle('save-file', async (event, payload = {}) => {
      if (!event.senderFrame?.url.startsWith('aoscad://app/')) return false;
      const filename = basename(String(payload.filename || 'model.bin')).replace(/[\\/:*?"<>|]+/g, '-');
      const extension = extname(filename).slice(1).toLowerCase();
      const bytes = payload.bytes;
      if (!bytes || (bytes.byteLength || bytes.length || 0) > 512 * 1024 * 1024) return false;
      const owner = BrowserWindow.fromWebContents(event.sender);
      const { canceled, filePath } = await dialog.showSaveDialog(owner, {
        title: 'Сохранить файл AoSCAD',
        defaultPath: resolve(app.getPath('downloads'), filename),
        buttonLabel: 'Сохранить',
        filters: extension ? [{ name: 'Файл AoSCAD', extensions: [extension] }] : [],
      });
      if (canceled || !filePath) return false;
      await writeFile(filePath, Buffer.from(bytes));
      return true;
    });
    protocol.handle('aoscad', serveApp);
    createWindow();
    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
  });
}

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
