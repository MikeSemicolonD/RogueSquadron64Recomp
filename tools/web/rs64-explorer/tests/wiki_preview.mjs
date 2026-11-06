// node tools/web/rs64-explorer/tests/wiki_preview.mjs [out dir]
// Copies docs/wiki to wiki-preview/ (gitignored) for VS Code's Markdown preview: wiki page links ([x](Page)) become Page.md links, and each page
// ends with the sidebar and footer, as the wiki shows them.
import { readdirSync, readFileSync, writeFileSync, rmSync, mkdirSync, cpSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const repo = resolve(fileURLToPath(import.meta.url), '../../../../..');
const src = join(repo, 'docs/wiki');
const out = resolve(process.argv[2] ?? join(repo, 'wiki-preview'));
const pages = new Set(readdirSync(src).filter((f) => f.endsWith('.md')).map((f) => f.slice(0, -3)));
const linkify = (md) => md.replace(/\]\(([^)\s#:/]+)(#[^)]*)?\)/g, (m, page, anchor = '') => (pages.has(page) ? `](${page}.md${anchor})` : m));

assert.equal(linkify('[a](Home) [b](Home#x) [c](images/a.png) [d](https://x/Home)'), '[a](Home.md) [b](Home.md#x) [c](images/a.png) [d](https://x/Home)');

const read = (name) => (pages.has(name) ? linkify(readFileSync(join(src, name + '.md'), 'utf8')).trim() : '');
const tail = [read('_Sidebar'), read('_Footer')].filter(Boolean).join('\n\n---\n\n');

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
cpSync(join(src, 'images'), join(out, 'images'), { recursive: true });

for (const page of pages) {
  if (page.startsWith('_')) continue;

  writeFileSync(join(out, page + '.md'), read(page) + (tail ? '\n\n---\n\n' + tail : '') + '\n');
}

console.log(`Wrote ${pages.size - 2} pages to ${out}; open Home.md and press Ctrl+Shift+V`);
