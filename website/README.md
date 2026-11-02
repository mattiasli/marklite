# MarkLite website

The landing page for MarkLite, built with [Astro](https://astro.build) as a static site. It is
deployed on Vercel at https://marklite-app.vercel.app.

## Working on it

Needs Node 24 (Node 22.23 or newer also works).

```
npm ci
npm run dev       # http://localhost:4321
```

| Script | What it does |
|---|---|
| `npm run dev` | Development server with live reload |
| `npm run check` | Type-checks the Astro and TypeScript files |
| `npm run build` | Builds the static site into `dist/` |
| `npm run preview` | Serves `dist/` locally |
| `npm test` | Checks the built site (run `npm run build` first) |

`npm test` runs `scripts/test-dist.mjs` and then [html-validate](https://html-validate.org). The
script checks that:

- the page says "Carefully crafted by Mattias Li" and links to mattias.li, LinkedIn, the GitHub repo and
  the installer download
- the version shown matches the `VERSION` file in the repository root
- every internal link, image and asset points at a file in `dist/`
- no space was lost before inline tags, and every image has alt text

## Where things are

- `src/pages/index.astro` is the page itself, `src/pages/404.astro` the not-found page.
- `src/components/` has the header, footer and the inline icon set.
- `src/site.ts` holds the URLs (repo, download, author links).
- `src/assets/` has the app icon and the two screenshots. Astro converts them to AVIF and WebP at
  several sizes during the build.
- `public/` is copied as is: favicons, `og.png` (the link preview image) and `robots.txt`.

The app version comes from `../VERSION` and is read in `astro.config.mjs` at build time, so the site
always shows the version in the repository.

The download button points at
`https://github.com/mattiasli/marklite/releases/latest/download/MarkLite-Setup.exe`, which always
serves the installer from the newest GitHub release.

## Screenshots

The screenshots are real captures of MarkLite with a sample document, at 175% display scaling. To
replace them, keep the same file names in `src/assets/` (`screenshot-light.png`,
`screenshot-dark.png`). Run MarkLite from a copy that has its own `MarkLite.ini` next to it (portable
mode) so your own settings and session aren't in the shot.

## Deploying

Vercel builds the site from the `main` branch through its GitHub integration. The project settings
are:

- Root Directory: `website`
- Framework, install, build and output settings come from `vercel.json`
- "Include files outside the root directory in the Build Step" must stay on, because the build reads
  `../VERSION`

Pull requests get a preview deployment automatically.
