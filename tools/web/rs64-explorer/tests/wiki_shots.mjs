// node tools/web/rs64-explorer/tests/wiki_shots.mjs [--rom <rom.z64>] [--out <dir>] [--only <name,...>] [--browser msedge|chrome]
// Takes the wiki's explorer screenshots listed in wiki_shots.json, each in a fresh page with the ROM loaded. Steps call the page's WebMCP tools
// (WEBMCP_TOOLS in webmcp.js) or click / right-click / select / fill / press like a user. Needs `npm install` in this folder and an installed Edge or Chrome.
// Crop: "clip": [x, y, width, height] in page pixels, "element": a CSS selector to crop to (then clip is relative to it), "pad": pixels added around.
// "viewport": [width, height] (default 1400 x 860), "hide": selectors hidden before the capture, "theme": "light" (default dark).
// "annotations": { "arrow": <selector or [x, y]>, "angle", "length", "size", "offset": [dx, dy], "gap", "text", "textSize" }
// and { "label": <text>, "at": <selector or [x, y]>, "offset", "size" }; both take "color", "textColor", "outline" (px) and "outlineColor".
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

// Runs in the page: draws arrows and labels over everything, in page pixels. An arrow's tip sits on the edge of its target's box facing
// where it comes from; angle 0 comes from the right, 90 from below, 180 from the left, 270 from above.
function drawAnnotations(list)
{

  const NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  const layer = document.createElement('div');

  svg.setAttribute('style', 'position:fixed;inset:0;width:100vw;height:100vh;pointer-events:none;z-index:2147483646;overflow:visible');
  layer.setAttribute('style', 'position:fixed;inset:0;pointer-events:none;z-index:2147483647');
  document.body.append(svg, layer);

  const rectOf = (at) => {

    if (Array.isArray(at))
      return { x: at[0], y: at[1], width: 0, height: 0 };

    const el = document.querySelector(at);

    if (!el)
      throw new Error(`annotation target ${at} not found`);

    return el.getBoundingClientRect();
  };

  // A label box whose edge touches (x, y) on the side facing direction (dx, dy); centered on it without a direction.
  const label = (text, x, y, size, s, dx = 0, dy = 0) => {

    const box = document.createElement('div');

    box.textContent = text;
    box.setAttribute('style', `position:absolute;left:0;top:0;white-space:nowrap;font:600 ${size}px system-ui,sans-serif;color:${s.textColor};background:${s.color};padding:${size * 0.3}px ${size * 0.6}px;border-radius:${size * 0.4}px;box-shadow:0 0 0 ${s.outline}px ${s.outlineColor}`);
    layer.append(box);

    const w = box.offsetWidth;
    const h = box.offsetHeight;
    const t = Math.min(dx ? w / 2 / Math.abs(dx) : Infinity, dy ? h / 2 / Math.abs(dy) : Infinity);
    const shift = Number.isFinite(t) ? t + 4 : 0;

    box.style.transform = `translate(${x + dx * shift - w / 2}px, ${y + dy * shift - h / 2}px)`;
  };

  for (const a of list)
  {
    const s = { color: a.color ?? '#ff4f2e', textColor: a.textColor ?? '#fff', outline: a.outline ?? 2, outlineColor: a.outlineColor ?? 'rgba(0,0,0,.6)' };

    if (a.label != null)
    {
      const r = rectOf(a.at);
      const [ox, oy] = a.offset ?? [0, 0];

      label(a.label, r.x + r.width / 2 + ox, r.y + r.height / 2 + oy, a.size ?? 15, s);
      continue;
    }

    const r = rectOf(a.arrow);
    const ang = ((a.angle ?? 225) * Math.PI) / 180;
    const dx = Math.cos(ang);
    const dy = Math.sin(ang);
    const cx = r.x + r.width / 2;
    const cy = r.y + r.height / 2;
    const edge = Math.min(dx ? r.width / 2 / Math.abs(dx) : Infinity, dy ? r.height / 2 / Math.abs(dy) : Infinity);
    const gap = edge + (a.gap ?? 6);
    const [ox, oy] = a.offset ?? [0, 0];
    const tipX = cx + dx * gap + ox;
    const tipY = cy + dy * gap + oy;
    const len = a.length ?? 70;
    const size = a.size ?? 4;
    const head = size * 3.5;
    const tailX = tipX + dx * len;
    const tailY = tipY + dy * len;
    const baseX = tipX + dx * head;
    const baseY = tipY + dy * head;
    const nx = -dy * head * 0.6;
    const ny = dx * head * 0.6;
    const shape = `<polygon points="${tipX},${tipY} ${baseX + nx},${baseY + ny} ${baseX - nx},${baseY - ny}" stroke-linejoin="round"/>`;
    const line = (w) => `<line x1="${tailX}" y1="${tailY}" x2="${baseX}" y2="${baseY}" stroke-width="${w}" stroke-linecap="round"/>`;

    // The outline is the same shapes drawn wider underneath in the outline colour.
    svg.insertAdjacentHTML('beforeend', `<g stroke="${s.outlineColor}" fill="${s.outlineColor}" stroke-width="${s.outline * 2}">${line(size + s.outline * 2)}${shape}</g>`
      + `<g stroke="${s.color}" fill="${s.color}" stroke-width="0">${line(size)}${shape}</g>`);

    if (a.text)
      label(a.text, tailX, tailY, a.textSize ?? 15, s, dx, dy);
  }
}

// Served from the repo root so the explorer's ../../../src/video/default_lights.json resolves as on the site.
const types = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.png': 'image/png',
  '.svg': 'image/svg+xml'
};

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

// The installed Edge runs headless on the GPU; Playwright's bundled headless shell renders WebGPU/WebGL on the CPU, minutes per shot.
const browser = await chromium.launch({ channel: arg('--browser', 'msedge'), args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist'] });
let failed = 0;

mkdirSync(out, { recursive: true });

for (const shot of shots)
{
  const [width, height] = shot.viewport ?? [1400, 860];
  const page = await browser.newPage({ viewport: { width, height }, colorScheme: shot.theme ?? 'dark' });

  try {
    await page.goto(url);
    await page.setInputFiles('#romfile', rom);
    await page.waitForFunction(() => !document.body.classList.contains('no-rom'), null, { timeout: 120000 });

    for (const step of shot.steps ?? []) {
      if (step.tool)
      {
        const r = await page.evaluate(([name, args]) => WEBMCP_TOOLS.find((t) => t.name === name).run(args), [step.tool, step.args ?? {}]);

        if (step.log)
          console.log(`  ${step.tool}:`, JSON.stringify(r).slice(0, 400));
      }
      else if (step.eval) {
        await page.evaluate(step.eval);
      } else if (step.click) {
        await page.click(step.click);
      } else if (step.rightClick)
      {
        const box = await page.locator('#view').boundingBox();

        await page.mouse.click(box.x + box.width * step.rightClick[0], box.y + box.height * step.rightClick[1], { button: 'right' });
      }
      else if (step.select) {
        await page.selectOption(step.select[0], step.select[1]);
      } else if (step.fill) {
        await page.fill(step.fill[0], step.fill[1]);
      } else if (step.press) {
        await page.keyboard.press(step.press);
      } else if (step.wait) {
        await page.waitForTimeout(step.wait);
      }
    }

    if (shot.hide)
      await page.addStyleTag({ content: `${shot.hide.join(', ')} { visibility: hidden !important; }` });

    if (shot.annotations)
      await page.evaluate(drawAnnotations, shot.annotations);

    // Lets the view draw the final state (and textures finish uploading) before the capture.
    await page.waitForTimeout(shot.settle ?? 800);
    await page.evaluate(() => new Promise((ok) => requestAnimationFrame(() => requestAnimationFrame(ok))));

    const file = join(out, shot.name + '.png');
    const view = page.viewportSize();
    const pad = shot.pad ?? 0;
    let clip = shot.clip && { x: shot.clip[0], y: shot.clip[1], width: shot.clip[2], height: shot.clip[3] };

    if (shot.element)
    {
      const b = await page.locator(shot.element).boundingBox();

      if (!b)
        throw new Error(`${shot.element} is not on screen`);

      // clip, when given with element, is an offset from the element's top-left corner.
      clip = clip ? { ...clip, x: b.x + clip.x, y: b.y + clip.y } : b;
    }

    if (clip)
    {
      const x = Math.max(0, clip.x - pad);
      const y = Math.max(0, clip.y - pad);

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
