const form = document.querySelector("#form");
const input = document.querySelector("#input");
const version = document.querySelector("#version");
const button = document.querySelector("#build");
const status = document.querySelector("#result");
const description = document.querySelector("#build-description");
const heroAnimation = document.querySelector("#lamp-intro");
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
    description: "Full DOOM OS 0.7.4 and SP-ETC v9.53 with all Genie modes, Screen Genie USB mirror & remote control, and the Cheddar Butt startup. Drum Genie has eight lanes.",
  },
  stock: {
    profile: "build-03",
    description: "Roland 5.52 with all Genie modes, Screen Genie USB mirror & remote control, and the Cheddar Butt startup. Drum Genie has eight lanes.",
  },
  legacy: {
    build: buildLegacy,
    label: "Legacy DOOM OS 0.6.1 + Sequence Genie",
    description: "DOOM OS 0.6.1 and SP-ETC v9.53 with Sequence Genie only. Also accepts the exact SP-ETC v9.53 APP1.",
  },
};

async function buildPreset(source, inputHash, profile, depth = 0) {
  if (profile.parent) {
    const parent = catalog.profiles.find(entry => entry.id === profile.parent);
    if (!parent || depth >= 2) throw new Error("Invalid parent chain for the firmware patch.");
    const image = await buildPreset(source, inputHash, parent, depth + 1);
    return applyContainer(image, {...profile, progress: "Applying startup artwork…"});
  }
  if (inputHash !== stockHash) throw new Error("This build needs the original Roland 5.52 APP1.");
  if (profile.recipe === "patch") {
    return applyContainer(source, {...profile, progress: `Building ${profile.label}…`});
  }
  throw new Error("Unknown firmware build recipe.");
}

function updateSelection() {
  version.disabled = input.disabled = busy;
  const preset = builds[version.value];
  const profile = catalog?.profiles.find(entry => entry.id === preset.profile);
  selection = preset.profile ? profile && {...profile, build: buildPreset} : preset;
  description.textContent = preset.profile && !profile && catalogError ? catalogError : preset.description;
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
    const response = await fetch(new URL("./firmware-catalog.json?v=20261127-panel", import.meta.url));
    if (!response.ok) throw new Error();
    const result = await response.json();
    if (result.version !== 12 || !Array.isArray(result.profiles)) throw new Error();
    catalog = result;
  } catch {
    catalogError = "The firmware catalog could not be loaded. Reload the page to retry.";
  } finally {
    loadingCatalog = false;
    updateSelection();
  }
}

version.addEventListener("change", changeSelection);
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
    if (selected.bytes && (output.length !== selected.bytes || await sha256(output) !== selected.hash)) {
      throw new Error("The output does not match the catalog size or checksum.");
    }
    const url = URL.createObjectURL(new Blob([output], {type: "application/octet-stream"}));
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
