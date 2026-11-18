# Web JS style

The JavaScript under `tools/web` keeps the comment, one-statement and no-bare-block rules from [AGENTS.md](../../AGENTS.md#style-conventions), with its own layout. `style_check` checks the comment and one-statement rules. `voice_cats.js` (generated), the vendored `tools/web/play/coi-serviceworker.js` and the built `tools/web/play/rs64tint.js` are skipped.

`node tools/web/rs64-explorer/tests/jsfmt.mjs --write` applies the layout to every file (or the ones named) and `--check` lists the files that differ. Both need `npm install` in that folder.

```js
function inspect(hit)
{
  const o = hit.object;
  const tile = tileOf(o);

  if (!tile)
    return null;

  let alpha = 'opaque';

  if (info.valpha)
    alpha = 'blended';
  else if (info.cutout)
    alpha = 'cutout';

  return {
    title: `Tile ${tile.x}, ${tile.y}`,
    rows: [
      ['Alpha', alpha],
      ['Flags', hex(tile.flags)]
    ]
  };
}
```

- The opening brace of an `if` / `else` / loop / `switch` / named function goes on its own line when the header spans lines or the header or a direct body line is 70+ characters; otherwise, and for every arrow function, object and array, it stays on the statement's line.
- A body without braces always goes on the next line, two spaces in (`if (x)` / `return;`, `else` / `y();`, `while (...)` / `i++;`).
- One declaration per `const` / `let` / `var`.
- A literal that spans lines is the opening brace, one element per line two spaces in, then the closing brace; a single-line literal on a line over 160 characters is split that way. Lists of plain numbers, strings or shorthand names, and `key: plain value` lookup tables, stay packed. Comment lines inside a literal stay above the element they describe.
- A ternary inside a ternary is in parentheses; a chain of three or more reads better as `if` / `else if` (as above).
- Blank lines separate steps: around multi-line blocks and statements (a guard with its body on the next line counts), where a run of declarations gives way to other statements or back, before a `return` or `throw` that follows other statements, and before a comment that follows code. Never before a closing brace.
- A file's header comment and reference notes on data formats may run past 2 lines.
