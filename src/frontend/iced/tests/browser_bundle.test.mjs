import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';

const wasm = 'app-0123456789abcdef_bg.wasm';
const marker = 'MMLTK_HOST_API_PROTOCOL_17';
const index = `<link href="/${wasm}"><script>init({module_or_path: '/${wasm}'});</script>`;

function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'mmltk-browser-bundle-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const staged = join(root, 'staged'), current = join(root, 'current'), protocol = join(root, 'protocol');
  mkdirSync(staged);
  writeFileSync(protocol, marker);
  writeFileSync(join(staged, 'index.html'), index);
  writeFileSync(join(staged, wasm), marker);
  return {staged, current, protocol, run(mode, ...paths) {
    const result = spawnSync(process.env.MMLTK_BROWSER_BUNDLE_PUBLISHER, [mode, ...paths, protocol], {encoding: 'utf8'});
    assert.ifError(result.error);
    assert.equal(result.signal, null);
    return result;
  }};
}

test('browser bundle publishes one matching artifact through comments and atomic replacement', t => {
  const f = fixture(t);
  writeFileSync(join(f.staged, 'index.html'), `<!-- a quoted ' comment -->
<script>
// Iced's actual window observations
/* another " comment */
</script>${index}`);
  mkdirSync(f.current);
  writeFileSync(join(f.current, 'previous'), 'retained until publication');
  const published = f.run('publish', f.staged, f.current);
  assert.equal(published.status, 0, published.stderr);
  assert.equal(existsSync(f.staged), false);
  assert.equal(existsSync(join(f.current, 'previous')), false);
  assert.equal(readFileSync(join(f.current, wasm), 'utf8'), marker);
  const verified = f.run('verify', f.current);
  assert.equal(verified.status, 0, verified.stderr);
});

test('browser bundle rejects invalid selections before replacing the current distribution', async t => {
  const cases = {
    'missing reference': f => writeFileSync(join(f.staged, 'index.html'), '<html></html>'),
    'unquoted reference': f => writeFileSync(join(f.staged, 'index.html'), `<link href=/${wasm}>`),
    'escaped reference': f => writeFileSync(join(f.staged, 'index.html'), `<link href="\\/${wasm}">`),
    'conflicting references': f => writeFileSync(join(f.staged, 'index.html'), `${index}<link href="/other-0123456789abcdef_bg.wasm">`),
    'network path': f => writeFileSync(join(f.staged, 'index.html'), `<link href="//${wasm}">`),
    'parent path': f => writeFileSync(join(f.staged, 'index.html'), `<link href="../${wasm}">`),
    'multiple artifacts': f => writeFileSync(join(f.staged, 'extra.wasm'), marker),
    'wrong protocol': f => writeFileSync(join(f.staged, wasm), 'MMLTK_HOST_API_PROTOCOL_16'),
    'duplicate protocol': f => writeFileSync(join(f.staged, wasm), `${marker}\0${marker}`),
    'symlink artifact': f => {
      rmSync(join(f.staged, wasm));
      symlinkSync(f.protocol, join(f.staged, wasm));
    },
  };
  for (const [name, invalidate] of Object.entries(cases)) {
    await t.test(name, t => {
      const f = fixture(t);
      mkdirSync(f.current);
      writeFileSync(join(f.current, 'previous'), 'healthy');
      invalidate(f);
      const rejected = f.run('publish', f.staged, f.current);
      assert.equal(rejected.status, 1);
      assert.match(rejected.stderr, /mmltk-browser-bundle-contract:/);
      assert.equal(readFileSync(join(f.current, 'previous'), 'utf8'), 'healthy');
      assert.equal(existsSync(f.staged), true);
    });
  }
});
