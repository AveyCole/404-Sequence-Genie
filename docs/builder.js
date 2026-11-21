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
    const parent = new Uint8Array(await file.arrayBuffer());
    if (await sha256(parent) !== patch.input) {
      throw new Error("This file is not the supported SP-ETC v9.53 APP1.");
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
