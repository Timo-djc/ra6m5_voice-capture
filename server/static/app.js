const API_BASE = window.location.origin;
const RECORD_SECONDS = 6;
const REGISTER_CLIPS = 3;
const HISTORY_LIMIT = 20;
const POLL_INTERVAL_MS = 4000;
const SPEAKER_ID_RE = /^[a-zA-Z0-9_-]{1,32}$/;

function requiredElement(id) {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`missing element: ${id}`);
  }
  return element;
}

const refs = {
  speakerIdInput: requiredElement("speakerIdInput"),
  refreshBtn: requiredElement("refreshBtn"),
  registerDirectBtn: requiredElement("registerDirectBtn"),
  registerRelayBtn: requiredElement("registerRelayBtn"),
  testDirectBtn: requiredElement("testDirectBtn"),
  testRelayBtn: requiredElement("testRelayBtn"),
  resetDataBtn: requiredElement("resetDataBtn"),
  phaseText: requiredElement("phaseText"),
  countdownText: requiredElement("countdownText"),
  pendingText: requiredElement("pendingText"),
  backendConfiguredText: requiredElement("backendConfiguredText"),
  backendEffectiveText: requiredElement("backendEffectiveText"),
  backendStrategyText: requiredElement("backendStrategyText"),
  backendMessageText: requiredElement("backendMessageText"),
  backendStatusNote: requiredElement("backendStatusNote"),
  speakerList: requiredElement("speakerList"),
  latestFlowText: requiredElement("latestFlowText"),
  latestExpectedText: requiredElement("latestExpectedText"),
  latestActualText: requiredElement("latestActualText"),
  latestPassedText: requiredElement("latestPassedText"),
  latestScoreText: requiredElement("latestScoreText"),
  latestMarginText: requiredElement("latestMarginText"),
  latestTimeText: requiredElement("latestTimeText"),
  mailAttemptText: requiredElement("mailAttemptText"),
  mailSentText: requiredElement("mailSentText"),
  mailErrorText: requiredElement("mailErrorText"),
  historyTableBody: requiredElement("historyTableBody"),
  activityList: requiredElement("activityList"),
  confirmDialog: requiredElement("confirmDialog"),
};

const state = {
  busy: false,
  speakers: [],
  history: [],
  latestResult: null,
  activities: [],
  pendingRelay: null,
  pollTimer: null,
  systemStatus: null,
};

const api = {
  async request(path, options = {}) {
    const response = await fetch(`${API_BASE}${path}`, options);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(payload.detail || payload.message || `HTTP ${response.status}`);
    }
    return payload;
  },

  getSpeakers() {
    return this.request("/api/speakers");
  },

  getIdentifyEvents(limit = HISTORY_LIMIT) {
    return this.request(`/api/identify-events?limit=${encodeURIComponent(limit)}`);
  },

  getSystemStatus() {
    return this.request("/api/system/status");
  },

  async deleteSpeaker(speakerId) {
    const response = await fetch(`${API_BASE}/api/speakers/${encodeURIComponent(speakerId)}`, {
      method: "DELETE",
    });
    if (response.ok || response.status === 404) {
      return;
    }
    const payload = await response.json().catch(() => ({}));
    throw new Error(payload.detail || `delete speaker failed: ${response.status}`);
  },

  uploadSamples(speakerId, blobs) {
    const form = new FormData();
    blobs.forEach((blob, idx) => {
      form.append("files", blob, `${String(idx + 1).padStart(2, "0")}_enroll.wav`);
    });
    return this.request(`/api/speakers/${encodeURIComponent(speakerId)}/samples`, {
      method: "POST",
      body: form,
    });
  },

  identify(speakerId, blob, location = null) {
    const form = new FormData();
    form.append("file", blob, "identify.wav");
    form.append("speaker_id", speakerId);
    form.append("notify_on_failure", "true");
    if (location) {
      if (location.label) {
        form.append("location_label", location.label);
      }
      if (Number.isFinite(location.latitude)) {
        form.append("latitude", String(location.latitude));
      }
      if (Number.isFinite(location.longitude)) {
        form.append("longitude", String(location.longitude));
      }
      if (Number.isFinite(location.accuracy_m)) {
        form.append("accuracy_m", String(location.accuracy_m));
      }
    }
    return this.request("/api/identify", {
      method: "POST",
      body: form,
    });
  },

  exportRegister(speakerId, blobs) {
    const form = new FormData();
    form.append("speaker_id", speakerId);
    blobs.forEach((blob, idx) => {
      form.append("files", blob, `${String(idx + 1).padStart(2, "0")}_enroll.wav`);
    });
    return this.request("/api/mvp-demo/register-export", {
      method: "POST",
      body: form,
    });
  },

  exportIdentify(speakerId, blob) {
    const form = new FormData();
    form.append("speaker_id", speakerId);
    form.append("file", blob, "04_identify.wav");
    return this.request("/api/mvp-demo/identify-export", {
      method: "POST",
      body: form,
    });
  },

  resetAll() {
    return this.request("/api/admin/reset", { method: "POST" });
  },
};

function pushActivity(message) {
  const stamp = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  state.activities.unshift(`[${stamp}] ${message}`);
  state.activities = state.activities.slice(0, 8);
  renderActivity();
}

function renderActivity() {
  if (!state.activities.length) {
    refs.activityList.innerHTML = '<li class="activity-item">页面已就绪，等待操作。</li>';
    return;
  }
  refs.activityList.innerHTML = state.activities
    .map((item) => `<li class="activity-item">${escapeHtml(item)}</li>`)
    .join("");
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function formatFlow(flow) {
  if (flow === "pc-direct") {
    return "PC 直接";
  }
  if (flow === "mcu-relay") {
    return "MCU 中转";
  }
  return flow || "--";
}

function formatSource(source) {
  if (source === "pc-direct") {
    return "PC 直接";
  }
  if (source === "mcu-relay") {
    return "MCU 中转";
  }
  if (!source) {
    return "--";
  }
  return source;
}

function formatDateTime(value) {
  if (!value) {
    return "--";
  }
  const normalized = String(value).replace(" ", "T");
  const parsed = new Date(normalized);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }
  return parsed.toLocaleString("zh-CN", { hour12: false });
}

function formatFixed(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number.toFixed(3) : "--";
}

function setPhase(text) {
  refs.phaseText.textContent = text;
}

function setCountdown(text) {
  refs.countdownText.textContent = text;
}

function formatBackendStrategy(status) {
  if (!status) {
    return "--";
  }
  const label = status.identify_strategy === "sample-majority" ? "sample-majority" : "centroid";
  return `${label} @ ${formatFixed(status.identify_threshold)}`;
}

function buildBackendMessage(status) {
  if (!status) {
    return "--";
  }
  const parts = [];
  if (status.model_id) {
    parts.push(status.model_revision ? `${status.model_id} @ ${status.model_revision}` : status.model_id);
  }
  if (status.message) {
    parts.push(status.message);
  }
  return parts.length ? parts.join(" | ") : "no extra note";
}

function renderBackendStatus() {
  const status = state.systemStatus;
  refs.backendConfiguredText.textContent = status ? status.configured_backend : "--";
  refs.backendEffectiveText.textContent = status ? status.effective_backend : "--";
  refs.backendStrategyText.textContent = formatBackendStrategy(status);
  refs.backendMessageText.textContent = buildBackendMessage(status);
  refs.backendStatusNote.classList.toggle("status-warning", Boolean(status && status.warning));
}

function renderPending() {
  if (!state.pendingRelay) {
    refs.pendingText.textContent = "无";
    return;
  }

  if (state.pendingRelay.kind === "register") {
    const speaker = getSpeakerById(state.pendingRelay.speakerId);
    if (speaker && speaker.last_source === "mcu-relay" && speaker.active) {
      refs.pendingText.textContent = `MCU 注册完成：${speaker.speaker_id}`;
      return;
    }
    if (speaker && speaker.last_source === "mcu-relay") {
      refs.pendingText.textContent = `MCU 注册进行中：${speaker.sample_count}/${speaker.required_samples}`;
      return;
    }
    refs.pendingText.textContent = `等待 MCU 回传注册：${state.pendingRelay.speakerId}`;
    return;
  }

  refs.pendingText.textContent = `等待 MCU 回传测试：${state.pendingRelay.speakerId}`;
}

function setBusy(nextBusy) {
  state.busy = nextBusy;
  [
    refs.speakerIdInput,
    refs.refreshBtn,
    refs.registerDirectBtn,
    refs.registerRelayBtn,
    refs.testDirectBtn,
    refs.testRelayBtn,
    refs.resetDataBtn,
  ].forEach((element) => {
    element.disabled = nextBusy;
  });
}

function getSpeakerById(speakerId) {
  return state.speakers.find((item) => item.speaker_id === speakerId) || null;
}

function readSpeakerId({ requireActive = false } = {}) {
  const speakerId = refs.speakerIdInput.value.trim();
  if (!SPEAKER_ID_RE.test(speakerId)) {
    throw new Error("speaker_id 必须匹配 [a-zA-Z0-9_-]{1,32}");
  }
  if (requireActive) {
    const speaker = getSpeakerById(speakerId);
    if (!speaker) {
      throw new Error("测试前请先选择一个已注册的 speaker_id");
    }
    if (!speaker.active) {
      throw new Error("该 speaker 尚未完成注册，请先补齐 3 段注册语音");
    }
  }
  return speakerId;
}

function selectSpeaker(speakerId) {
  refs.speakerIdInput.value = speakerId;
  renderSpeakers();
}

function renderSpeakers() {
  if (!state.speakers.length) {
    refs.speakerList.innerHTML = '<div class="empty-state">当前没有已注册 speaker。完成一次注册后，这里会自动展示 ID、状态和最近来源。</div>';
    return;
  }

  const selected = refs.speakerIdInput.value.trim();
  refs.speakerList.innerHTML = state.speakers
    .map((speaker) => {
      const classes = ["speaker-card"];
      if (speaker.active) {
        classes.push("active");
      }
      if (selected === speaker.speaker_id) {
        classes.push("selected");
      }
      return `
        <button type="button" class="${classes.join(" ")}" data-speaker-id="${escapeHtml(speaker.speaker_id)}">
          <div class="speaker-top">
            <span class="speaker-id">${escapeHtml(speaker.speaker_id)}</span>
            <span class="tag ${speaker.active ? "active" : "inactive"}">${speaker.active ? "已激活" : "未激活"}</span>
          </div>
          <div class="speaker-meta">
            <span class="tag source">${escapeHtml(formatSource(speaker.last_source))}</span>
            <span>样本 ${speaker.sample_count}/${speaker.required_samples}</span>
          </div>
          <div class="speaker-meta">最近更新时间：${escapeHtml(formatDateTime(speaker.last_sample_at || speaker.created_at))}</div>
        </button>
      `;
    })
    .join("");

  refs.speakerList.querySelectorAll("[data-speaker-id]").forEach((element) => {
    element.addEventListener("click", () => {
      selectSpeaker(element.getAttribute("data-speaker-id") || "");
    });
  });
}

function renderLatestResult() {
  const latest = state.latestResult;
  if (!latest) {
    refs.latestFlowText.textContent = "--";
    refs.latestExpectedText.textContent = "--";
    refs.latestActualText.textContent = "--";
    refs.latestPassedText.textContent = "--";
    refs.latestScoreText.textContent = "--";
    refs.latestMarginText.textContent = "--";
    refs.latestTimeText.textContent = "--";
    refs.mailAttemptText.textContent = "--";
    refs.mailSentText.textContent = "--";
    refs.mailErrorText.textContent = "--";
    return;
  }

  refs.latestFlowText.textContent = formatFlow(latest.flow);
  refs.latestExpectedText.textContent = latest.expected_speaker_id || "(空)";
  refs.latestActualText.textContent = `${latest.status} / ${latest.speaker_id}`;
  refs.latestPassedText.textContent = latest.expected_speaker_id ? (latest.passed ? "通过" : "未通过") : "无对比";
  refs.latestScoreText.textContent = formatFixed(latest.score);
  refs.latestMarginText.textContent = formatFixed(latest.margin);
  refs.latestTimeText.textContent = formatDateTime(latest.created_at);
  refs.mailAttemptText.textContent = latest.notification_attempted ? "已尝试" : "未尝试";
  refs.mailSentText.textContent = latest.notification_sent ? "已发送" : "未发送";
  refs.mailErrorText.textContent = latest.notification_error || "无";
}

function renderHistory() {
  if (!state.history.length) {
    refs.historyTableBody.innerHTML = '<tr><td colspan="6" class="empty-cell"><div class="empty-state">暂无测试历史。完成一次测试后，这里会自动追加记录。</div></td></tr>';
    return;
  }

  refs.historyTableBody.innerHTML = state.history
    .map((item) => {
      const passedText = item.expected_speaker_id ? (item.passed ? "通过" : "未通过") : "无对比";
      const mailText = item.notification_attempted
        ? item.notification_sent
          ? "已发送"
          : item.notification_error || "发送失败"
        : "未尝试";
      return `
        <tr>
          <td>${escapeHtml(formatDateTime(item.created_at))}</td>
          <td><span class="history-flow ${escapeHtml(item.flow)}">${escapeHtml(formatFlow(item.flow))}</span></td>
          <td>${escapeHtml(item.expected_speaker_id || "(空)")}</td>
          <td>${escapeHtml(`${item.status} / ${item.speaker_id}`)}</td>
          <td><span class="history-pass ${item.passed ? "pass" : "fail"}">${escapeHtml(passedText)}</span></td>
          <td>${escapeHtml(mailText)}</td>
        </tr>
      `;
    })
    .join("");
}

function hydrateLatestFromHistory() {
  if (!state.history.length) {
    if (!state.pendingRelay) {
      state.latestResult = null;
    }
    return;
  }
  state.latestResult = state.history[0];
}

function updateRelayProgress() {
  if (!state.pendingRelay) {
    renderPending();
    return;
  }

  if (state.pendingRelay.kind === "register") {
    const speaker = getSpeakerById(state.pendingRelay.speakerId);
    if (speaker && speaker.last_source === "mcu-relay" && speaker.active) {
      state.pendingRelay = null;
      setPhase("MCU 中转注册完成");
      pushActivity(`MCU 中转注册完成：${speaker.speaker_id}`);
    }
  } else {
    const match = state.history.find(
      (item) => item.event_id > state.pendingRelay.baselineEventId && item.flow === "mcu-relay" && item.expected_speaker_id === state.pendingRelay.speakerId
    );
    if (match) {
      state.pendingRelay = null;
      state.latestResult = match;
      setPhase(match.passed ? "MCU 中转测试完成" : "MCU 中转测试未通过");
      pushActivity(`MCU 中转测试已回传：${match.expected_speaker_id || "(空)"} -> ${match.speaker_id}`);
    }
  }
  renderPending();
}

async function refreshData({ silent = false } = {}) {
  try {
    const [speakers, history, systemStatus] = await Promise.all([
      api.getSpeakers(),
      api.getIdentifyEvents(HISTORY_LIMIT),
      api.getSystemStatus(),
    ]);
    state.speakers = speakers;
    state.history = history;
    state.systemStatus = systemStatus;
    hydrateLatestFromHistory();
    renderSpeakers();
    renderHistory();
    updateRelayProgress();
    renderLatestResult();
    renderBackendStatus();
    if (!silent) {
      pushActivity("界面数据已刷新");
    }
  } catch (error) {
    if (!silent) {
      pushActivity(`刷新失败：${error.message}`);
    }
    throw error;
  }
}

function downmixToMono(chunks, totalLength) {
  const mono = new Float32Array(totalLength);
  let offset = 0;
  for (const chunk of chunks) {
    mono.set(chunk, offset);
    offset += chunk.length;
  }
  return mono;
}

function encodeWav(samples, sampleRate) {
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(buffer);

  function writeString(offset, value) {
    for (let idx = 0; idx < value.length; idx += 1) {
      view.setUint8(offset + idx, value.charCodeAt(idx));
    }
  }

  writeString(0, "RIFF");
  view.setUint32(4, 36 + samples.length * 2, true);
  writeString(8, "WAVE");
  writeString(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeString(36, "data");
  view.setUint32(40, samples.length * 2, true);

  let offset = 44;
  for (let idx = 0; idx < samples.length; idx += 1) {
    const clamped = Math.max(-1, Math.min(1, samples[idx]));
    const value = clamped < 0 ? clamped * 0x8000 : clamped * 0x7fff;
    view.setInt16(offset, value, true);
    offset += 2;
  }

  return new Blob([buffer], { type: "audio/wav" });
}

async function recordClip(seconds, label) {
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    throw new Error("当前浏览器不支持麦克风录音");
  }

  setPhase(`准备录音：${label}`);
  pushActivity(`请求麦克风权限：${label}`);
  const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
  const AudioContextCtor = window.AudioContext || window.webkitAudioContext;
  const audioContext = new AudioContextCtor();
  const source = audioContext.createMediaStreamSource(stream);
  const processor = audioContext.createScriptProcessor(4096, 1, 1);
  const chunks = [];
  let totalLength = 0;

  processor.onaudioprocess = (event) => {
    const input = event.inputBuffer.getChannelData(0);
    const copy = new Float32Array(input.length);
    copy.set(input);
    chunks.push(copy);
    totalLength += copy.length;
  };

  source.connect(processor);
  processor.connect(audioContext.destination);

  setPhase(`录音中：${label}`);
  let remainingMs = seconds * 1000;
  setCountdown(`${(remainingMs / 1000).toFixed(1)}s`);
  const countdownTimer = window.setInterval(() => {
    remainingMs -= 200;
    const remaining = Math.max(0, remainingMs / 1000);
    setCountdown(`${remaining.toFixed(1)}s`);
  }, 200);

  try {
    await new Promise((resolve) => window.setTimeout(resolve, seconds * 1000));
  } finally {
    window.clearInterval(countdownTimer);
    processor.disconnect();
    source.disconnect();
    stream.getTracks().forEach((track) => track.stop());
    await audioContext.close();
  }

  setCountdown("-");
  pushActivity(`录音完成：${label}`);
  return encodeWav(downmixToMono(chunks, totalLength), audioContext.sampleRate);
}

async function recordRegisterClips() {
  const clips = [];
  for (let idx = 0; idx < REGISTER_CLIPS; idx += 1) {
    clips.push(await recordClip(RECORD_SECONDS, `注册片段 ${idx + 1}/${REGISTER_CLIPS}`));
  }
  return clips;
}

async function captureBrowserLocation() {
  if (!navigator.geolocation || typeof navigator.geolocation.getCurrentPosition !== "function") {
    return null;
  }

  return new Promise((resolve) => {
    navigator.geolocation.getCurrentPosition(
      (position) => {
        resolve({
          label: "browser-geolocation",
          latitude: position.coords.latitude,
          longitude: position.coords.longitude,
          accuracy_m: position.coords.accuracy,
        });
      },
      () => resolve(null),
      {
        enableHighAccuracy: true,
        timeout: 3000,
        maximumAge: 60000,
      }
    );
  });
}

async function ensureSpeakerCleared(speakerId) {
  await api.deleteSpeaker(speakerId);
  pushActivity(`已清理同名旧 speaker：${speakerId}`);
}

function normalizeLatestResult(payload, flow) {
  return {
    flow,
    expected_speaker_id: payload.expected_speaker_id || "",
    status: payload.status,
    speaker_id: payload.speaker_id,
    score: payload.score,
    margin: payload.margin,
    passed: Boolean(payload.passed),
    notification_attempted: Boolean(payload.notification_attempted),
    notification_sent: Boolean(payload.notification_sent),
    notification_error: payload.notification_error || "",
    created_at: new Date().toISOString(),
  };
}

async function handleDirectRegister() {
  const speakerId = readSpeakerId();
  setBusy(true);
  try {
    state.pendingRelay = null;
    renderPending();
    setPhase("开始 PC 直接注册");
    pushActivity(`开始 PC 直接注册：${speakerId}`);
    await ensureSpeakerCleared(speakerId);
    const clips = await recordRegisterClips();
    const payload = await api.uploadSamples(speakerId, clips);
    const latest = payload[payload.length - 1];
    await refreshData({ silent: true });
    setPhase("PC 直接注册完成");
    pushActivity(`PC 直接注册完成：${speakerId}，样本 ${latest.accepted}/${latest.required}`);
  } finally {
    setCountdown("-");
    setBusy(false);
  }
}

async function handleRelayRegister() {
  const speakerId = readSpeakerId();
  setBusy(true);
  try {
    setPhase("开始导出 MCU 注册数据");
    pushActivity(`开始导出 MCU 注册数据：${speakerId}`);
    await ensureSpeakerCleared(speakerId);
    const clips = await recordRegisterClips();
    const payload = await api.exportRegister(speakerId, clips);
    state.pendingRelay = { kind: "register", speakerId };
    renderPending();
    await refreshData({ silent: true });
    setPhase("已导出 MCU 注册数据");
    pushActivity(`MCU 注册导出完成：${speakerId}，已写入 ${payload.output_c}`);
  } finally {
    setCountdown("-");
    setBusy(false);
  }
}

async function handleDirectTest() {
  const speakerId = readSpeakerId({ requireActive: true });
  setBusy(true);
  try {
    state.pendingRelay = null;
    renderPending();
    setPhase("开始 PC 直接测试");
    pushActivity(`开始 PC 直接测试：${speakerId}`);
    const blob = await recordClip(RECORD_SECONDS, "直接测试");
    const location = await captureBrowserLocation();
    const payload = await api.identify(speakerId, blob, location);
    state.latestResult = normalizeLatestResult(payload, "pc-direct");
    renderLatestResult();
    await refreshData({ silent: true });
    setPhase(payload.passed ? "PC 直接测试通过" : "PC 直接测试未通过");
    pushActivity(`PC 直接测试完成：${speakerId} -> ${payload.speaker_id}`);
  } finally {
    setCountdown("-");
    setBusy(false);
  }
}

async function handleRelayTest() {
  const speakerId = readSpeakerId();
  setBusy(true);
  try {
    setPhase("开始导出 MCU 测试数据");
    pushActivity(`开始导出 MCU 测试数据：${speakerId}`);
    const blob = await recordClip(RECORD_SECONDS, "MCU 中转测试");
    const payload = await api.exportIdentify(speakerId, blob);
    state.pendingRelay = {
      kind: "identify",
      speakerId,
      baselineEventId: state.history.length ? state.history[0].event_id : 0,
    };
    renderPending();
    await refreshData({ silent: true });
    setPhase("已导出 MCU 测试数据");
    pushActivity(`MCU 测试导出完成：${speakerId}，已写入 ${payload.output_c}`);
  } finally {
    setCountdown("-");
    setBusy(false);
  }
}

function waitForDialog(dialog) {
  if (typeof dialog.showModal !== "function") {
    return Promise.resolve(window.confirm("确认清空全部运行数据？"));
  }

  dialog.showModal();
  return new Promise((resolve) => {
    dialog.addEventListener(
      "close",
      () => {
        resolve(dialog.returnValue === "confirm");
      },
      { once: true }
    );
  });
}

async function handleReset() {
  const confirmed = await waitForDialog(refs.confirmDialog);
  if (!confirmed) {
    pushActivity("已取消清空运行数据");
    return;
  }

  setBusy(true);
  try {
    setPhase("正在清空运行数据");
    const payload = await api.resetAll();
    state.pendingRelay = null;
    state.latestResult = null;
    state.history = [];
    state.speakers = [];
    renderPending();
    renderLatestResult();
    renderHistory();
    renderSpeakers();
    await refreshData({ silent: true });
    setPhase("运行数据已清空");
    pushActivity(`运行数据已清空：speakers=${payload.deleted_speakers}，history=${payload.deleted_identify_events}`);
  } finally {
    setBusy(false);
  }
}

async function handleRefresh() {
  setPhase("刷新中");
  await refreshData();
  setPhase(state.pendingRelay ? refs.pendingText.textContent : "空闲");
}

async function guardAction(action) {
  try {
    await action();
  } catch (error) {
    setPhase("执行失败");
    setCountdown("-");
    pushActivity(`错误：${error.message}`);
    setBusy(false);
  }
}

function attachEvents() {
  refs.speakerIdInput.addEventListener("input", () => {
    renderSpeakers();
  });
  refs.refreshBtn.addEventListener("click", () => void guardAction(handleRefresh));
  refs.registerDirectBtn.addEventListener("click", () => void guardAction(handleDirectRegister));
  refs.registerRelayBtn.addEventListener("click", () => void guardAction(handleRelayRegister));
  refs.testDirectBtn.addEventListener("click", () => void guardAction(handleDirectTest));
  refs.testRelayBtn.addEventListener("click", () => void guardAction(handleRelayTest));
  refs.resetDataBtn.addEventListener("click", () => void guardAction(handleReset));
}

function startPolling() {
  if (state.pollTimer) {
    window.clearInterval(state.pollTimer);
  }
  state.pollTimer = window.setInterval(() => {
    void refreshData({ silent: true }).catch(() => null);
  }, POLL_INTERVAL_MS);
}

async function init() {
  renderActivity();
  renderPending();
  renderLatestResult();
  renderHistory();
  renderSpeakers();
  renderBackendStatus();
  attachEvents();
  setPhase("初始化中");
  await refreshData({ silent: true });
  setPhase("空闲");
  pushActivity("页面已就绪");
  startPolling();
}

void init().catch((error) => {
  setPhase("初始化失败");
  pushActivity(`初始化失败：${error.message}`);
});
