const encoder = new TextEncoder();
const limits = { ssid: 32, password: 64, apiUrl: 127, token: 95, country: 3, language: 2 };

function bytes(value, limit, label) {
  const data = encoder.encode(value);
  if (data.length > limit) throw new Error(`${label} is too long (${data.length}/${limit} bytes).`);
  return data;
}
function appendField(output, data) { output.push(data.length, ...data); }
function base64url(data) {
  let binary = '';
  for (let i = 0; i < data.length; i += 0x8000)
    binary += String.fromCharCode(...data.slice(i, i + 0x8000));
  return btoa(binary).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '');
}
function payloadFromForm(form) {
  const values = Object.fromEntries(new FormData(form));
  let apiUrl = values.apiUrl.trim().replace(/\/+$/, '');
  if (!/^https?:\/\//.test(apiUrl)) throw new Error('API URL must start with http:// or https://.');
  const ssid = bytes(values.ssid, limits.ssid, 'SSID');
  if (!ssid.length) throw new Error('SSID is required.');
  const raw = [];
  appendField(raw, ssid);
  appendField(raw, bytes(values.password, limits.password, 'WiFi password'));
  appendField(raw, bytes(apiUrl, limits.apiUrl, 'API URL'));
  appendField(raw, bytes(values.token, limits.token, 'Device token'));
  appendField(raw, bytes(values.country.toUpperCase(), limits.country, 'Country code'));
  appendField(raw, bytes(values.language, limits.language, 'Language'));
  raw.push(form.elements.insecure.checked ? 1 : 0);
  return `GMS1_${base64url(new Uint8Array(raw))}`;
}
function renderQr(payload) {
  const qr = qrcode(0, 'M');
  qr.addData(payload, 'Byte');
  qr.make();
  const count = qr.getModuleCount();
  const quietZone = 4;
  const extent = count + quietZone * 2;
  const cell = Math.max(3, Math.floor(360 / count));
  let svg = `<svg role="img" aria-label="Scanner setup QR" viewBox="-${quietZone} -${quietZone} ${extent} ${extent}" shape-rendering="crispEdges">`;
  svg += `<rect x="-${quietZone}" y="-${quietZone}" width="${extent}" height="${extent}" fill="#fff"/>`;
  for (let row = 0; row < count; row++) for (let col = 0; col < count; col++)
    if (qr.isDark(row, col)) svg += `<rect x="${col}" y="${row}" width="1" height="1"/>`;
  svg += '</svg>';
  const host = document.getElementById('setup-qr');
  host.innerHTML = svg;
  host.style.width = `${extent * cell}px`;
  document.getElementById('qr-result').hidden = false;
}

const form = document.getElementById('qr-form');
form.addEventListener('submit', event => {
  event.preventDefault();
  const message = document.getElementById('qr-message');
  try { renderQr(payloadFromForm(form)); message.textContent = 'QR ready. Point the scanner at it.'; }
  catch (error) { message.textContent = error.message; document.getElementById('qr-result').hidden = true; }
});

const installer = document.getElementById('installer');
installer.addEventListener('state-changed', event => {
  const detail = event.detail || {};
  const status = document.getElementById('status');
  if (detail.message) status.textContent = detail.message;
  if (detail.state === 'FINISHED') document.getElementById('qr-form').scrollIntoView({ behavior: 'smooth' });
});
