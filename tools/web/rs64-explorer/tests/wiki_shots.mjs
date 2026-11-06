// node tools/web/rs64-explorer/tests/wiki_shots.mjs [--rom <rom.z64>] [--out <dir>] [--only <name,...>]
// Takes the wiki's explorer screenshots listed in wiki_shots.json, each in a fresh page with the ROM loaded. Steps call the page's WebMCP tools
// (WEBMCP_TOOLS in webmcp.js) or click / right-click / press like a user. Needs `npm install` and `npx playwright install chromium` in this folder.
// Crop: "clip": [x, y, width, height] in page pixels (1400 x 860 page), "element": a CSS selector to crop to (then clip is relative to it), "pad": pixels added around.
import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync, mkdirSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { join, extname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = resolve(fileURLToPath(import.meta.url), '../../../../..');
const arg = (name, def) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
};
const rom = resolve(arg('--rom', join(repo, 'build/Debug/rogue_squadron.z64')));
const out = resolve(arg('--out', join(repo, 'docs/wiki/images')));
const only = arg('--only', '')?.split(',').filter(Boolean);
const shots = JSON.parse(readFileSync(new URL('./wiki_shots.json', import.meta.url))).filter((s) => !only.length || only.includes(s.name));

// Served from the repo root so the explorer's ../../../src/video/default_lights.json resolves as on the site.
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.png': 'image/png' };
const server = createServer(async (req, res) => {
  const path = join(repo, decodeURIComponent(new URL(req.url, 'http://x').pathname));

  if (!path.startsWith(repo)) {
    res.writeHead(403).end();
    return;
  }

  try {
    const body = await readFile(path);

    res.writeHead(200, { 'content-type': types[extname(path)] ?? 'application/octet-stream' }).end(body);
  } catch {
    res.writeHead(404).end();
  }
});

await new Promise((ok) => server.listen(0, '127.0.0.1', ok));

const url = `http://127.0.0.1:${server.address().port}/tools/web/rs64-explorer/index.html`;
const browser = await chromium.launch({ args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist'] });
let failed = 0;

mkdirSync(out, { recursive: true });

for (const shot of shots) {
  const page = await browser.newPage({ viewport: { width: 1400, height: 860 }, colorScheme: shot.theme ?? 'dark' });

  try {
    await page.goto(url);
    await page.setInputFiles('#romfile', rom);
    await page.waitForFunction(() => !document.body.classList.contains('no-rom'), null, { timeout: 120000 });

    for (const step of shot.steps ?? []) {
      if (step.tool) {
        const r = await page.evaluate(([name, args]) => WEBMCP_TOOLS.find((t) => t.name === name).run(args), [step.tool, step.args ?? {}]);

        if (step.log) console.log(`  ${step.tool}:`, JSON.stringify(r).slice(0, 400));
      } else if (step.click) {
        await page.click(step.click);
      } else if (step.rightClick) {
        const box = await page.locator('#view').boundingBox();

        await page.mouse.click(box.x + box.width * step.rightClick[0], box.y + box.height * step.rightClick[1], { button: 'right' });
      } else if (step.select) {
        await page.selectOption(step.select[0], step.select[1]);
      } else if (step.press) {
        await page.keyboard.press(step.press);
      } else if (step.wait) {
        await page.waitForTimeout(step.wait);
      }
    }

    // Lets the view draw the final state (and textures finish uploading) before the capture.
    await page.waitForTimeout(shot.settle ?? 800);
    await page.evaluate(() => new Promise((ok) => requestAnimationFrame(() => requestAnimationFrame(ok))));

    const file = join(out, shot.name + '.png');
    const view = page.viewportSize();
    const pad = shot.pad ?? 0;
    let clip = shot.clip && { x: shot.clip[0], y: shot.clip[1], width: shot.clip[2], height: shot.clip[3] };

    if (shot.element) {
      const b = await page.locator(shot.element).boundingBox();

      if (!b) throw new Error(`${shot.element} is not on screen`);

      // clip, when given with element, is an offset from the element's top-left corner.
      clip = clip ? { ...clip, x: b.x + clip.x, y: b.y + clip.y } : b;
    }

    if (clip) {
      const x = Math.max(0, clip.x - pad), y = Math.max(0, clip.y - pad);

      clip = { x, y, width: Math.min(view.width, clip.x + clip.width + pad) - x, height: Math.min(view.height, clip.y + clip.height + pad) - y };
    }

    await page.screenshot({ path: file, clip });
    console.log('ok  ', shot.name);
  } catch (e) {
    failed++;
    console.log('FAIL', shot.name, e.message.split('\n')[0]);
  }

  await page.close();
}

await browser.close();
server.close();
process.exit(failed ? 1 : 0);
