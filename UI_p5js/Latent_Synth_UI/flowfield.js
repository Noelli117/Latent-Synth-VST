let uiMode = 0;
let modebutton;
let explorerButton;
let explorerOpen = false;
let explorerPanel;
let explorerStatusLabel;
let localModelSelect;
let onlineModelSelect;
let hostReqCounter = 1;
let nativeFoldButton;
let nativeFoldPanel;
let nativeFoldStatusLabel;
let nativeFoldControls = [];
let nativeFoldOpen = false;
let flowKnobs = [];
let latentKnobPairs = [];

const LATENT_DIM = 8;
const HOST_PUSH_MIN_INTERVAL_MS = 20;
const HOST_PUSH_DEADBAND = 0.0005;
const pageParams = new URLSearchParams(window.location.search);
const HOST_BRIDGE_ENABLED = pageParams.get("rave_host") === "1";
const HOST_NAV_FALLBACK = pageParams.get("rave_nav_fallback") === "1";
const FORCED_NAV_ACTIONS = new Set([
  "native_fold_get",
  "native_fold_set",
  "model_explorer",
  "list_models",
  "select_model",
  "import_model",
  "download_model",
]);
let hostStreamSeq = 1;
let lastHostPushMs = 0;
let lastSentMode = "";
let lastSentRadius = NaN;
let lastSentGlobal = new Array(LATENT_DIM).fill(NaN);
let lastSentScale = new Array(LATENT_DIM).fill(NaN);
let lastSentBias = new Array(LATENT_DIM).fill(NaN);

let radius = 0;


let scale = new Array(LATENT_DIM).fill(1);
let bias = new Array(LATENT_DIM).fill(0);

let particles = [];

let curviness = 1;
const BASE_PARTICLES = 128;
const MAX_PARTICLES = 320;
let noiseScale = 0.01;
let speed = 2.0;
let flowGain = 0.1;   // Global velocity gain (replaces hard-coded /10)
let contrast = 0.7;   // X/Y directional contrast
let intensity = 0.3; // Controls latent responsiveness and liveliness

const pi = Math.PI;
const EPSILON = 0.000001; // Prevent division by zero
const LATENT_RADIUS_MIN = 0.05;
const LATENT_RADIUS_MAX = 8.0;
const LATENT_NOISE_MAX = 4.0;
const TEMPORAL_JITTER_ONSET = 0.6;
const PARTICLE_STROKE_ALPHA = 190;
const TRAIL_DEFOG_ALPHA = 10;


// 8D latent state
let latent = new Array(LATENT_DIM).fill(0);
let latentDirection = new Array(LATENT_DIM).fill(0);
let latentNoiseState = new Array(LATENT_DIM).fill(0);
let directionUnit = new Array(LATENT_DIM).fill(0);
let targetLatent = new Array(LATENT_DIM).fill(0);

// Flow Field mode knobs
let knobSpeed;
let knobNoise;
let knobCurve;
let knobFlowGain;
let knobContrast;
let knobIntensity;
let knobLatentNoise;
let knobStereoWidth;

// Latent mode knobs (outer Bias, inner Scale)

let knobBias0, knobBias1, knobBias2, knobBias3;
let knobBias4, knobBias5, knobBias6, knobBias7;

let knobScale0, knobScale1, knobScale2, knobScale3;
let knobScale4, knobScale5, knobScale6, knobScale7;


function setExplorerStatus(message) {
  if (explorerStatusLabel) {
    explorerStatusLabel.html(message);
  }
}

function loadHostScriptPayload(scriptId, srcFile, globalKey, onDone) {
  const old = document.getElementById(scriptId);
  if (old) old.remove();

  const script = document.createElement("script");
  script.id = scriptId;
  script.src = `${srcFile}?t=${Date.now()}`;
  script.onload = () => onDone(window[globalKey] || null);
  script.onerror = () => onDone(window[globalKey] || null);
  document.head.appendChild(script);
}

function styleTopButton(button) {
  button.style("background-color", "black");
  button.style("color", "white");
  button.style("border", "1px solid white");
  button.style("box-shadow", "0 0 10px white");
  button.style("transition", "0.1s");
}

function attachTopButtonInteraction(button, onRelease) {
  button.mousePressed(() => {
    button.style("transform", "scale(0.92)");
    button.style("box-shadow", "0 0 4px white");
  });

  button.mouseReleased(() => {
    button.style("transform", "scale(1)");
    button.style("box-shadow", "0 0 10px white");
    if (onRelease) onRelease();
  });
}

function sendBridgeQuery(query) {
  const url = `rave://latent?${query}`;
  window.location.replace(url);
  return true;
}

function sendHostAction(action, params = {}) {
  if (!HOST_BRIDGE_ENABLED) {
    setExplorerStatus("Host bridge is disabled.");
    return "";
  }

  const search = new URLSearchParams();
  search.set("action", action);
  const req = String(hostReqCounter++);
  search.set("req", req);
  for (const key of Object.keys(params)) {
    if (params[key] !== undefined && params[key] !== null) {
      search.set(key, String(params[key]));
    }
  }

  // Force critical host actions through URL navigation so JUCE pageAboutToLoad always catches them.
  if (FORCED_NAV_ACTIONS.has(action)) {
    const url = `rave://latent?${search.toString()}`;
    window.location.replace(url);
    return req;
  }

  sendBridgeQuery(search.toString());
  return req;
}

function loadHostModelsFile(onDone) {
  loadHostScriptPayload("host-models-script", "host_models.js", "__hostModels", onDone);
}

function fillLocalModelSelect(localModels, selectedPath = "") {
  localModelSelect.elt.innerHTML = "";
  if (!localModels || localModels.length === 0) {
    localModelSelect.option("(no local models)", "");
    localModelSelect.value("");
    return;
  }
  for (let i = 0; i < localModels.length; i++) {
    const m = localModels[i];
    const label = m.name || "(unnamed)";
    const value = m.path || "";
    localModelSelect.option(label, value);
  }
  if (selectedPath) {
    localModelSelect.value(selectedPath);
  } else {
    localModelSelect.selected(localModelSelect.elt.options[0].value);
  }
}

function fillOnlineModelSelect(onlineModels) {
  onlineModelSelect.elt.innerHTML = "";
  if (!onlineModels || onlineModels.length === 0) {
    onlineModelSelect.option("(no online models)", "");
    onlineModelSelect.value("");
    return;
  }
  for (let i = 0; i < onlineModels.length; i++) {
    onlineModelSelect.option(onlineModels[i], onlineModels[i]);
  }
  onlineModelSelect.selected(onlineModelSelect.elt.options[0].value);
}

function requestAndPopulateModels() {
  setExplorerStatus("Refreshing model list...");
  const req = sendHostAction("list_models");

  const poll = (triesLeft) => {
    loadHostModelsFile((data) => {
      if (data && (!req || !data.req || data.req === req)) {
        const local = Array.isArray(data.localModels) ? data.localModels : [];
        const online = Array.isArray(data.onlineModels) ? data.onlineModels : [];
        fillLocalModelSelect(local, data.selectedPath || "");
        fillOnlineModelSelect(online);
        setExplorerStatus(`Local: ${local.length} | Online: ${online.length}`);
        return;
      }

      if (triesLeft > 0) {
        setTimeout(() => poll(triesLeft - 1), 140);
      } else {
        const stale = window.__hostModels || null;
        if (stale) {
          const local = Array.isArray(stale.localModels) ? stale.localModels : [];
          const online = Array.isArray(stale.onlineModels) ? stale.onlineModels : [];
          fillLocalModelSelect(local, stale.selectedPath || "");
          fillOnlineModelSelect(online);
          setExplorerStatus(`Local: ${local.length} | Online: ${online.length} (stale)`);
        } else {
          fillLocalModelSelect([], "");
          fillOnlineModelSelect([]);
          setExplorerStatus("Model list refresh timed out.");
        }
      }
    });
  };

  poll(15);
}

function createModelExplorerPanel() {
  explorerPanel = createDiv("");
  explorerPanel.position(330, 52);
  explorerPanel.style("width", "340px");
  explorerPanel.style("padding", "10px");
  explorerPanel.style("border", "1px solid #fff");
  explorerPanel.style("background", "rgba(0,0,0,0.88)");
  explorerPanel.style("color", "#fff");
  explorerPanel.style("box-shadow", "0 0 14px rgba(255,255,255,0.18)");
  explorerPanel.style("display", "none");
  explorerPanel.style("z-index", "20");

  const title = createDiv("Model Explorer");
  title.parent(explorerPanel);
  title.style("font-size", "14px");
  title.style("margin-bottom", "10px");

  const localLabel = createDiv("Local Models");
  localLabel.parent(explorerPanel);
  localLabel.style("font-size", "12px");
  localLabel.style("margin-bottom", "4px");

  localModelSelect = createSelect();
  localModelSelect.parent(explorerPanel);
  localModelSelect.style("width", "100%");
  localModelSelect.style("margin-bottom", "8px");
  localModelSelect.option("(loading...)", "");

  const loadBtn = createButton("Load Selected");
  loadBtn.parent(explorerPanel);
  loadBtn.style("margin-right", "8px");
  loadBtn.mousePressed(() => {
    const selectedPath = localModelSelect.value();
    const selectedIndex = localModelSelect.elt.selectedIndex;
    const selectedName =
      selectedIndex >= 0 ? localModelSelect.elt.options[selectedIndex].text : "";
    if (!selectedPath) {
      setExplorerStatus("No local model selected.");
      return;
    }
    setExplorerStatus("Loading model...");
    sendHostAction("select_model", {
      path: selectedPath,
      name: selectedName
    });
    setExplorerStatus("Load request sent.");
    setTimeout(requestAndPopulateModels, 250);
  });

  const importBtn = createButton("Import .ts");
  importBtn.parent(explorerPanel);
  importBtn.mousePressed(() => {
    setExplorerStatus("Opening file picker...");
    sendHostAction("import_model");
    setTimeout(requestAndPopulateModels, 800);
  });

  const sep = createDiv("");
  sep.parent(explorerPanel);
  sep.style("height", "1px");
  sep.style("background", "rgba(255,255,255,0.25)");
  sep.style("margin", "10px 0");

  const onlineLabel = createDiv("Official Models");
  onlineLabel.parent(explorerPanel);
  onlineLabel.style("font-size", "12px");
  onlineLabel.style("margin-bottom", "4px");

  onlineModelSelect = createSelect();
  onlineModelSelect.parent(explorerPanel);
  onlineModelSelect.style("width", "100%");
  onlineModelSelect.style("margin-bottom", "8px");
  onlineModelSelect.option("(loading...)", "");

  const downloadBtn = createButton("Download Official");
  downloadBtn.parent(explorerPanel);
  downloadBtn.mousePressed(() => {
    const selectedName = onlineModelSelect.value();
    if (!selectedName) {
      setExplorerStatus("No online model selected.");
      return;
    }
    setExplorerStatus("Downloading model...");
    sendHostAction("download_model", { name: selectedName });
    setExplorerStatus("Download request sent.");
    setTimeout(requestAndPopulateModels, 700);
  });

  const refreshBtn = createButton("Refresh");
  refreshBtn.parent(explorerPanel);
  refreshBtn.style("margin-left", "8px");
  refreshBtn.mousePressed(requestAndPopulateModels);

  explorerStatusLabel = createDiv("Idle");
  explorerStatusLabel.parent(explorerPanel);
  explorerStatusLabel.style("font-size", "11px");
  explorerStatusLabel.style("opacity", "0.85");
  explorerStatusLabel.style("margin-top", "10px");
}

function toggleExplorerPanel(open) {
  explorerOpen = open;
  explorerPanel.style("display", open ? "block" : "none");
  explorerButton.html(open ? "Close Explorer" : "Model Explorer");
  if (open) {
    requestAndPopulateModels();
  }
}

function setNativeFoldStatus(message) {
  if (nativeFoldStatusLabel) {
    nativeFoldStatusLabel.html(message);
  }
}

function sendNativeFoldParam(paramId, value) {
  sendHostAction("native_fold_set", { id: paramId, value });
}

function loadNativeFoldParamsFile(onDone) {
  loadHostScriptPayload(
    "host-native-fold-params-script",
    "host_native_fold_params.js",
    "__hostNativeFoldParams",
    onDone
  );
}

function addNativeFoldSection(title) {
  const section = createDiv("");
  section.parent(nativeFoldPanel);
  section.style("margin-bottom", "8px");

  const header = createButton(`▼ ${title}`);
  header.parent(section);
  header.style("width", "100%");
  header.style("text-align", "left");
  header.style("background", "#111");
  header.style("color", "#fff");
  header.style("border", "1px solid #555");
  header.style("padding", "4px 6px");
  header.style("font-size", "11px");

  const body = createDiv("");
  body.parent(section);
  body.style("padding", "6px 4px 4px 4px");
  body.style("border-left", "1px solid #444");
  body.style("border-right", "1px solid #444");
  body.style("border-bottom", "1px solid #444");

  let open = true;
  header.mousePressed(() => {
    open = !open;
    body.style("display", open ? "block" : "none");
    header.html(`${open ? "▼" : "▶"} ${title}`);
  });

  return body;
}

function addNativeFoldSlider(parent, config) {
  const row = createDiv("");
  row.parent(parent);
  row.style("margin-bottom", "6px");

  const label = createDiv(config.label);
  label.parent(row);
  label.style("font-size", "11px");
  label.style("margin-bottom", "2px");

  const slider = createSlider(config.min, config.max, config.defaultValue, config.step);
  slider.parent(row);
  slider.style("width", "130px");

  const valueText = createSpan(` ${Number(config.defaultValue).toFixed(config.decimals)}`);
  valueText.parent(row);
  valueText.style("font-size", "11px");

  slider.input(() => {
    const value = Number(slider.value());
    valueText.html(` ${value.toFixed(config.decimals)}`);
    sendNativeFoldParam(config.id, value);
  });

  nativeFoldControls.push({
    id: config.id,
    type: "slider",
    slider,
    valueText,
    decimals: config.decimals,
  });
}

function addNativeFoldSelect(parent, config) {
  const row = createDiv("");
  row.parent(parent);
  row.style("margin-bottom", "6px");

  const label = createDiv(config.label);
  label.parent(row);
  label.style("font-size", "11px");
  label.style("margin-bottom", "2px");

  const select = createSelect();
  select.parent(row);
  select.style("width", "140px");
  for (let i = 0; i < config.options.length; i++) {
    select.option(config.options[i].label, String(config.options[i].value));
  }
  select.changed(() => {
    sendNativeFoldParam(config.id, Number(select.value()));
  });

  nativeFoldControls.push({
    id: config.id,
    type: "select",
    select,
  });
}

function addNativeFoldToggle(parent, config) {
  const row = createDiv("");
  row.parent(parent);
  row.style("margin-bottom", "6px");

  const checkbox = createCheckbox(config.label, config.defaultValue);
  checkbox.parent(row);
  checkbox.style("font-size", "11px");
  checkbox.changed(() => {
    sendNativeFoldParam(config.id, checkbox.checked() ? 1 : 0);
  });

  nativeFoldControls.push({
    id: config.id,
    type: "toggle",
    checkbox,
  });
}

function applyNativeFoldParams(params) {
  if (!params) return;

  for (let i = 0; i < nativeFoldControls.length; i++) {
    const c = nativeFoldControls[i];
    if (!(c.id in params)) continue;
    const value = Number(params[c.id]);

    if (c.type === "slider") {
      if (document.activeElement !== c.slider.elt) {
        c.slider.value(value);
        c.valueText.html(` ${value.toFixed(c.decimals)}`);
      }
    } else if (c.type === "select") {
      c.select.value(String(Math.round(value)));
    } else if (c.type === "toggle") {
      c.checkbox.checked(value >= 0.5);
    }
  }

  const fftSize = params.fft_size ? Math.round(Number(params.fft_size)) : 0;
  setNativeFoldStatus(fftSize > 0 ? `FFT Size: ${fftSize}` : "Params synced");
}

function requestNativeFoldParams() {
  setNativeFoldStatus("Refreshing...");
  const req = sendHostAction("native_fold_get");

  const poll = (triesLeft) => {
    loadNativeFoldParamsFile((data) => {
      if (data && data.params && (!req || !data.req || data.req === req)) {
        applyNativeFoldParams(data.params);
        return;
      }

      if (triesLeft > 0) {
        setTimeout(() => poll(triesLeft - 1), 120);
      } else if (window.__hostNativeFoldParams && window.__hostNativeFoldParams.params) {
        applyNativeFoldParams(window.__hostNativeFoldParams.params);
        setNativeFoldStatus("Synced (cached)");
      } else {
        setNativeFoldStatus("Refresh timed out.");
      }
    });
  };

  poll(12);
}

function createNativeFoldablePanel() {
  nativeFoldPanel = createDiv("");
  nativeFoldPanel.position(16, 52);
  nativeFoldPanel.style("width", "190px");
  nativeFoldPanel.style("padding", "8px");
  nativeFoldPanel.style("border", "1px solid #777");
  nativeFoldPanel.style("background", "rgba(0,0,0,0.84)");
  nativeFoldPanel.style("color", "#fff");
  nativeFoldPanel.style("font-family", "monospace");
  nativeFoldPanel.style("z-index", "19");
  nativeFoldPanel.style("display", "none");

  const title = createDiv("Audio Params");
  title.parent(nativeFoldPanel);
  title.style("font-size", "12px");
  title.style("margin-bottom", "8px");

  const inputBody = addNativeFoldSection("Input Parameters");
  addNativeFoldSlider(inputBody, {
    id: "input_gain", label: "Gain (dB)",
    min: -70, max: 12, step: 0.1, defaultValue: 0, decimals: 1,
  });
  addNativeFoldSelect(inputBody, {
    id: "channel_mode", label: "Channel mode",
    options: [{ label: "L", value: 1 }, { label: "R", value: 2 }, { label: "L + R", value: 3 }],
  });

  const compBody = addNativeFoldSection("Compressor Parameters");
  addNativeFoldSlider(compBody, {
    id: "input_thresh", label: "Threshold (dB)",
    min: -60, max: 0, step: 0.1, defaultValue: 0, decimals: 1,
  });
  addNativeFoldSlider(compBody, {
    id: "input_ratio", label: "Ratio (:1)",
    min: 1, max: 10, step: 0.1, defaultValue: 1, decimals: 1,
  });

  const outputBody = addNativeFoldSection("Output Parameters");
  addNativeFoldSlider(outputBody, {
    id: "output_gain", label: "Gain (dB)",
    min: -70, max: 12, step: 0.1, defaultValue: 0, decimals: 1,
  });
  addNativeFoldSlider(outputBody, {
    id: "output_drywet", label: "Dry/Wet (%)",
    min: 0, max: 100, step: 1, defaultValue: 100, decimals: 0,
  });
  addNativeFoldToggle(outputBody, {
    id: "output_limit", label: "Limit", defaultValue: true,
  });

  const fftBody = addNativeFoldSection("FFT / Window");
  addNativeFoldSelect(fftBody, {
    id: "latency_mode", label: "FFT Size",
    options: [
      { label: "512", value: 9 }, { label: "1024", value: 10 },
      { label: "2048", value: 11 }, { label: "4096", value: 12 },
      { label: "8192", value: 13 }, { label: "16384", value: 14 },
      { label: "32768", value: 15 },
    ],
  });

  const refreshBtn = createButton("Refresh Params");
  refreshBtn.parent(nativeFoldPanel);
  refreshBtn.mousePressed(requestNativeFoldParams);

  nativeFoldStatusLabel = createDiv("Idle");
  nativeFoldStatusLabel.parent(nativeFoldPanel);
  nativeFoldStatusLabel.style("font-size", "11px");
  nativeFoldStatusLabel.style("opacity", "0.85");
  nativeFoldStatusLabel.style("margin-top", "6px");
}

function toggleNativeFoldPanel(open) {
  nativeFoldOpen = open;
  nativeFoldPanel.style("display", open ? "block" : "none");
  nativeFoldButton.html(open ? "Close Audio Params" : "Audio Params");
  if (open) requestNativeFoldParams();
}

function getFlowKnobs() {
  return flowKnobs;
}

function getLatentKnobPairs() {
  return latentKnobPairs;
}

function updateLatentArraysFromKnobs() {
  const pairs = getLatentKnobPairs();
  for (let i = 0; i < pairs.length; i++) {
    const pair = pairs[i];
    const outerBiasKnob = pair[0];
    const innerScaleKnob = pair[1];
    bias[i] = outerBiasKnob.value;
    scale[i] = innerScaleKnob.value;
  }
}

function getSmoothedRadius(values) {
  let sumSq = 0;
  for (let i = 0; i < values.length; i++) {
    sumSq += values[i] * values[i];
  }
  return sqrt(sumSq);
}

// Box-Muller Gaussian white noise (mean=0, std=1)
function randn() {
  let u = 0;
  let v = 0;
  while (u === 0) u = Math.random();
  while (v === 0) v = Math.random();
  return sqrt(-2.0 * Math.log(u)) * cos(2.0 * pi * v);
}

function updateKnobsForCurrentMode() {
  if (uiMode === 0) {
    const flowKnobs = getFlowKnobs();
    for (let i = 0; i < flowKnobs.length; i++) {
      flowKnobs[i].update();
    }
    return;
  }

  const latentPairs = getLatentKnobPairs();
  for (let i = 0; i < latentPairs.length; i++) {
    latentPairs[i][0].update();
    latentPairs[i][1].update();
  }
  if (knobLatentNoise) knobLatentNoise.update();
  if (knobStereoWidth) knobStereoWidth.update();
  updateLatentArraysFromKnobs();
}

function drawKnobsForCurrentMode() {
  if (uiMode === 0) {
    knobSpeed.display("Speed");
    knobNoise.display("Noise Scale");
    knobCurve.display("Curve");
    knobFlowGain.display("Flow Gain");
    knobContrast.display("Contrast");
    knobIntensity.display("Intensity");
    return;
  }

  const latentPairs = getLatentKnobPairs();
  for (let i = 0; i < latentPairs.length; i++) {
    latentPairs[i][0].display(`Latent ${i + 1}`);
    latentPairs[i][1].display("");
  }
  knobLatentNoise.display("Latent Noise");
  knobStereoWidth.display("Stereo Width");
}

// Initialize UI and runtime state
function setup() {
  
  // Canvas and background
  createCanvas(1000, 700);
  background(0);
  
  // Top mode switch button
  modebutton = createButton("Switch Mode");
  modebutton.position(465, 10);
  styleTopButton(modebutton);
  attachTopButtonInteraction(modebutton, () => {
    uiMode = (uiMode + 1) % 2;
  });

  explorerButton = createButton("Model Explorer");
  explorerButton.position(590, 10);
  styleTopButton(explorerButton);
  attachTopButtonInteraction(explorerButton, () => {
    toggleExplorerPanel(!explorerOpen);
  });

  nativeFoldButton = createButton("Audio Params");
  nativeFoldButton.position(330, 10);
  styleTopButton(nativeFoldButton);
  attachTopButtonInteraction(nativeFoldButton, () => {
    toggleNativeFoldPanel(!nativeFoldOpen);
  });

  createModelExplorerPanel();
  createNativeFoldablePanel();
  
  for (let i = 0; i < MAX_PARTICLES; i++) {
    particles.push(createVector(random(width), random(height)));
  }

  for (let i = 0; i < LATENT_DIM; i++) {
    latent[i] = random(-1, 1);
  }
  
  for (let i = 0; i < LATENT_DIM; i++) {
    latentDirection[i] = random(-1, 1);
  }
  normalizeDirection();

  stroke(255);
  strokeWeight(3);

  // Flow Field knob layout
  knobSpeed = new Knob(100, 125, 40, 0, 10, 5, true, 0.75);
  knobNoise = new Knob(100, 325, 40, 0.001, 0.05, 0.0255, true, 5.0);
  knobCurve = new Knob(100, 525, 40, 0.5, 4, 2.25, true, 5.0);

  knobFlowGain = new Knob(900, 125, 40, 0.01, 0.3, 0.155, true, 0.75);
  knobContrast = new Knob(900, 325, 40, 0, 0.95, 0.475, true, 5.0);
  knobIntensity = new Knob(900, 525, 40, 0.3, 1.0, 0.65, true, 5.0);
  knobLatentNoise = new Knob(430, 600, 35, 0, 3, 0, true, 1.0);
  knobStereoWidth = new Knob(570, 600, 35, 0, 200, 100, true, 1.0);
  
  // Latent knob layout (concentric: outer Bias, inner Scale)
  // Latent 1
  knobBias0  = new Knob(100, 150, 40, -3, 3, 0, true, 1.0); // outer
  knobScale0 = new Knob(100, 150, 25, 0, 5, 1, false, 1.0);  // inner

  // Latent 2
  knobBias1  = new Knob(100, 275, 40, -3, 3, 0, true, 1.0);
  knobScale1 = new Knob(100, 275, 25, 0, 5, 1, false, 1.0);

  // Latent 3
  knobBias2  = new Knob(100, 400, 40, -3, 3, 0, true, 1.0);
  knobScale2 = new Knob(100, 400, 25, 0, 5, 1, false, 1.0);

  // Latent 4
  knobBias3  = new Knob(100, 525, 40, -3, 3, 0, true, 1.0);
  knobScale3 = new Knob(100, 525, 25, 0, 5, 1, false, 1.0);

  // Latent 5
  knobBias4  = new Knob(900, 150, 40, -3, 3, 0, true, 1.0);
  knobScale4 = new Knob(900, 150, 25, 0, 5, 1, false, 1.0);

  // Latent 6
  knobBias5  = new Knob(900, 275, 40, -3, 3, 0, true, 1.0);
  knobScale5 = new Knob(900, 275, 25, 0, 5, 1, false, 1.0);

  // Latent 7
  knobBias6  = new Knob(900, 400, 40, -3, 3, 0, true, 1.0);
  knobScale6 = new Knob(900, 400, 25, 0, 5, 1, false, 1.0);

  // Latent 8
  knobBias7  = new Knob(900, 525, 40, -3, 3, 0, true, 1.0);
  knobScale7 = new Knob(900, 525, 25, 0, 5, 1, false, 1.0); 

  flowKnobs = [knobSpeed, knobNoise, knobCurve, knobFlowGain, knobContrast, knobIntensity];
  latentKnobPairs = [
    [knobBias0, knobScale0],
    [knobBias1, knobScale1],
    [knobBias2, knobScale2],
    [knobBias3, knobScale3],
    [knobBias4, knobScale4],
    [knobBias5, knobScale5],
    [knobBias6, knobScale6],
    [knobBias7, knobScale7],
  ];

}

// Normalize direction vector
function normalizeDirection() {
  let mag = 0;
  for (let i = 0; i < LATENT_DIM; i++) {
    mag += latentDirection[i] * latentDirection[i];
  }
  mag = sqrt(mag) + EPSILON;

  for (let i = 0; i < LATENT_DIM; i++) {
    latentDirection[i] /= mag;
  }
}


function draw() {
  // 1) Update knob state first
  updateKnobsForCurrentMode();

  speed = knobSpeed.value;
  noiseScale = knobNoise.value;
  curviness = knobCurve.value;
  flowGain = knobFlowGain.value;
  contrast = knobContrast.value;
  intensity = knobIntensity.value;

  // 2) Higher intensity -> longer trails (less per-frame erase)
  const trailAlpha = lerp(16, 2, intensity);
  background(0, trailAlpha);
  // Soft cleanup: continuously remove residue without hard screen wipes.
  noStroke();
  fill(0, TRAIL_DEFOG_ALPHA);
  rect(0, 0, width, height);
  stroke(255, PARTICLE_STROKE_ALPHA);

  // 3) Particle count scales with intensity
  const activeParticles = floor(lerp(BASE_PARTICLES, MAX_PARTICLES, intensity));
  let S = speed * flowGain;
  let safeS = Math.max(S, EPSILON);
  let sx = 1 - contrast;
  let sy = 1 + contrast;
  let invNum = 1 / activeParticles;

  // Statistics used for latent generation
  let sumVX = 0;
  let sumVY = 0;
  let sumPX = 0;
  let sumPY = 0;
  let sumNoise = 0;
  let sumCos = 0;
  let sumSin = 0;
  let sumDriveMag = 0;
  
  // Flow field idea reference:
  // https://www.youtube.com/watch?v=sZBfLgfsvSk
  for (let i = 0; i < activeParticles; i++) {
    let p = particles[i];
    point(p.x, p.y);

    let n = noise(p.x * noiseScale, p.y * noiseScale);
    let a = pi * curviness * (n - 0.5);

    // X/Y contrast coupling: when one side increases, the other is relatively reduced.

    let tx = cos(a) * sx;
    let ty = sin(a) * sy;
    const driveMag = sqrt(tx * tx + ty * ty);
    sumDriveMag += driveMag;

    let magnitude = sqrt(tx * tx + ty * ty) + EPSILON;
    let invMagnitude = 1 / magnitude;

    let vx = tx * invMagnitude * S;
    let vy = ty * invMagnitude * S;

    p.x += vx;
    p.y += vy;

    // Velocity stats
    sumVX += vx;
    sumVY += vy;

    // Position stats
    sumPX += p.x;
    sumPY += p.y;

    // Noise stats
    sumNoise += n;

    // Direction stats (mean motion direction)
    sumCos += tx * invMagnitude;
    sumSin += ty * invMagnitude;

    if (p.x < 0 || p.x > width || p.y < 0 || p.y > height) {
      p.x = random(width);
      p.y = random(height);
    }
  }

  // Motion Energy -> Latent Radius:
  // latent = radius * direction
  // direction = timbre orientation, radius = energy/intensity.

  let meanVX = sumVX * invNum;
  let meanVY = sumVY * invNum;
  const meanDriveMag = sumDriveMag * invNum;
  const maxDriveMag = Math.max(Math.abs(sx), Math.abs(sy)) + EPSILON;
  const motionNorm = constrain(meanDriveMag / maxDriveMag, 0, 1);
  let motionEnergy = motionNorm * safeS;

  // Map motion energy into latent radius range
  let r_target = map(
    motionEnergy,
    0,
    safeS,
    LATENT_RADIUS_MIN,
    LATENT_RADIUS_MAX
  );
  r_target = constrain(r_target, LATENT_RADIUS_MIN, LATENT_RADIUS_MAX);

  // Extract 8D directional features from flow stats:
  // d0/d1: spatial center, d2: noise complexity, d3: dominant motion angle
  // d4/d5: mean velocity, d6: speed state, d7: curviness state

  let avgX = sumPX * invNum;
  let avgY = sumPY * invNum;
  let avgNoise = sumNoise * invNum;
  let avgAngle = atan2(sumSin * invNum, sumCos * invNum);

  // Build 8D direction features (roughly in [-1, 1])
  let d = latentDirection;

  d[0] = map(avgX, 0, width, -1, 1);
  d[1] = map(avgY, 0, height, -1, 1);
  d[2] = avgNoise * 2 - 1;
  d[3] = avgAngle / pi;

  d[4] = map(meanVX, -safeS, safeS, -1, 1);
  d[5] = map(meanVY, -safeS, safeS, -1, 1);
  d[6] = map(speed, 0, 10, -1, 1);
  d[7] = map(curviness, 0.5, 4, -1, 1);

  // Normalize direction
  let norm = 0;
  for (let i = 0; i < LATENT_DIM; i++) {
    norm += d[i] * d[i];
  }
  norm = sqrt(norm) + EPSILON;

  for (let i = 0; i < LATENT_DIM; i++) {
    directionUnit[i] = d[i] / norm;
  }


  // Apply radius scaling: latent = normalize(direction) * r_target
  const isGlobalMode = (uiMode === 0);
  const temporalJitterMix = constrain(
    (intensity - TEMPORAL_JITTER_ONSET) / (1.0 - TEMPORAL_JITTER_ONSET),
    0,
    1
  );
  const motionJitterAmount = intensity * motionNorm * 0.6 * temporalJitterMix;
  const latentNoiseAmount = intensity * LATENT_NOISE_MAX;
  const noiseSpeed = lerp(0.03, 0.6, intensity);

  for (let i = 0; i < LATENT_DIM; i++) {

  // Global direction component
  const global = r_target * directionUnit[i];

  // Dual mode:
  // global mode uses flow-field latent directly;
  // native mode applies only local scale/bias.
  if (isGlobalMode) {
    targetLatent[i] = global;
    const temporalJitter = (noise(frameCount * 0.02, i * 31.7) - 0.5) * 2.0;
    targetLatent[i] += motionJitterAmount * temporalJitter;

    // Gaussian perturbation update speed is tied to intensity:
    // low intensity is smoother, high intensity changes faster.
    latentNoiseState[i] = lerp(latentNoiseState[i], randn(), noiseSpeed);
    targetLatent[i] += latentNoiseAmount * latentNoiseState[i];
  } else {
    targetLatent[i] = bias[i];
  }
  }
  // --------------------------------------------

  const alpha = lerp(0.01, 0.25, intensity);
  for (let i = 0; i < LATENT_DIM; i++) {
    latent[i] += alpha * (targetLatent[i] - latent[i]);
  }

  let smoothedRadius = getSmoothedRadius(latent);

  pushLatentToHost(smoothedRadius, latent, scale, bias);
  // 4) Draw knobs
  drawKnobsForCurrentMode();

}

// Push latent state to host
function hasArrayDelta(current, previous, deadband) {
  for (let i = 0; i < LATENT_DIM; i++) {
    if (Math.abs(current[i] - previous[i]) > deadband) {
      return true;
    }
  }
  return false;
}

function copyArrayValues(destination, source) {
  for (let i = 0; i < LATENT_DIM; i++) {
    destination[i] = source[i];
  }
}

function pushLatentToHost(radiusValue, latentValues, scaleValues, biasValues) {
  if (!HOST_BRIDGE_ENABLED) return;
  const isGlobalMode = (uiMode === 0);
  const mode = isGlobalMode ? "global" : "native";
  const nowMs = millis();
  const modeChanged = mode !== lastSentMode;

  const params = new URLSearchParams();
  params.set("action", "latent_stream");
  params.set("mode", mode);
  params.set("seq", String(hostStreamSeq));
  params.set("radius", radiusValue.toFixed(6));

  for (let i = 0; i < LATENT_DIM; i++) {
    if (isGlobalMode) {
      params.set(`g${i}`, latentValues[i].toFixed(6));
    } else {
      params.set(`s${i}`, scaleValues[i].toFixed(6));
      params.set(`b${i}`, biasValues[i].toFixed(6));
    }
  }

  const query = params.toString();
  if (!sendBridgeQuery(query)) return;

  hostStreamSeq += 1;
  lastHostPushMs = nowMs;
  lastSentMode = mode;
  lastSentRadius = radiusValue;
  if (isGlobalMode) {
    copyArrayValues(lastSentGlobal, latentValues);
  } else {
    copyArrayValues(lastSentScale, scaleValues);
    copyArrayValues(lastSentBias, biasValues);
  }
}

function commitNativeBottomKnobsToHost() {
  if (!HOST_BRIDGE_ENABLED || uiMode !== 1) return;
  sendHostAction("native_fold_set", { id: "latent_jitter", value: knobLatentNoise.value.toFixed(6) });
  sendHostAction("native_fold_set", { id: "output_width", value: knobStereoWidth.value.toFixed(6) });
}

// ==========================
// Knob class
// ==========================
class Knob {

  constructor(
    x,
    y,
    r,
    min,
    max,
    value,
    showTicks = true,
    taper = 0.7   // 1 is linear, >1 gives a more exponential feel
  ) {

    this.x = x;
    this.y = y;
    this.r = r;

    this.min = min;
    this.max = max;

    this.showTicks = showTicks;
    this.taper = taper;

    this.dragging = false;

    // UI layer: keep value normalized to 0~1
    // Normalize initial value
    this.normalized = (value - min) / (max - min);
    this.normalized = constrain(this.normalized, 0, 1);

    // Compute mapped real value
    this.value = this.computeValue();
  }

  // DSP mapping layer
  computeValue() {

    // Apply taper only here to decouple display from parameter mapping
    let shaped = pow(this.normalized, this.taper);

    return this.min + shaped * (this.max - this.min);
  }

  // Drag update
  update() {

    if (this.dragging) {

      // Linear change in normalized space
      let delta = (pmouseY - mouseY) * 0.005;

      this.normalized += delta;
      this.normalized = constrain(this.normalized, 0, 1);

      // Update real value
      this.value = this.computeValue();
    }
  }

  // Draw knob
  display(label) {

    push();
    translate(this.x, this.y);

    fill(20);
    stroke(255);
    circle(0, 0, this.r * 2);

    let startAngle = PI * 0.75;
    let endAngle   = PI * 2.25;

    // Ticks
    if (this.showTicks) {

      strokeWeight(2);
      stroke(180);

      // Minimum marker
      line(
        cos(startAngle) * this.r * 0.85,
        sin(startAngle) * this.r * 0.85,
        cos(startAngle) * this.r,
        sin(startAngle) * this.r
      );

      noStroke();
      fill(180);
      textAlign(CENTER, CENTER);
      text(
        "-",
        cos(startAngle) * this.r * 1.2,
        sin(startAngle) * this.r * 1.2
      );

      // Maximum marker
      stroke(255);
      strokeWeight(2);
      line(
        cos(endAngle) * this.r * 0.85,
        sin(endAngle) * this.r * 0.85,
        cos(endAngle) * this.r,
        sin(endAngle) * this.r
      );

      noStroke();
      fill(255);
      text(
        "+",
        cos(endAngle) * this.r * 1.2,
        sin(endAngle) * this.r * 1.2
      );
    }

    // Pointer
    // Angle depends only on normalized value for linear visual behavior
    let angle = startAngle + this.normalized * (endAngle - startAngle);

    strokeWeight(3);
    stroke(255);
    line(
      0,
      0,
      cos(angle) * this.r * 0.8,
      sin(angle) * this.r * 0.8
    );

    // Label
    noStroke();
    fill(255);
    textAlign(CENTER);
    textSize(12);
    text(label, 0, this.r + 20);

    pop();
  }

  // Mouse interaction
  pressed() {

    if (dist(mouseX, mouseY, this.x, this.y) < this.r) {
      this.dragging = true;
    }
  }

  released() {
    this.dragging = false;
  }

  // Utility method
  setValue(v) {

    this.normalized = (v - this.min) / (this.max - this.min);
    this.normalized = constrain(this.normalized, 0, 1);

    this.value = this.computeValue();
  }
}

// ==========================
// Mouse events
// ==========================
function pressConcentricKnobs(outerKnob, innerKnob) {
  const d = dist(mouseX, mouseY, outerKnob.x, outerKnob.y);
  if (d < innerKnob.r) {
    innerKnob.pressed();
  } else if (d < outerKnob.r) {
    outerKnob.pressed();
  }
}

function mousePressed() {
  if (uiMode === 0) {
    const flowKnobs = getFlowKnobs();
    for (let i = 0; i < flowKnobs.length; i++) {
      flowKnobs[i].pressed();
    }
    return;
  }

  // Latent concentric knobs: inner ring has priority
  const latentPairs = getLatentKnobPairs();
  for (let i = 0; i < latentPairs.length; i++) {
    pressConcentricKnobs(latentPairs[i][0], latentPairs[i][1]);
  }
  knobLatentNoise.pressed();
  knobStereoWidth.pressed();
}

function mouseReleased() {
  const flowKnobs = getFlowKnobs();
  for (let i = 0; i < flowKnobs.length; i++) {
    flowKnobs[i].released();
  }

  const latentPairs = getLatentKnobPairs();
  for (let i = 0; i < latentPairs.length; i++) {
    latentPairs[i][0].released();
    latentPairs[i][1].released();
  }
  knobLatentNoise.released();
  knobStereoWidth.released();
  commitNativeBottomKnobsToHost();
}
