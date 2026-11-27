const form = document.querySelector("#form");
const input = document.querySelector("#input");
const version = document.querySelector("#version");
const button = document.querySelector("#build");
const status = document.querySelector("#result");
const description = document.querySelector("#build-description");
const heroAnimation = document.querySelector("#lamp-intro");
const modules = document.querySelector("#modules");
const base = document.querySelector("#base");
const spetc = document.querySelector("#spetc");
const genie = document.querySelector("#genie");
const portrait = document.querySelector("#portrait");
const portraitOption = document.querySelector("#portrait-option");
const space = document.querySelector("#firmware-space");
const meter = document.querySelector("#firmware-meter");
const size = document.querySelector("#firmware-size");
const deviceOLED = document.querySelector("#device-oled");
const doomBoot = document.querySelector("#doomBoot");
const doomHyperspace = document.querySelector("#doomHyperspace");
const doomHelp = document.querySelector("#doomHelp");
const noteRepeat = document.querySelector("#noteRepeat");
const styleOptions = document.querySelector("#doom-styles-option");
const styleChecks = [...styleOptions.querySelectorAll("input")];
const allStyles = (1 << styleChecks.length) - 1;
const chromatic = document.querySelector("#chromatic");
const chromaticOption = document.querySelector("#chromatic-option");
const lamp = document.querySelector("#genieLamp");
const lampOption = document.querySelector("#lamp-option");
const introOptions = document.querySelector("#intro-options");
const introChecks = [...introOptions.querySelectorAll("input")];
const workflowOptions = document.querySelector("#workflow-options");
const workflowChecks = [...workflowOptions.querySelectorAll("input")];
const addonOptions = document.querySelector("#addon-options");
const addonChecks = [...addonOptions.querySelectorAll("input")];
const doomBootOption = document.querySelector("#doom-boot-option");
const doomHyperspaceOption = document.querySelector("#doom-hyperspace-option");
const doomHelpOption = document.querySelector("#doom-help-option");
const doomFeatureOptions = document.querySelector("#doom-feature-options");
const doomFeatureChecks = [...doomFeatureOptions.querySelectorAll("input")];
const noteRepeatOption = document.querySelector("#note-repeat-option");
const spetcFeatureOptions = document.querySelector("#spetc-feature-options");
const spetcFeatureChecks = [...spetcFeatureOptions.querySelectorAll("input")];
const released = document.querySelector("#firmware-released");
const modeOptions = document.querySelector("#genie-mode-options");
const modeChecks = [...modeOptions.querySelectorAll("input")];
const allModes = (1 << modeChecks.length) - 1;
const suiteMarker = 0x47534d00;
let catalog;
let catalogError = "";
let loadingCatalog = false;
let busy = false;
let selection;

if (heroAnimation && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
  heroAnimation.play().catch(() => {});
}

async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, byte => byte.toString(16).padStart(2, "0")).join("");
}

const stockHash = "4a3d67711e14dcc97d50249a4eee7dd6df0251a2f37757cbbe2556c233730d80";
const etc953Hash = "faeea5b004273f2dbcfca00387cfcc59078d3ba42fd832d52d363209d8b3b75e";
const legacyHash = "b60e4cfba4c018b6719c3ade14c74ae35094d8ffa7c42408a2c8f41de6815ef5";

function patchURL(file, digest) {
  return new URL(`./${file}?v=${digest.slice(0, 8)}`, import.meta.url);
}

function report(message, state = "") {
  status.textContent = message;
  status.dataset.state = state;
}

async function applyContainer(source, {label, file, hash, progress}) {
  report(progress);
  const response = await fetch(patchURL(file, hash));
  if (!response.ok) throw new Error(`The ${label} patch could not be loaded.`);
  const container = new Uint8Array(await response.arrayBuffer());
  const {bytes, digest} = window.applyPatch(container, source);
  if (digest !== hash) throw new Error(`The ${label} build checksum did not match.`);
  return bytes;
}

function legacyBase(source, withSPETC) {
  const base = window.applyPatch(window.fromBase64(window.DOOMOS_PATCH), source);
  return withSPETC
    ? window.applyPatch(window.fromBase64(window.DOOMOS_KITS_PATCH), base.bytes).bytes
    : base.bytes;
}

async function buildLegacy(parent, inputHash) {
  if (inputHash !== etc953Hash && inputHash !== stockHash) {
    throw new Error("Select the original Roland 5.52 APP1 or exact SP-ETC v9.53 APP1.");
  }
  const response = await fetch(patchURL("genie-patch.json", legacyHash));
  if (!response.ok) throw new Error("The legacy patch could not be loaded.");
  const patch = await response.json();
  if (patch.input !== etc953Hash || patch.output !== legacyHash) {
    throw new Error("The legacy patch checksum did not match.");
  }
  if (inputHash === stockHash) {
    report("Applying SP-ETC v9.53…");
    parent = legacyBase(parent, true);
    if (await sha256(parent) !== patch.input) throw new Error("SP-ETC checksum mismatch.");
  }
  return applyOverlay(parent, patch);
}

async function applyOverlay(parent, patch) {
  const output = new Uint8Array(patch.size);
  output.set(parent);
  for (const [offset, hex] of patch.segments) {
    const bytes = Uint8Array.from(hex.match(/.{2}/g) ?? [], byte => parseInt(byte, 16));
    output.set(bytes, offset);
  }
  if (await sha256(output) !== patch.output) throw new Error("Output checksum mismatch.");
  return output;
}

const builds = {
  "074": {
    profile: "build-08",
    description: "Full DOOM OS 0.7.4 and SP-ETC v9.53 with all Genie modes and the Cheddar Butt startup. Drum Genie has eight lanes.",
  },
  stock: {
    profile: "build-03",
    description: "Roland 5.52 with all Genie modes and the Cheddar Butt startup. Drum Genie has eight lanes.",
  },
  legacy: {
    build: buildLegacy,
    label: "Legacy DOOM OS 0.6.1 + Sequence Genie",
    description: "DOOM OS 0.6.1 and SP-ETC v9.53 with Sequence Genie only. Also accepts the exact SP-ETC v9.53 APP1.",
  },
};

async function buildModules(source, inputHash, profile, depth = 0) {
  if (profile.parent) {
    const parent = catalog.profiles.find(entry => entry.id === profile.parent);
    if (!parent || depth >= 2) throw new Error("Invalid parent chain for the component patch.");
    const image = await buildModules(source, inputHash, parent, depth + 1);
    return applyContainer(image, {...profile, progress: "Applying the selected firmware components…"});
  }
  if (profile.recipe === "genie-legacy") return buildLegacy(source, inputHash);
  if (inputHash !== stockHash) throw new Error("This combination needs the original Roland 5.52 APP1.");
  if (profile.recipe === "stock") return source;
  if (profile.recipe === "doom-legacy" || profile.recipe === "spetc-legacy") {
    report(`Building ${profile.label}…`);
    return legacyBase(source, profile.recipe === "spetc-legacy");
  }
  if (profile.recipe === "patch") {
    return applyContainer(source, {...profile, progress: `Building ${profile.label}…`});
  }
  throw new Error("Unknown firmware build recipe.");
}

function omittedModeSpans(layout, mask) {
  return layout.slots.flatMap((slots, bit) => mask & (1 << bit) ? [] : slots);
}

function applyModes(output, {modes, modeMask}) {
  if (!modes || modeMask === allModes) return;
  const view = new DataView(output.buffer, output.byteOffset, output.byteLength);
  if (view.getUint32(modes.offset, true) !== (suiteMarker | allModes)) {
    throw new Error("The Suite mode configuration did not match.");
  }
  view.setUint32(modes.offset, suiteMarker | modeMask, true);
  for (const [offset, length] of omittedModeSpans(modes, modeMask)) {
    if (offset < 0 || length <= 0 || offset + length > output.length) {
      throw new Error("Invalid Suite mode payload bounds.");
    }
    output.fill(0, offset, offset + length);
  }
}

function applyComponents(output, {components, componentMask}) {
  for (const [index, component] of (components?.edits ?? []).entries()) {
    if (!(componentMask & (1 << index))) continue;
    for (const [offset, hex] of component.writes) {
      output.set(Uint8Array.from(hex.match(/../g), byte => parseInt(byte, 16)), offset);
    }
    for (const [offset, length] of (component.zeros ?? [component.zero])) {
      if (offset < 0 || length <= 0 || offset + length > output.length) {
        throw new Error("Invalid firmware component bounds.");
      }
      output.fill(0, offset, offset + length);
    }
  }
}

async function applyDecorations(output, {styleVariant, styleSet, removeLamp}) {
  if (styleVariant && styleSet) {
    const {offset, frames: [frameOffset, size], hash, reserve} = styleSet;
    const table = Uint8Array.from(styleVariant.table.match(/../g), byte => parseInt(byte, 16));
    const bank = output.subarray(frameOffset, frameOffset + size);
    const sliceBank = () => {
      if (!reserve) return bank;
      const [resStart, resLen] = reserve;
      const out = new Uint8Array(size - resLen);
      out.set(bank.subarray(0, resStart), 0);
      out.set(bank.subarray(resStart + resLen), resStart);
      return out;
    };
    const initial = sliceBank();
    const checked = new Uint8Array(table.length + initial.length);
    checked.set(output.subarray(offset, offset + table.length));
    checked.set(initial, table.length);
    if (await sha256(checked) !== hash) throw new Error("The DOOM animation bank did not match.");
    for (const [start, length] of styleVariant.zero) {
      if (start < 0 || length <= 0 || start + length > size) {
        throw new Error("Invalid DOOM animation frame bounds.");
      }
      bank.fill(0, start, start + length);
    }
    checked.set(table);
    checked.set(sliceBank(), table.length);
    if (await sha256(checked) !== styleVariant.hash) throw new Error("The updated DOOM animation bank did not match.");
    output.set(table, offset);
  }
  if (removeLamp) {
    const {offset, size, hash} = catalog.decorations.lamp;
    if (await sha256(output.subarray(offset, offset + size)) !== hash) {
      throw new Error("The Sequence Genie lamp did not match.");
    }
    output.fill(0, offset, offset + size);
  }
}

async function applyOptionalPayloads(output, options = []) {
  for (const option of options) {
    for (const {offset, size, hash, replace} of option.changes) {
      const end = offset + size;
      if (offset < 0 || size <= 0 || end > output.length ||
          await sha256(output.subarray(offset, end)) !== hash) {
        throw new Error("An optional firmware component did not match.");
      }
      if (replace) {
        const bytes = Uint8Array.from(replace.match(/../g), byte => parseInt(byte, 16));
        if (bytes.length !== size) throw new Error("Invalid firmware replacement size.");
        output.set(bytes, offset);
      } else {
        output.fill(0, offset, end);
      }
    }
  }
}

function applyWorkflows(output, {modes, modeMask, workflowMask}) {
  if (!workflowMask) return;
  const view = new DataView(output.buffer, output.byteOffset, output.byteLength);
  if (view.getUint32(modes.offset, true) !== (suiteMarker | modeMask)) {
    throw new Error("The Suite workflow configuration did not match.");
  }
  view.setUint32(modes.offset, suiteMarker | modeMask | workflowMask, true);
}

function describeUnavailable() {
  if (base.value === "074" && genie.value === "suite" && !spetc.checked) {
    return "Full Genie Suite on DOOM OS 0.7.4 requires SP-ETC. Enable SP-ETC or choose Sequence only.";
  }
  if (base.value === "074" && genie.value === "core" && spetc.checked) {
    return "Sequence only is available on DOOM OS 0.7.4 with its native instruments. Clear SP-ETC or choose Full Genie Suite.";
  }
  if (base.value === "061") {
    return "On DOOM OS 0.6.1, choose Off or enable SP-ETC and choose Sequence only.";
  }
  return "This combination has no linked build.";
}

function updateSelection() {
  const custom = version.value === "custom";
  modules.hidden = !custom;
  modules.disabled = !custom || busy;
  version.disabled = input.disabled = busy;
  spetc.disabled = base.value === "stock";
  doomBootOption.hidden = doomBoot.disabled = base.value !== "074";
  doomHelpOption.hidden = doomHelp.disabled = doomBoot.disabled;
  doomFeatureOptions.hidden = doomBoot.disabled;
  for (const check of doomFeatureChecks) check.disabled = doomFeatureOptions.hidden;
  doomHyperspaceOption.hidden = doomHyperspace.disabled = doomBoot.disabled || !doomBoot.checked;
  noteRepeatOption.hidden = noteRepeat.disabled = spetc.disabled || !spetc.checked;
  spetcFeatureOptions.hidden = noteRepeat.disabled;
  for (const check of spetcFeatureChecks) check.disabled = spetcFeatureOptions.hidden;
  styleOptions.hidden = styleOptions.disabled = doomHyperspace.disabled;
  chromaticOption.hidden = chromatic.disabled = noteRepeat.disabled;
  modeOptions.hidden = modeOptions.disabled = genie.value !== "suite";
  lampOption.hidden = lamp.disabled = modeOptions.disabled;
  addonOptions.hidden = addonOptions.disabled = modeOptions.disabled;
  for (const check of addonChecks) check.disabled = addonOptions.disabled;
  workflowOptions.hidden = modeOptions.disabled;
  for (const check of introChecks) {
    check.disabled = modeOptions.disabled || !modeChecks[Number(check.dataset.mode)].checked;
    check.closest("label").hidden = check.disabled;
  }
  introOptions.hidden = introChecks.every(check => check.disabled);
  portraitOption.hidden = portrait.disabled = base.value === "061";
  space.hidden = true;
  if (custom) {
    const choices = {
      base: base.value,
      spetc: !spetc.disabled && spetc.checked,
      genie: genie.value,
      portrait: !portrait.disabled && portrait.checked,
      doomBoot: !doomBoot.disabled && doomBoot.checked,
      doomHelp: !doomHelp.disabled && doomHelp.checked,
      doomHyperspace: !doomHyperspace.disabled && doomHyperspace.checked,
      noteRepeat: !noteRepeat.disabled && noteRepeat.checked,
    };
    const profile = catalog?.profiles.find(entry =>
      Object.entries(choices).every(([name, value]) => entry[name] === value));
    const addonsReserve = Boolean(profile?.addonsReserve);
    styleChecks[0].disabled = styleOptions.disabled || addonsReserve;
    const modeMask = modeChecks.reduce((mask, input, i) => mask | (input.checked ? 1 << i : 0), 0);
    const styleMask = styleOptions.disabled ? 0 : styleChecks.reduce((mask, input, i) =>
      mask | ((addonsReserve && i === 0) || !input.checked ? 1 << i : 0), 0);
    const componentMask = (styleMask === allStyles ? 1 : 0) | (!chromatic.disabled && !chromatic.checked ? 2 : 0);
    const styleSet = addonsReserve ? catalog?.decorations.stylesReserved : catalog?.decorations.styles;
    const styleVariant = styleSet?.variants[styleMask];
    const removeLamp = !lamp.disabled && !lamp.checked;
    const optionalPayloads = profile?.modes ? introChecks
      .filter(check => !check.disabled && !check.checked)
      .map(check => profile.modes.optional[check.id]) : [];
    const omittedAddons = profile?.modes ? addonChecks
      .filter(check => !check.disabled && !check.checked && profile.modes.optional[check.id]) : [];
    optionalPayloads.push(...omittedAddons.map(check => profile.modes.optional[check.id]));
    const omittedDoomTools = doomFeatureChecks.filter(check => !check.disabled && !check.checked);
    const omittedSpetcTools = spetcFeatureChecks.filter(check => !check.disabled && !check.checked);
    if (profile) {
      optionalPayloads.push(...omittedDoomTools.map(check => catalog.doomFeatures[check.id]));
      optionalPayloads.push(...omittedSpetcTools.map(check => catalog.spetcFeatures[profile.base][check.id]));
      if (profile.withoutInstrumentSources && omittedDoomTools.some(check => check.id === "doomInstruments")) {
        optionalPayloads.push(profile.withoutInstrumentSources);
      }
    }
    const workflowMask = profile?.modes ? workflowChecks.reduce((mask, check) =>
      mask | (check.checked ? 0 : Number(check.value)), 0) : 0;
    selection = profile && {
      ...profile, modeMask, componentMask, styleVariant, styleSet, removeLamp, optionalPayloads, workflowMask, build: buildModules,
    };
    deviceOLED.textContent = profile
      ? {suite: "GENIE SUITE", core: "SEQUENCE GENIE", none: base.value === "stock" ? "ROLAND 5.52" : "DOOM OS"}[genie.value]
      : catalog ? "CHECK BUILD" : "APP1";
    const changes = [];
    if (styleMask && catalog) {
      const names = styleSet.order.filter((_, i) => styleMask & (1 << i) && !(addonsReserve && i === 0));
      if (names.length) changes.push(`${names.join(", ")} ${names.length === 1 ? "uses" : "use"} VHS.`);
    }
    if (componentMask & 2) changes.push("Sample chromatic mode uses the baseline controls.");
    if (removeLamp) changes.push("Sequence Genie opens directly to Setup.");
    if (omittedAddons.some(check => check.id === "genieScramble")) changes.push("Scramble is off.");
    if (omittedAddons.some(check => check.id === "genieDisplay")) changes.push("Screen Genie USB mirror and remote control are off.");
    if (workflowMask & 128) changes.push("Bounce records one take at a time.");
    if (omittedDoomTools.some(check => check.id === "doomShortcuts")) changes.push("Custom shortcuts are disabled.");
    if (omittedDoomTools.some(check => check.id === "doomArp")) changes.push("The DOOM arpeggiator is disabled.");
    if (omittedDoomTools.some(check => check.id === "doomInstruments")) {
      changes.push("DOOM Instruments and Genie instrument playback are disabled.");
    }
    if (omittedDoomTools.some(check => check.id === "doomBounce")) changes.push("DOOM Bounce in Place is disabled.");
    if (omittedDoomTools.some(check => check.id === "doomSidechain")) changes.push("The DOOM Sidechain panel is disabled.");
    if (omittedDoomTools.some(check => check.id === "doomWaveform")) changes.push("The DOOM Waveform Display is disabled.");
    if (omittedSpetcTools.length) changes.push(`SP-ETC features off · ${omittedSpetcTools.map(check => check.value).join(", ")}`);
    description.textContent = profile ? [profile.detail, ...changes].join(" ") :
      (catalog ? describeUnavailable() : catalogError || "Loading firmware combinations…");
    if (profile) {
      const count = new Intl.NumberFormat();
      meter.max = catalog.limit;
      meter.value = profile.bytes;
      size.textContent = `${count.format(profile.bytes)} bytes · ${count.format(catalog.limit - profile.bytes)} bytes before the settings sector`;
      const componentBytes = (profile.released ?? []).reduce((total, span) => total + span.bytes, 0);
      const modeBytes = profile.modes
        ? omittedModeSpans(profile.modes, modeMask).reduce((total, [, length]) => total + length, 0)
        : 0;
      const extraBytes = (profile.components?.edits ?? []).reduce((total, component, i) =>
        total + (componentMask & (1 << i)
          ? (component.zeros ?? [component.zero]).reduce((sum, [, length]) => sum + length, 0)
          : 0), 0);
      const styleBytes = (styleVariant?.zero ?? []).reduce((total, [, length]) => total + length, 0);
      const lampBytes = removeLamp ? catalog.decorations.lamp.size : 0;
      const optionalBytes = optionalPayloads.flatMap(option => option.changes).reduce(
        (total, edit) => total + (edit.replace ? 0 : edit.size), 0);
      const reclaimed = componentBytes + modeBytes + extraBytes + styleBytes + lampBytes + optionalBytes;
      released.hidden = !reclaimed;
      released.textContent = reclaimed
        ? `${count.format(reclaimed)} bytes reclaimed inside APP1 for future modules.`
        : "";
      space.hidden = false;
    }
  } else {
    const preset = builds[version.value];
    const profile = catalog?.profiles.find(entry => entry.id === preset.profile);
    selection = preset.profile ? profile && {...profile, modeMask: allModes, build: buildModules} : preset;
    description.textContent = preset.profile && !profile && catalogError ? catalogError : preset.description;
  }
  button.disabled = busy || !selection;
}

async function changeSelection() {
  report("");
  updateSelection();
  if (version.value === "legacy" || catalog || loadingCatalog) return;
  loadingCatalog = true;
  catalogError = "";
  updateSelection();
  try {
    const response = await fetch(new URL("./firmware-catalog.json?v=20261127-remote", import.meta.url));
    if (!response.ok) throw new Error();
    const result = await response.json();
    if (result.version !== 12 || !Array.isArray(result.profiles) || !Number.isInteger(result.limit) ||
        !result.decorations?.styles || !result.decorations?.stylesReserved || !result.decorations?.lamp ||
        !doomFeatureChecks.every(check => result.doomFeatures?.[check.id]?.changes.length) ||
        !["061", "074"].every(base => spetcFeatureChecks.every(check =>
          result.spetcFeatures?.[base]?.[check.id]?.changes.length))) throw new Error();
    catalog = result;
  } catch {
    catalogError = "The firmware catalog could not be loaded. Reopen Custom module selection to retry.";
  } finally {
    loadingCatalog = false;
    updateSelection();
  }
}
version.addEventListener("change", changeSelection);
modules.addEventListener("change", changeSelection);
base.addEventListener("change", () => {
  if (base.value === "stock") spetc.checked = false;
});
input.addEventListener("change", () => report(""));
changeSelection();

form.addEventListener("submit", async event => {
  event.preventDefault();
  const file = input.files[0];
  if (!file || !selection || busy) return;
  const selected = selection;
  busy = true;
  updateSelection();
  report("Checking firmware…");
  try {
    const source = new Uint8Array(await file.arrayBuffer());
    const inputHash = await sha256(source);
    const output = await selected.build(source, inputHash, selected);
    applyModes(output, selected);
    applyComponents(output, selected);
    const digest = selected.componentMask
      ? selected.components.hashes[selected.componentMask][selected.modes ? selected.modeMask : 0]
      : selected.modes ? selected.modes.hashes[selected.modeMask] : selected.hash;
    if (selected.bytes && (output.length !== selected.bytes || await sha256(output) !== digest)) {
      throw new Error("The output does not match the catalog size or checksum.");
    }
    await applyDecorations(output, selected);
    await applyOptionalPayloads(output, selected.optionalPayloads);
    applyWorkflows(output, selected);
    const url = URL.createObjectURL(new Blob([output], {type:"application/octet-stream"}));
    const link = document.createElement("a");
    link.href = url;
    link.download = "SP404MKII_APP1.bin";
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    report(`Verified ${selected.label}. APP1 download started.`, "done");
  } catch (error) {
    report(error.message, "error");
  } finally {
    busy = false;
    updateSelection();
  }
});
