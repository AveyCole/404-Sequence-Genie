const form = document.querySelector("#form");
const input = document.querySelector("#input");
const version = document.querySelector("#version");
const button = document.querySelector("#build");
const status = document.querySelector("#result");

async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, byte => byte.toString(16).padStart(2, "0")).join("");
}

const stockHash = "4a3d67711e14dcc97d50249a4eee7dd6df0251a2f37757cbbe2556c233730d80";
const combinedHash = "49e489fcf93530767f087e472328f85dd9dfca012b28ae3794b5b89158a21460";

async function build074(source, inputHash) {
  if (inputHash !== stockHash) {
    throw new Error("The 0.7.4 build needs the original Roland 5.52 APP1.");
  }
  status.textContent = "Applying DOOM OS 0.7.4, SP-ETC v9.53, and Sequence Genie…";
  const response = await fetch(new URL("./genie-doom074.patch?v=49e489fc", import.meta.url));
  if (!response.ok) throw new Error("The 0.7.4 patch could not be loaded.");
  const container = new Uint8Array(await response.arrayBuffer());
  const {bytes, digest} = window.applyPatch(container, source);
  if (digest !== combinedHash) throw new Error("The 0.7.4 build checksum did not match.");
  return bytes;
}

async function buildStock(parent, inputHash) {
  if (inputHash !== stockHash) {
    throw new Error("The stock + Sequence Genie build needs the original Roland 5.52 APP1.");
  }
  status.textContent = "Adding Sequence Genie to Roland 5.52…";
  const response = await fetch(new URL("./genie-stock.json?v=fcfb7315", import.meta.url));
  if (!response.ok) throw new Error("The stock Sequence Genie patch could not be loaded.");
  const patch = await response.json();
  if (patch.input !== stockHash ||
      patch.output !== "fcfb731560121f95eb8ff2d3756796bcb878b9b2d3605d1949765e297b754ead") {
    throw new Error("The stock Sequence Genie patch checksum did not match.");
  }
  return applyOverlay(parent, patch);
}

async function buildLegacy(parent, inputHash) {
  const response = await fetch(new URL("./genie-patch.json", import.meta.url));
  if (!response.ok) throw new Error("The legacy patch could not be loaded.");
  const patch = await response.json();
  if (inputHash !== patch.input) {
    if (inputHash !== stockHash) {
      throw new Error("Select the original Roland 5.52 APP1 or exact SP-ETC v9.53 APP1.");
    }
    status.textContent = "Applying SP-ETC v9.53…";
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
  "074": [build074, "DOOM OS 0.7.4"],
  legacy: [buildLegacy, "legacy"],
  stock: [buildStock, "Stock 5.52 + Sequence Genie"],
};

form.addEventListener("submit", async event => {
  event.preventDefault();
  const file = input.files[0];
  if (!file) return;
  const [build, label] = builds[version.value];
  button.disabled = true;
  status.textContent = "Checking firmware…";
  try {
    const source = new Uint8Array(await file.arrayBuffer());
    const inputHash = await sha256(source);
    const output = await build(source, inputHash);
    const url = URL.createObjectURL(new Blob([output], {type:"application/octet-stream"}));
    const link = document.createElement("a");
    link.href = url;
    link.download = "SP404MKII_APP1.bin";
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    status.textContent = `Verified ${label} APP1 downloaded.`;
  } catch (error) {
    status.textContent = error.message;
  } finally {
    button.disabled = false;
  }
});
