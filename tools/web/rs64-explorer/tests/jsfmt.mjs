// node tools/web/rs64-explorer/tests/jsfmt.mjs [--check | --write] [files...]
// Lays out the web JS the way AGENTS.md "Web JS" describes (style_check checks only part of it line by line). With no files it takes every
// .js / .mjs under tools/web except voice_cats.js (generated). --check lists what would change and exits 1; --write rewrites. Needs `npm install`
// in this folder (TypeScript's parser). Every rewrite is verified first: the same code once declaration lists are split and ternary parentheses
// dropped, the same string and template texts, the same comment words, it parses, and a second run changes nothing.
import ts from 'typescript';
import { readFileSync, writeFileSync, renameSync, readdirSync } from 'node:fs';
import { join, relative, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const repo = resolve(fileURLToPath(import.meta.url), '../../../../..');
const WRITE = process.argv.includes('--write');
const CHECK = process.argv.includes('--check');
const K = ts.SyntaxKind;
const LONG_LINE = 160;
const BRACE_LINE = 70;
const report = [];
const parse = (name, text) => ts.createSourceFile(name, text, ts.ScriptTarget.Latest, true, ts.ScriptKind.JS);

function webFiles(dir) {
  const out = [];

  for (const e of readdirSync(dir, { withFileTypes: true }))
  {
    const p = join(dir, e.name);

    if (e.isDirectory() && e.name !== 'node_modules')
      out.push(...webFiles(p));
    else if (e.isFile() && /\.m?js$/.test(e.name) && e.name !== 'voice_cats.js')
      out.push(p);
  }

  return out;
}

// ---------- helpers ----------

function lineInfo(text)
{
  const starts = [0];

  for (let i = 0; i < text.length; i++)
    if (text[i] === '\n')
      starts.push(i + 1);

  const lineOf = (pos) => {
    let lo = 0;
    let hi = starts.length - 1;

    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;

      if (starts[mid] <= pos)
        lo = mid;
      else
        hi = mid - 1;
    }

    return lo;
  };

  const lineText = (l) => text.slice(starts[l], l + 1 < starts.length ? starts[l + 1] - 1 : text.length);
  const ws = (l) => /^[ \t]*/.exec(lineText(l))[0];

  return { starts, lineOf, lineText, ws };
}

// Applies edits, dropping any that overlap an earlier-starting (outer) one; returns [text, applied count].
function applyEdits(text, edits) {
  edits.sort((a, b) => a.start - b.start || b.end - a.end);

  const keep = [];
  let lastEnd = -1;

  for (const e of edits) {
    if (e.start < lastEnd)
      continue;

    keep.push(e);
    lastEnd = Math.max(lastEnd, e.end);
  }

  for (const e of [...keep].reverse())
    text = text.slice(0, e.start) + e.text + text.slice(e.end);

  return [text, keep.length];
}

function allComments(sf, text) {
  const seen = new Map();

  const visit = (n) => {
    for (const r of [...(ts.getLeadingCommentRanges(text, n.pos) || []), ...(ts.getTrailingCommentRanges(text, n.end) || [])])
      seen.set(r.pos, r);

    ts.forEachChild(n, visit);
  };

  visit(sf);

  return [...seen.values()].sort((a, b) => a.pos - b.pos);
}

function walk(n, fn) {
  fn(n);
  ts.forEachChild(n, (c) => walk(c, fn));
}

// Template literal spans: lines starting inside one must never be re-indented.
function templateSpans(sf) {
  const out = [];

  walk(sf, (n) => {
    if (n.kind === K.TemplateExpression || n.kind === K.NoSubstitutionTemplateLiteral)
      out.push([n.getStart(sf), n.end]);
  });

  return out;
}

const inSpans = (spans, pos) => spans.some(([a, b]) => pos > a && pos < b);
const isList = (n) => n && [K.Block, K.SourceFile, K.CaseClause, K.DefaultClause, K.ModuleBlock].includes(n.kind);
const hasComment = (s) => /\/[/*]/.test(s);
const simpleEl = (e) => [
  K.NumericLiteral,
  K.StringLiteral,
  K.Identifier,
  K.TrueKeyword,
  K.FalseKeyword,
  K.NullKeyword
].includes(e.kind) || (e.kind === K.PrefixUnaryExpression && e.operand.kind === K.NumericLiteral);

// Lists of plain values, shorthand names, or key: plain value lookup tables stay packed.
function packedList(n, els)
{
  if (n.kind === K.ArrayLiteralExpression)
    return els.every(simpleEl);

  return els.every((e) => e.kind === K.ShorthandPropertyAssignment) || els.every((e) => e.kind === K.PropertyAssignment && simpleEl(e.initializer));
}

// Lines after the first of s (starting at file offset from) moved by delta columns; template literal lines untouched.
function shiftText(s, from, delta, spans) {
  let pos = from;

  return s.split('\n').map((line, i) => {
    const at = pos;

    pos += line.length + 1;

    if (!i || !line.trim() || inSpans(spans, at))
      return line;

    if (delta >= 0)
      return ' '.repeat(delta) + line;

    return line.slice(Math.min(-delta, /^ */.exec(line)[0].length));
  }).join('\n');
}

// ---------- verification ----------

const printer = ts.createPrinter({ removeComments: true, newLine: ts.NewLineKind.LineFeed });

// The code with layout taken out: declaration lists split, ternary parentheses and literal line breaks dropped, whitespace collapsed.
function shape(name, text)
{
  const f = ts.factory;

  const tr = (ctx) => {
    const visit = (n) => {
      if (n.kind === K.ParenthesizedExpression && n.expression.kind === K.ConditionalExpression)
        return ts.visitNode(n.expression, visit);

      if (n.kind === K.ObjectLiteralExpression)
        return f.createObjectLiteralExpression(ts.visitNodes(n.properties, visit), false);

      if (n.kind === K.ArrayLiteralExpression)
        return f.createArrayLiteralExpression(ts.visitNodes(n.elements, visit), false);

      if (n.kind === K.VariableStatement && isList(n.parent) && n.declarationList.declarations.length > 1)
      {
        return n.declarationList.declarations.map((d) => f.createVariableStatement(n.modifiers, f.createVariableDeclarationList([ts.visitNode(d, visit)], n.declarationList.flags)));
      }

      return ts.visitEachChild(n, visit, ctx);
    };

    return (node) => ts.visitNode(node, visit);
  };

  const out = ts.transform(parse(name, text), [tr]).transformed[0];

  return printer.printFile(out).replace(/\n\s*/g, ' ').replace(/([[{(]) /g, '$1').replace(/ ([\]})])/g, '$1').replace(/,(?=[\]}])/g, '');
}

function literalTexts(name, text)
{
  const sf = parse(name, text);
  const out = [];
  const kinds = [K.StringLiteral, K.NoSubstitutionTemplateLiteral, K.TemplateHead, K.TemplateMiddle, K.TemplateTail, K.RegularExpressionLiteral];

  walk(sf, (n) => {
    if (kinds.includes(n.kind))
      out.push(n.getText(sf));
  });

  return out.join('\u0000');
}

function commentWords(name, text)
{
  return allComments(parse(name, text), text).map((c) => text.slice(c.pos, c.end).replace(/^\/\/|^\/\*|\*\/$/g, '')).join(' ').split(/\s+/).filter(Boolean).join(' ');
}

// ---------- pass: comments ----------

// Where a joined comment splits in two: outside brackets at a sentence end, else at "; ", else at ", ", nearest the middle.
function commentSplit(joined) {
  const mid = joined.length / 2;

  const tests = [
    (i) => joined[i] === '.' && /[A-Za-z"'`]/.test(joined[i + 2]) && !/(e.g|i.e|etc)$/.test(joined.slice(Math.max(0, i - 3), i)),
    (i) => joined[i] === ';',
    (i) => joined[i] === ','
  ];

  for (const ok of tests) {
    let best = -1;
    let depth = 0;

    for (let i = 0; i < joined.length - 2; i++)
    {
      const ch = joined[i];

      if ('([{'.includes(ch))
        depth++;
      else if (')]}'.includes(ch))
        depth = Math.max(0, depth - 1);
      else if (!depth && joined[i + 1] === ' ' && ok(i) && (best < 0 || Math.abs(i - mid) < Math.abs(best - mid)))
        best = i;
    }

    if (best > 0 && Math.abs(best - mid) < joined.length / 3)
      return best;
  }

  return -1;
}

// A comment after code moves above its line; a run of 3+ comment lines joins into 1-2 lines when that fits 200 columns.
// Exempt: the file's header comment, "// ---- Section ----" headings, lists and tables, longer reference notes.
function commentPass(name, text)
{
  let sf = parse(name, text);
  let L = lineInfo(text);
  let edits = [];

  for (const c of allComments(sf, text))
  {
    if (c.kind !== K.SingleLineCommentTrivia)
      continue;

    const l = L.lineOf(c.pos);
    const before = text.slice(L.starts[l], c.pos);

    if (!before.trim())
      continue;

    edits.push({ start: c.pos - /[ \t]*$/.exec(before)[0].length, end: c.end, text: '' });
    edits.push({ start: L.starts[l], end: L.starts[l], text: L.ws(l) + text.slice(c.pos, c.end) + '\n' });
  }

  text = applyEdits(text, edits)[0];
  sf = parse(name, text);
  L = lineInfo(text);
  edits = [];

  const own = allComments(sf, text).filter((c) => c.kind === K.SingleLineCommentTrivia && !text.slice(L.starts[L.lineOf(c.pos)], c.pos).trim());
  const firstCode = sf.statements.find((s) => !(s.kind === K.ExpressionStatement && s.expression.kind === K.StringLiteral));
  const headerEnd = firstCode ? firstCode.getStart(sf) : text.length;

  const joinRun = (r) => {
    const indent = L.ws(L.lineOf(r[0].pos));
    const parts = r.map((c) => text.slice(c.pos + 2, c.end).replace(/^ /, ''));

    if (parts.some((p) => /^\s|^[-*•]|^\d+[.)]\s|^#|^[A-Za-z]+:\s*$|^\|/.test(p)))
      return;

    const joined = parts.join(' ').replace(/\s+/g, ' ').trim();
    let out = [joined];

    if (/^(node|npm|npx|usage|python|pwsh|cmake)\b/i.test(parts[0]))
    {
      out = [parts[0].trim(), parts.slice(1).join(' ').replace(/\s+/g, ' ').trim()];
    }
    else if (joined.length > 140) {
      const at = commentSplit(joined);

      if (at > 0)
        out = [joined.slice(0, at + 1), joined.slice(at + 2)];
    }

    if (out.some((s) => indent.length + 3 + s.length > 200))
      return;

    edits.push({ start: r[0].pos, end: r.at(-1).end, text: out.map((s, i) => (i ? indent : '') + '// ' + s).join('\n') });
  };

  let run = [];

  const flush = () => {
    if (run.length >= 3)
      joinRun(run);

    run = [];
  };

  for (const c of own) {
    if (c.pos < headerEnd)
      continue;

    if (/^\/\/ ?-{2,}.*-{2,}\s*$/.test(text.slice(c.pos, c.end))) {
      flush();
      continue;
    }

    const prev = run.at(-1);

    if (prev && L.lineOf(c.pos) === L.lineOf(prev.pos) + 1) {
      run.push(c);
    } else {
      flush();
      run = [c];
    }
  }

  flush();

  return applyEdits(text, edits)[0];
}

// ---------- pass: one declaration per statement ----------

function splitDeclsPass(name, text) {
  const sf = parse(name, text);
  const L = lineInfo(text);
  const edits = [];

  walk(sf, (n) => {
    if (n.kind !== K.VariableStatement || !isList(n.parent) || n.declarationList.declarations.length < 2)
      return;

    const ds = n.declarationList.declarations;
    const start = n.getStart(sf);
    const prefix = text.slice(start, ds[0].getStart(sf));

    if (ds.some((d, i) => i && hasComment(text.slice(ds[i - 1].end, d.getStart(sf)))))
      return report.push(`${name}:${L.lineOf(start) + 1}: comment inside a declaration list`);

    edits.push({ start, end: n.end, text: ds.map((d) => prefix + d.getText(sf) + ';').join('\n' + L.ws(L.lineOf(start))) });
  });

  return applyEdits(text, edits)[0];
}

// ---------- pass: nested ternaries get parentheses ----------

function ternaryPass(name, text) {
  const sf = parse(name, text);
  const edits = [];

  walk(sf, (n) => {
    if (n.kind !== K.ConditionalExpression)
      return;

    for (const part of [n.condition, n.whenTrue, n.whenFalse]) {
      if (part.kind === K.ConditionalExpression) {
        edits.push({ start: part.getStart(sf), text: '(' });
        edits.push({ start: part.end, text: ')' });
      }
    }
  });

  edits.sort((a, b) => b.start - a.start);

  for (const e of edits)
    text = text.slice(0, e.start) + e.text + text.slice(e.start);

  return text;
}

// ---------- pass: expand one-line blocks that hold an if / loop ----------

const LOOPS = [K.ForStatement, K.ForOfStatement, K.ForInStatement, K.WhileStatement];

function expandBlocksPass(name, text) {
  const sf = parse(name, text);
  const L = lineInfo(text);
  const edits = [];

  walk(sf, (n) => {
    if (n.kind !== K.Block || !n.statements.length)
      return;

    const o = n.getStart(sf);
    const c = n.end - 1;

    if (L.lineOf(o) !== L.lineOf(c) || !n.statements.some((s) => s.kind === K.IfStatement || LOOPS.includes(s.kind)) || hasComment(text.slice(o, c)))
      return;

    const ind = L.ws(L.lineOf(o));

    edits.push({ start: o, end: n.end, text: '{\n' + n.statements.map((s) => ind + '  ' + s.getText(sf)).join('\n') + '\n' + ind + '}' });
  });

  return applyEdits(text, edits);
}

// ---------- pass: a brace-less if / else / loop body goes on its own line ----------

function bodiesPass(name, text) {
  const sf = parse(name, text);
  const L = lineInfo(text);
  const spans = templateSpans(sf);
  const edits = [];

  // Later lines of a multi-line body move right with it.
  const move = (gapStart, body, ind) => {
    const bs = body.getStart(sf);

    if (!hasComment(text.slice(gapStart, bs)))
      edits.push({ start: gapStart, end: body.end, text: '\n' + ind + '  ' + shiftText(text.slice(bs, body.end), bs, 2, spans) });
  };

  walk(sf, (n) => {
    if (n.kind === K.IfStatement)
    {
      const t = n.thenStatement;
      const ind = L.ws(L.lineOf(n.getStart(sf)));
      const close = n.getChildren(sf).find((c) => c.kind === K.CloseParenToken);
      const e = n.elseStatement;
      const ek = e && n.getChildren(sf).find((c) => c.kind === K.ElseKeyword);

      if (t.kind !== K.Block && L.lineOf(close.end) === L.lineOf(t.getStart(sf)))
        move(close.end, t, ind);

      if (e && e.kind !== K.Block && e.kind !== K.IfStatement && L.lineOf(ek.end) === L.lineOf(e.getStart(sf)))
        move(ek.end, e, L.ws(L.lineOf(ek.getStart(sf))));

      if (e && t.kind !== K.Block && L.lineOf(t.end - 1) === L.lineOf(ek.getStart(sf)))
        edits.push({ start: t.end, end: ek.getStart(sf), text: '\n' + ind });
    }
    else if (LOOPS.includes(n.kind))
    {
      const b = n.statement;
      const close = n.getChildren(sf).filter((c) => c.kind === K.CloseParenToken).at(-1);

      if (b.kind !== K.Block && b.kind !== K.EmptyStatement && L.lineOf(close.end) === L.lineOf(b.getStart(sf)))
        move(close.end, b, L.ws(L.lineOf(n.getStart(sf))));
    }
  });

  return applyEdits(text, edits);
}

// ---------- pass: a long line's single-line object / array literal goes one element per line ----------

const literalParts = (n) => (n.kind === K.ObjectLiteralExpression ? [n.properties, '{', '}'] : [n.elements, '[', ']']);
const isLiteral = (n) => n.kind === K.ObjectLiteralExpression || n.kind === K.ArrayLiteralExpression;

function literalsPass(name, text) {
  const sf = parse(name, text);
  const L = lineInfo(text);
  const spans = templateSpans(sf);
  const edits = [];

  walk(sf, (n) => {
    if (!isLiteral(n))
      return;

    const [els, open, close] = literalParts(n);
    const o = n.getStart(sf);
    const l = L.lineOf(o);

    if (els.length < 2 || l !== L.lineOf(n.end - 1) || L.lineText(l).length <= LONG_LINE || n.end - o < 40 || packedList(n, els))
      return;

    if (inSpans(spans, o) || hasComment(text.slice(o, n.end)))
      return;

    const ind = L.ws(l);

    edits.push({
      start: o,
      end: n.end,
      text: open + '\n' + els.map((e) => ind + '  ' + e.getText(sf)).join(',\n') + (els.hasTrailingComma ? ',' : '') + '\n' + ind + close
    });
  });

  return applyEdits(text, edits);
}

// ---------- pass: a multi-line object / array literal is brace, one element per line, two-space indent, close ----------

function multiLiteralsPass(name, text) {
  const sf = parse(name, text);
  const L = lineInfo(text);
  const spans = templateSpans(sf);
  const edits = [];
  const col = (p) => p - L.starts[L.lineOf(p)];

  walk(sf, (n) => {
    if (!isLiteral(n))
      return;

    const [els, open, close] = literalParts(n);
    const o = n.getStart(sf);
    const c = n.end - 1;

    if (els.length < 2 || L.lineOf(o) === L.lineOf(c) || inSpans(spans, o) || packedList(n, els))
      return;

    const ind = L.ws(L.lineOf(o));
    const ind2 = ind + '  ';
    const ownLines = els.every((e, i) => col(e.getStart(sf)) === ind2.length && (!i || L.lineOf(e.getStart(sf)) > L.lineOf(els[i - 1].end - 1)));

    if (L.lineOf(els[0].getStart(sf)) > L.lineOf(o) && ownLines && L.lineOf(c) > L.lineOf(els.at(-1).end - 1) && col(c) === ind.length)
      return;

    // Comment lines between elements stay on their own lines above the element after them (or above the close); a /* */ stops this.
    const gaps = [...els.map((e, i) => text.slice(i ? els[i - 1].end : o + 1, e.getStart(sf))), text.slice(els.at(-1).end, c)];

    if (gaps.some((g) => g.includes('/*')))
      return report.push(`${name}:${L.lineOf(o) + 1}: multi-line literal with a block comment left as written`);

    const notes = (g) => g.split('\n').map((s) => s.trim()).filter((s) => s.startsWith('//')).map((s) => ind2 + s + '\n').join('');
    const body = els.map((e, i) => notes(gaps[i]) + ind2 + shiftText(e.getText(sf), e.getStart(sf), ind2.length - col(e.getStart(sf)), spans)).join(',\n');

    edits.push({ start: o, end: n.end, text: open + '\n' + body + (els.hasTrailingComma ? ',' : '') + '\n' + notes(gaps.at(-1)) + ind + close });
  });

  return applyEdits(text, edits);
}

// ---------- pass: a brace-less body on its own line sits two spaces in from its if / else / loop ----------

function bodyIndentPass(name, text) {
  const sf = parse(name, text);
  const L = lineInfo(text);
  const spans = templateSpans(sf);
  const edits = [];

  const fix = (headerStart, body) => {
    if (!body || [K.Block, K.IfStatement, K.EmptyStatement].includes(body.kind))
      return;

    const bs = body.getStart(sf);
    const l = L.lineOf(bs);

    if (l === L.lineOf(headerStart) || text.slice(L.starts[l], bs).trim())
      return;

    const want = L.ws(L.lineOf(headerStart)) + '  ';
    const have = L.ws(l);

    if (want !== have)
      edits.push({ start: L.starts[l], end: body.end, text: want + shiftText(text.slice(bs, body.end), bs, want.length - have.length, spans) });
  };

  walk(sf, (n) => {
    if (n.kind === K.IfStatement)
    {
      const ek = n.elseStatement && n.getChildren(sf).find((c) => c.kind === K.ElseKeyword);

      fix(n.getStart(sf), n.thenStatement);

      if (ek)
        fix(ek.getStart(sf), n.elseStatement);
    }
    else if (LOOPS.includes(n.kind)) {
      fix(n.getStart(sf), n.statement);
    }
  });

  return applyEdits(text, edits);
}

// ---------- pass: blank lines, indentation, statement splitting, own-line braces for long blocks ----------

const COMPOUND = new Set([
  K.IfStatement,
  ...LOOPS,
  K.DoStatement,
  K.SwitchStatement,
  K.TryStatement,
  K.LabeledStatement,
  K.Block,
  K.FunctionDeclaration,
  K.ClassDeclaration
]);

function layoutPass(name, text)
{
  const sf = parse(name, text);
  const L = lineInfo(text);
  const spans = templateSpans(sf);
  const edits = [];
  const ln = (pos) => L.lineOf(pos);
  const startOf = (n) => n.getStart(sf);
  const multiLine = (n) => ln(startOf(n)) !== ln(n.end - 1);
  const endsLikeBlock = (s) => multiLine(s) && /[}\]]\)*;?$/.test(s.getText(sf).trimEnd()) && [
    K.VariableStatement,
    K.ExpressionStatement,
    K.ReturnStatement
  ].includes(s.kind);
  const bigBlock = (s) => (COMPOUND.has(s.kind) && multiLine(s)) || endsLikeBlock(s);

  // Steps: declarations giving way to other simple statements (or back), and a return or throw after other statements.
  const decl = (s) => s.kind === K.VariableStatement;
  const step = (a, z) => z.kind === K.ReturnStatement || z.kind === K.ThrowStatement || (!COMPOUND.has(a.kind) && !COMPOUND.has(z.kind) && decl(a) !== decl(z));

  const existingBlank = (start, end) => {
    const parts = text.slice(start, end).split('\n').slice(1, -1);

    return parts.length > 0 && parts[0].trim() === '';
  };

  // The gap between two tokens on different lines: comment lines kept (re-indented), blank lines set before the first line, none doubled.
  const reGap = (start, end, leadBlank, dropTrailBlank, indent) => {
    const gap = text.slice(start, end);
    const parts = gap.split('\n');

    if (parts.length < 2)
      return null;

    const head = parts[0].replace(/[ \t]+$/, '');
    const mid = parts.slice(1, -1).map((s) => s.replace(/[ \t\r]+$/, '')).map((s) => (/^\s*\/\//.test(s) ? indent + s.trim() : s));
    const out = [];

    while (mid.length && mid[0] === '')
      mid.shift();

    for (const m of mid)
      if (!(m === '' && out.at(-1) === ''))
        out.push(m);

    if (dropTrailBlank)
      while (out.length && out.at(-1) === '')
        out.pop();

    if (leadBlank)
      out.unshift('');

    const next = [head, ...out, indent].join('\n');

    return next === gap ? null : { start, end, text: next };
  };

  walk(sf, (b) => {
    if (![K.Block, K.SourceFile, K.ModuleBlock].includes(b.kind) || !b.statements.length)
      return;

    const st = b.statements;
    const isFile = b.kind === K.SourceFile;
    const open = isFile ? -1 : startOf(b);
    const close = isFile ? text.length : b.end - 1;

    if (!isFile && ln(open) === ln(close))
      return;

    // Every statement sits two spaces in from its block's brace line, its continuation lines moving with it.
    const childIndent = isFile ? '' : L.ws(ln(open)) + '  ';

    for (const s of st)
    {
      const s0 = startOf(s);
      const l = ln(s0);

      if (!text.slice(L.starts[l], s0).trim() && L.ws(l) !== childIndent && multiLine(s))
        edits.push({ start: s0, end: s.end, text: shiftText(s.getText(sf), s0, childIndent.length - L.ws(l).length, spans) });
    }

    if (!isFile) {
      const first = startOf(st[0]);
      const last = st.at(-1).end;

      if (ln(first) === ln(open))
      {
        if (!hasComment(text.slice(open + 1, first)))
          edits.push({ start: open + 1, end: first, text: '\n' + childIndent });
      }
      else
      {
        const e = reGap(open + 1, first, existingBlank(open + 1, first), false, childIndent);

        if (e)
          edits.push(e);
      }

      if (ln(last - 1) !== ln(close)) {
        const e = reGap(last, close, false, true, L.ws(ln(open)));

        if (e)
          edits.push(e);
      } else if (!hasComment(text.slice(last, close)))
      {
        edits.push({ start: last, end: close, text: '\n' + L.ws(ln(open)) });
      }
    }

    for (let i = 1; i < st.length; i++)
    {
      const a = st[i - 1];
      const z = st[i];
      const gs = a.end;
      const ge = startOf(z);
      const blank = bigBlock(a) || bigBlock(z) || (!isFile && step(a, z));

      if (ln(a.end - 1) === ln(ge))
      {
        if (!hasComment(text.slice(gs, ge)))
          edits.push({ start: gs, end: ge, text: '\n' + (blank ? '\n' : '') + childIndent });

        continue;
      }

      const e = reGap(gs, ge, blank || hasComment(text.slice(gs, ge)) || existingBlank(gs, ge), false, childIndent);

      if (e)
        edits.push(e);
    }
  });

  // Own-line brace for an if / else / loop / switch / named function whose header spans lines or whose header or direct body line is 70+.
  const directLines = (block, o, c) => {
    const lines = new Set();

    for (let l = ln(o) + 1; l < ln(c); l++)
      lines.add(l);

    const drop = (n) => {
      if (n !== block && ([K.Block, K.CaseBlock, K.ClassDeclaration].includes(n.kind) || isLiteral(n)))
        for (let l = ln(startOf(n)) + 1; l < ln(n.end - 1); l++)
          lines.delete(l);

      ts.forEachChild(n, drop);
    };

    ts.forEachChild(block, drop);

    return [...lines];
  };

  const ownLine = new Set();

  const brace = (headerStart, block) => {
    const o = startOf(block);
    const c = block.end - 1;

    if (ln(o) === ln(c))
      return;

    const before = text.slice(0, o);
    const gapStart = before.length - /\s*$/.exec(before)[0].length;
    const long = (l) => L.lineText(l).length >= BRACE_LINE;

    if (/\n/.test(text.slice(gapStart, o))) {
      ownLine.add(block);

      return;
    }

    if (!(ln(headerStart) !== ln(o) || long(ln(o)) || directLines(block, o, c).some(long)))
      return;

    edits.push({ start: gapStart, end: o, text: '\n' + L.ws(ln(headerStart)) });
    ownLine.add(block);
  };

  const elseOf = (n) => n.getChildren(sf).find((c) => c.kind === K.ElseKeyword);

  walk(sf, (n) => {
    if (n.kind === K.IfStatement) {
      if (n.thenStatement.kind === K.Block)
        brace(startOf(n), n.thenStatement);

      if (n.elseStatement?.kind === K.Block)
        brace(startOf(elseOf(n)), n.elseStatement);
    } else if (LOOPS.includes(n.kind) && n.statement.kind === K.Block)
    {
      brace(startOf(n), n.statement);
    }
    else if (n.kind === K.SwitchStatement) {
      brace(startOf(n), n.caseBlock);
    } else if ((n.kind === K.FunctionDeclaration || (n.kind === K.MethodDeclaration && n.parent.kind === K.ClassDeclaration)) && n.body)
    {
      brace(startOf(n), n.body);
    }
  });

  // "} else" after an own-line block puts else on the next line.
  walk(sf, (n) => {
    if (n.kind === K.IfStatement && n.elseStatement && ownLine.has(n.thenStatement) && ln(n.thenStatement.end - 1) === ln(startOf(elseOf(n))))
      edits.push({ start: n.thenStatement.end, end: startOf(elseOf(n)), text: '\n' + L.ws(ln(startOf(n))) });
  });

  walk(sf, (n) => {
    if (n.kind === K.Block && (n.parent.kind === K.Block || n.parent.kind === K.SourceFile))
      report.push(`${name}:${ln(startOf(n)) + 1}: bare block`);
  });

  return applyEdits(text, edits)[0];
}

// ---------- run ----------

// Repeats a pass until it changes nothing (a fixed-point loop), at most n times.
function settle(pass, name, text, n = 10) {
  for (let i = 0, count = 1; i < n && count; i++)
    [text, count] = pass(name, text);

  return text;
}

function formatRound(name, src)
{
  let out = ternaryPass(name, splitDeclsPass(name, commentPass(name, src)));

  for (const pass of [expandBlocksPass, bodiesPass, literalsPass, multiLiteralsPass, bodyIndentPass])
    out = settle(pass, name, out);

  // Layout repeats until nothing moves: a block that grows can change what its parent needs.
  for (let i = 0; i < 12; i++) {
    const kept = report.length;
    const next = layoutPass(name, out);

    if (next === out)
      break;

    report.length = kept;
    out = next;
  }

  return out.replace(/[ \t]+$/gm, '');
}

// Rounds of every pass until one changes nothing: layout shifts can lengthen lines that the literal passes then break.
function format(name, src) {
  let out = src;

  for (let round = 0; round < 8; round++) {
    const kept = report.length;
    const next = formatRound(name, out);

    if (next === out)
      break;

    report.length = kept;
    out = next;
  }

  return out;
}

const files = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const targets = files.length ? files.map((f) => resolve(f)) : webFiles(join(repo, 'tools/web'));
let changed = 0;

for (const f of targets)
{
  const rel = relative(repo, f).replace(/\\/g, '/');
  const orig = readFileSync(f, 'utf8');
  const src = orig.replace(/\r\n/g, '\n');
  const out = format(rel, src);

  if (shape(rel, src) !== shape(rel, out))
  {
    const bad = join(tmpdir(), 'jsfmt_' + rel.replace(/\//g, '_'));

    writeFileSync(bad, out);

    throw new Error(`${rel}: the layout pass changed the code (its output is in ${bad})`);
  }

  if (literalTexts(rel, src) !== literalTexts(rel, out))
    throw new Error(`${rel}: a string or template changed`);

  if (commentWords(rel, src) !== commentWords(rel, out))
    throw new Error(`${rel}: comment words changed`);

  if (parse(rel, out).parseDiagnostics.length)
    throw new Error(`${rel}: no longer parses`);

  const kept = report.length;
  const again = format(rel, out);

  report.length = kept;

  if (again !== out)
    report.push(`${rel}: not stable (a second run changes it)`);

  const final = orig.includes('\r\n') ? out.replace(/\n/g, '\r\n') : out;

  if (final === orig)
    continue;

  changed++;
  console.log(`${rel}: ${final.split('\n').length - orig.split('\n').length >= 0 ? '+' : ''}${final.split('\n').length - orig.split('\n').length} lines`);

  if (WRITE) {
    writeFileSync(f + '.jsfmt-tmp', final);

    if (readFileSync(f + '.jsfmt-tmp', 'utf8') !== final)
      throw new Error(`${rel}: temp write mismatch`);

    renameSync(f + '.jsfmt-tmp', f);
  }
}

for (const r of new Set(report))
  console.log('  ' + r);

console.log(`${changed} of ${targets.length} files ${WRITE ? 'rewritten' : 'would change'}`);

if (CHECK && changed)
  process.exit(1);
