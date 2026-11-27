// Self-check of the calibration fit in data/www/dsp.html (run: node scripts/check_dsp_fit.js).
// Measures the page's synthetic room (DSP.simulate) and asserts that both
// correction scopes bring it closer to the target without boosting below the floor.
const fs = require('fs');
const src = fs.readFileSync(__dirname + '/../data/www/dsp.html', 'utf8');
const DSP = new Function(src.slice(src.indexOf('/*DSP-START*/'), src.indexOf('/*DSP-END*/')) + '; return DSP;')();
const FS = 44100, grid = DSP.logGrid(20, 20000, 320);
const sw = DSP.sweep(FS), d = DSP.deconvolve(DSP.simulate(FS, 0, true), sw, FS);
const meas = DSP.smoothGrid(DSP.normalize(DSP.toGrid(d.P, d.M, FS, grid, 24), grid), grid, f => DSP.varFrac(f, 1));
const opt = { fs: FS, fmin: 40, fmax: 10000, roomHi: 500, tilt: 2, treble: 1, maxN: 10, maxCut: 12, maxBoost: 4, floor: 120 };
const tg = grid.map(f => DSP.target(f, opt.tilt, opt.treble));
// peaks (fine) everywhere, dips (half-octave) above the floor: what the fit minimizes
const dev = (arr, floor) => { const sm = DSP.smoothGrid(arr, grid, 2); let s = 0, n = 0;
  grid.forEach((f, i) => { if (f < 40 || f > 10000) return; const up = Math.max(0, arr[i] - tg[i]), dn = f >= floor ? Math.min(0, sm[i] - tg[i]) : 0; s += up * up + dn * dn; n++; });
  return Math.sqrt(s / n); };
for (const speaker of [false, true]) {
  const fl = DSP.autoFit(meas, grid, { ...opt, speaker }), eq = DSP.eqCurve(fl, grid, FS);
  const before = dev(meas, opt.floor), after = dev(meas.map((v, i) => v + eq[i]), opt.floor);
  console.log((speaker ? 'room+speaker' : 'room only   '), fl.length, 'filters', before.toFixed(2), '->', after.toFixed(2), 'dB');
  console.log('  ' + fl.map(f => Math.round(f.f) + 'Hz ' + (f.gain > 0 ? '+' : '') + f.gain + ' Q' + f.q).join(' | '));
  console.assert(fl.length <= opt.maxN, "too many filters");
  console.assert(Math.max(...eq) <= opt.maxBoost + 0.3, "boost above maxBoost in sum: " + Math.max(...eq).toFixed(1));
  console.assert(after < before, 'no improvement');
  if (!speaker) console.assert(fl.every(f => f.gain < 0 && f.f <= 500 * 1.2), 'room only: cuts up to 500 Hz only');
  else console.assert(fl.every(f => f.gain <= 0 || f.f >= opt.floor), 'a boost below the floor');
  // a boost must not lift the response below the floor by more than 1 dB
  if (speaker) grid.forEach((f, i) => { if (f < opt.floor / 1.5 && f > 30) console.assert(eq[i] < 1, 'lift at ' + Math.round(f) + ' Hz: ' + eq[i].toFixed(1)); });
}
// A speaker with a broad +5 dB presence peak at 2.5 kHz: the speaker pass
// must find it with one broad cut, room only must leave it alone.
const hump = grid.map(f => 5 * Math.exp(-((Math.log2(f / 2500)) ** 2) / (2 * 0.5 ** 2)));
const meas2 = meas.map((v, i) => v + hump[i]);
const sp = DSP.autoFit(meas2, grid, { ...opt, speaker: true }), ro = DSP.autoFit(meas2, grid, { ...opt, speaker: false });
console.log('presence peak: ' + sp.filter(f => f.f > 1000).map(f => Math.round(f.f) + 'Hz ' + f.gain + ' Q' + f.q).join(' | '));
console.assert(sp.some(f => f.f > 1500 && f.f < 4000 && f.gain < -2 && f.q <= 1.5), 'presence peak not corrected');
console.assert(ro.every(f => f.f < 1000), 'room only touched the treble');
console.log('ok');
