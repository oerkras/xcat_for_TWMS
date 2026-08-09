/**
 * Classic TWMS — probe Shoot ok vs Encode(melee) while pointblank_shoot is on.
 * frida -n Maplestory_Classic.exe -l scripts/frida-pbshoot-opcode-probe.js
 */
'use strict';

const kGa = 'GameAssembly.dll';
const kRva = {
  Shoot: 0x103d2a0,
  Ced: 0x106d7e0,
  Encode: 0xcd2690, // sub_7FF849CD2690 — CEB600 melee fallback
  CEB600: 0xceb600,
};

function ga() {
  const m = Process.findModuleByName(kGa);
  if (!m) throw new Error('GA missing');
  return m.base;
}

const counts = { shootEnter: 0, shootOk: 0, shootFail: 0, cedTrue: 0, cedFalse: 0, encode: 0 };

function main() {
  const base = ga();
  console.log('[*] GA ' + base);

  Interceptor.attach(base.add(kRva.Shoot), {
    onEnter(args) {
      counts.shootEnter++;
      this.mb = args[4].toInt32(); // isMortalBlow (5th: after rcx rdx r8 r9 → stack; Frida args[4])
      // Win64: args[0]=rcx ... args[3]=r9, args[4]=stack0 = isMortalBlow
    },
    onLeave(retval) {
      const v = retval.toInt32() & 0xff;
      if (v) counts.shootOk++;
      else counts.shootFail++;
      if (counts.shootEnter <= 8 || counts.shootEnter % 25 === 0) {
        console.log(
          '[Shoot] #' + counts.shootEnter + ' mb=' + this.mb + ' ret=' + v +
            ' ok=' + counts.shootOk + ' fail=' + counts.shootFail
        );
      }
    },
  });

  Interceptor.attach(base.add(kRva.Ced), {
    onLeave(retval) {
      const v = retval.toInt32() & 0xff;
      if (v) counts.cedTrue++;
      else counts.cedFalse++;
    },
  });

  Interceptor.attach(base.add(kRva.Encode), {
    onEnter(args) {
      counts.encode++;
      // CEB600: rcx=self rdx=0 r8=attackType(0=melee)
      console.log(
        '[Encode] #' + counts.encode +
          ' rdx=' + args[1].toInt32() +
          ' r8=' + args[2].toInt32() +
          ' (0≈Melee/op50 path)'
      );
    },
  });

  setInterval(function () {
    console.log(
      '[sum] shootEnter=' + counts.shootEnter +
        ' ok=' + counts.shootOk +
        ' fail=' + counts.shootFail +
        ' cedF=' + counts.cedFalse +
        ' cedT=' + counts.cedTrue +
        ' encode=' + counts.encode
    );
  }, 5000);

  console.log('[*] probing — fight at point-blank; watch Encode vs Shoot ok');
}

setImmediate(main);
