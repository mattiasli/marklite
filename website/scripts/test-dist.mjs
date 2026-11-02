// Checks the built site in dist/: required text and links, the version number, and that every
// internal link, image and asset points at a file that exists. Run `npm run build` first.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');
const failures = [];
const fail = (msg) => failures.push(msg);

if (!existsSync(join(dist, 'index.html'))) {
  console.error('dist/index.html not found. Run `npm run build` first.');
  process.exit(1);
}

const htmlFiles = [];
(function walk(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p);
    else if (name.endsWith('.html')) htmlFiles.push(p);
  }
})(dist);

const decode = (s) =>
  s
    .replace(/&amp;/g, '&')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&nbsp;/g, ' ');
const textOf = (html) =>
  decode(
    html
      .replace(/<script[\s\S]*?<\/script>/gi, ' ')
      .replace(/<style[\s\S]*?<\/style>/gi, ' ')
      .replace(/<[^>]+>/g, ' '),
  )
    .replace(/\s+/g, ' ')
    .trim();
const hrefsOf = (html) => [...html.matchAll(/\shref="([^"]*)"/g)].map((m) => decode(m[1]));

// ---------------------------------------------------------------- the home page
const index = readFileSync(join(dist, 'index.html'), 'utf8');
const indexText = textOf(index);
const indexHrefs = hrefsOf(index);

if (!indexText.includes('Carefully crafted by Mattias Li')) fail('index.html: missing "Carefully crafted by Mattias Li"');

const requiredLinks = [
  'https://www.mattias.li',
  'https://www.linkedin.com/in/mattiasli/',
  'https://github.com/mattiasli/marklite',
  'https://github.com/mattiasli/marklite/releases/latest/download/MarkLite-Setup.exe',
];
for (const url of requiredLinks) {
  if (!indexHrefs.includes(url)) fail(`index.html: no link to ${url}`);
}

let version = null;
try {
  version = readFileSync(join(root, '..', 'VERSION'), 'utf8').trim();
} catch {
  fail('../VERSION could not be read');
}
if (version && !indexText.includes(`Version ${version}`)) fail(`index.html: "Version ${version}" (from ../VERSION) not shown`);

// A word glued to an inline tag ("Choose<strong>") means a space was lost when Astro compressed
// whitespace around a line break in the source.
for (const file of htmlFiles) {
  const html = readFileSync(file, 'utf8');
  for (const m of html.matchAll(/[A-Za-z0-9,]<(strong|em|code|kbd|a)\b/g)) {
    fail(`${relative(dist, file)}: missing space before <${m[1]}>: ...${html.slice(Math.max(0, m.index - 24), m.index + 8)}...`);
  }
}

if (!/<link rel="canonical" href="https:\/\/marklite-app\.vercel\.app\/?"/.test(index)) fail('index.html: canonical URL missing');
if (!/<meta property="og:image" content="https:\/\/marklite-app\.vercel\.app\/og\.png"/.test(index)) fail('index.html: og:image missing');

// ---------------------------------------------------------------- every page: internal links and assets
for (const file of htmlFiles) {
  const html = readFileSync(file, 'utf8');
  const name = relative(dist, file);
  const ids = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));
  const refs = [
    ...hrefsOf(html),
    ...[...html.matchAll(/\ssrc="([^"]*)"/g)].map((m) => decode(m[1])),
    ...[...html.matchAll(/\ssrcset="([^"]*)"/g)].flatMap((m) =>
      decode(m[1])
        .split(',')
        .map((part) => part.trim().split(/\s+/)[0])
        .filter(Boolean),
    ),
  ];

  for (const ref of refs) {
    if (/^(https?:|mailto:|tel:|data:)/.test(ref)) continue;
    const [pathPart, hash] = ref.split('#');
    if (!pathPart) {
      if (hash && !ids.has(hash)) fail(`${name}: in-page link #${hash} has no target`);
      continue;
    }
    const clean = pathPart.split('?')[0];
    const target = clean.startsWith('/') ? join(dist, clean) : join(dirname(file), clean);
    const candidates = [target, join(target, 'index.html'), `${target}.html`];
    const found = candidates.find((c) => existsSync(c) && statSync(c).isFile());
    if (!found) {
      fail(`${name}: ${ref} does not resolve to a file in dist/`);
      continue;
    }
    if (hash && found.endsWith('.html')) {
      const targetIds = new Set([...readFileSync(found, 'utf8').matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));
      if (!targetIds.has(hash)) fail(`${name}: ${ref} points at a missing id`);
    }
  }

  // Images need alt text (empty alt is fine for decorative ones).
  for (const m of html.matchAll(/<img\b[^>]*>/g)) {
    if (!/\salt(=|\s|>)/.test(m[0])) fail(`${name}: <img> without alt: ${m[0].slice(0, 80)}`);
  }
}

if (!existsSync(join(dist, '404.html'))) fail('404.html missing');

if (failures.length) {
  console.error(`Site checks failed (${failures.length}):`);
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log(`Site checks passed: ${htmlFiles.length} page(s), version ${version}.`);
