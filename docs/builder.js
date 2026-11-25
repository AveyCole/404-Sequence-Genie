const form = document.querySelector("#form");
const input = document.querySelector("#input");
const version = document.querySelector("#version");
const button = document.querySelector("#build");
const status = document.querySelector("#result");
const description = document.querySelector("#build-description");
const heroAnimation = document.querySelector("#lamp-intro");

if (heroAnimation && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
  heroAnimation.play().catch(() => {});
}

async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, byte => byte.toString(16).padStart(2, "0")).join("");
}

const stockHash = "4a3d67711e14dcc97d50249a4eee7dd6df0251a2f37757cbbe2556c233730d80";
const etc953Hash = "faeea5b004273f2dbcfca00387cfcc59078d3ba42fd832d52d363209d8b3b75e";
const combinedHash = "5877dccfb7fc40ff076ca4a0eb3afb82ab1b1dd44fb4649f883b7d01dcf88f52";
const stockGenieHash = "480decac2613a854864e45a2380de64995a2884553cd033aafa8e691a7016163";
const legacyHash = "b60e4cfba4c018b6719c3ade14c74ae35094d8ffa7c42408a2c8f41de6815ef5";

function patchURL(file, digest) {
  return new URL(`./${file}?v=${digest.slice(0, 8)}`, import.meta.url);
}

function report(message, state = "") {
  status.textContent = message;
  status.dataset.state = state;
}

async function buildFromStock(source, inputHash, {label, file, hash, progress}) {
  if (inputHash !== stockHash) throw new Error(`${label} needs the original Roland 5.52 APP1.`);
  report(progress);
  const response = await fetch(patchURL(file, hash));
  if (!response.ok) throw new Error(`The ${label} patch could not be loaded.`);
  const container = new Uint8Array(await response.arrayBuffer());
  const {bytes, digest} = window.applyPatch(container, source);
  if (digest !== hash) throw new Error(`The ${label} build checksum did not match.`);
  return bytes;
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
    const base = window.applyPatch(window.fromBase64(window.DOOMOS_PATCH), parent);
    parent = window.applyPatch(window.fromBase64(window.DOOMOS_KITS_PATCH), base.bytes).bytes;
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
    build: buildFromStock,
    label: "DOOM OS 0.7.4 + Genie Suite",
    file: "genie-doom074.patch",
    hash: combinedHash,
    progress: "Applying DOOM OS 0.7.4, SP-ETC v9.53, and the Genie Suite…",
    description: "Full DOOM OS 0.7.4 and SP-ETC v9.53 with the six-mode Genie Suite. Drum lanes can play samples, Sound Generator tones, DOOM instruments, or external MIDI in Drum Genie and Bounce.",
  },
  stock: {
    build: buildFromStock,
    label: "Stock Roland 5.52 + Genie Suite",
    file: "genie-stock.patch",
    hash: stockGenieHash,
    progress: "Adding the Genie Suite to stock Roland 5.52…",
    description: "The six-mode Genie Suite on stock Roland 5.52. Drum lanes can play samples, Sound Generator tones, or external MIDI in Drum Genie and Bounce.",
  },
  legacy: {
    build: buildLegacy,
    label: "Legacy DOOM OS 0.6.1 + Sequence Genie",
    description: "DOOM OS 0.6.1 and SP-ETC v9.53 with Sequence Genie only. Also accepts the exact SP-ETC v9.53 APP1.",
  },
};

function updateDescription() {
  description.textContent = builds[version.value].description;
  report("");
}
version.addEventListener("change", updateDescription);
input.addEventListener("change", () => report(""));
updateDescription();

form.addEventListener("submit", async event => {
  event.preventDefault();
  const file = input.files[0];
  if (!file || button.disabled) return;
  const selection = builds[version.value];
  button.disabled = version.disabled = input.disabled = true;
  report("Checking firmware…");
  try {
    const source = new Uint8Array(await file.arrayBuffer());
    const inputHash = await sha256(source);
    const output = await selection.build(source, inputHash, selection);
    const url = URL.createObjectURL(new Blob([output], {type:"application/octet-stream"}));
    const link = document.createElement("a");
    link.href = url;
    link.download = "SP404MKII_APP1.bin";
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    report(`Verified ${selection.label}. APP1 download started.`, "done");
  } catch (error) {
    report(error.message, "error");
  } finally {
    button.disabled = version.disabled = input.disabled = false;
  }
});
