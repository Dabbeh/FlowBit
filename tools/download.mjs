// Helper: download a file over HTTPS using Node's fetch (bypasses schannel).
// Usage: node download.js <url> <outputPath>
import { writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { mkdirSync } from 'node:fs';

const [url, out] = process.argv.slice(2);
if (!url || !out) {
  console.error('usage: node download.js <url> <outputPath>');
  process.exit(2);
}

const res = await fetch(url, { redirect: 'follow' });
if (!res.ok) {
  console.error(`HTTP ${res.status} ${res.statusText} for ${url}`);
  process.exit(1);
}
mkdirSync(dirname(out), { recursive: true });
const buf = Buffer.from(await res.arrayBuffer());
writeFileSync(out, buf);
console.log(`saved ${buf.length} bytes -> ${out}`);
