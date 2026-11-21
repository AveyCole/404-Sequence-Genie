const form = document.querySelector("#form");
const input = document.querySelector("#input");
const button = document.querySelector("#build");
const status = document.querySelector("#result");

async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, byte => byte.toString(16).padStart(2, "0")).join("");
}

form.addEventListener("submit", async event => {
  event.preventDefault();
  const file = input.files[0];
  if (!file) return;
  button.disabled = true;
  status.textContent = "Checking firmware…";
  try {
    const response = await fetch(new URL("./genie-patch.json", import.meta.url));
    if (!response.ok) throw new Error("The patch could not be loaded.");
    const patch = await response.json();
    let parent = new Uint8Array(await file.arrayBuffer());
    const inputHash = await sha256(parent);
    if (inputHash !== patch.input) {
      const stockHash = window.DOOMOS_RELEASE.firmware.find(entry => entry.version === "5.52").stock_sha256;
      if (inputHash !== stockHash) {
        throw new Error("Select stock Roland 5.52 APP1 or the exact SP-ETC v9.53 APP1.");
      }
      status.textContent = "Applying SP-ETC v9.53…";
      const base = window.applyPatch(window.fromBase64(window.DOOMOS_PATCH), parent);
      parent = window.applyPatch(window.fromBase64(window.DOOMOS_KITS_PATCH), base.bytes).bytes;
      if (await sha256(parent) !== patch.input) throw new Error("SP-ETC checksum mismatch.");
    }
    const output = new Uint8Array(patch.size);
    output.set(parent);
    for (const [offset, hex] of patch.segments) {
      const bytes = Uint8Array.from(hex.match(/.{2}/g) ?? [], byte => parseInt(byte, 16));
      output.set(bytes, offset);
    }
    if (await sha256(output) !== patch.output) throw new Error("Output checksum mismatch.");
    const url = URL.createObjectURL(new Blob([output], {type:"application/octet-stream"}));
    const link = document.createElement("a");
    link.href = url;
    link.download = "SP404MKII_APP1.bin";
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    status.textContent = "Verified APP1 downloaded.";
  } catch (error) {
    status.textContent = error.message;
  } finally {
    button.disabled = false;
  }
});
