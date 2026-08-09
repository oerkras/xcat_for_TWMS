/**
 * Classic TWMS — sample LiveValue HackLog family at runtime.
 *
 * Product: Maplestory_Classic.exe (NOT fengxing / msw).
 * Hooks CFF thunks that resolve to GetInt(id); default arg ecx=0.
 *
 * Usage (game must be running):
 *   frida -n Maplestory_Classic.exe -l scripts/frida-livevalue-hacklog-family.js
 *   frida -p <pid> -l scripts/frida-livevalue-hacklog-family.js
 *
 * RVAs from Dumps/runtime/FINDMOB_INRECT_HACKLOG_FAMILY_20260809.md (GA remount).
 * If attach fails after client patch, re-check thunk RVAs in IDA.
 */
'use strict';

const kGa = 'GameAssembly.dll';

/** @type {Record<string, number>} */
const kThunkRva = {
  MobHackLogDisconnectCount_408: 0x1715e70,
  ShootObjectHackLogAttackFail_409: 0x17161e0,
  FindMobInRectHackLogDisconnectCount_411: 0x1716760,
  TargetMobInspect_InflateRectVal_415: 0x17173a0,
  ShootObjectHackLogAttackFail2_523: 0x172b1a0,
  MobNotMoveHackCheckValue_525: 0x172b750,
  MobPullingHack_926: 0x1774c90,
  MobPullingHackThreshold_927: 0x1774fe0,
  MobPullingHackKick_928: 0x1775340,
};

const kBuilderRva = {
  FindMobInRectLogBuilder_75C170: 0xadc170,
  MobHackLogHub_975000: 0xcf5000,
  Parent_759420: 0xad9420,
};

function gaBase() {
  const m = Process.findModuleByName(kGa);
  if (!m) throw new Error(kGa + ' not loaded');
  return m.base;
}

function hookThunk(name, rva) {
  const addr = gaBase().add(rva);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.defv = args[0].toInt32(); // ecx default
    },
    onLeave(retval) {
      const v = retval.toInt32();
      console.log(
        '[LV] ' + name + ' default=' + this.defv + ' -> ' + v +
          (v === this.defv || v === 0 ? ' (likely unset/sentinel)' : ' (TABLE HIT?)')
      );
    },
  });
  console.log('[hook] ' + name + ' @ ' + addr);
}

function hookBuilder(name, rva) {
  const addr = gaBase().add(rva);
  Interceptor.attach(addr, {
    onEnter(args) {
      console.log(
        '[CALL] ' + name +
          ' rcx=' + args[0] +
          ' edx=' + args[1].toInt32() +
          ' r8=' + args[2] +
          ' r9=' + args[3].toInt32()
      );
    },
    onLeave(retval) {
      console.log('[RET ] ' + name + ' al=' + (retval.toInt32() & 0xff));
    },
  });
  console.log('[hook] ' + name + ' @ ' + addr);
}

function main() {
  console.log('[*] GA base ' + gaBase());
  for (const [n, rva] of Object.entries(kThunkRva)) hookThunk(n, rva);
  for (const [n, rva] of Object.entries(kBuilderRva)) hookBuilder(n, rva);
  console.log('[*] ready — enter field / fight; watch LV + CALL lines');
}

setImmediate(main);
