import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const src=resolve(root,'node_modules/occt-import-js/dist');
const dst=resolve(root,'public/occt');
await mkdir(dst,{recursive:true});
await Promise.all(['occt-import-js.js','occt-import-js.wasm'].map(name=>copyFile(resolve(src,name),resolve(dst,name))));
console.log('Copied local OpenCascade runtime to public/occt/.');
