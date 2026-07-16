(() => {
  "use strict";

  const demoTasks = [
    {
      task_id: "demo-backup-042",
      type: "backup",
      status: "RUNNING",
      created_at: new Date(Date.now() - 1000 * 60 * 8).toISOString(),
      started_at: new Date(Date.now() - 1000 * 60 * 7).toISOString(),
      source_path: "/home/user/data",
      progress: { stage: "scanning", processed_entries: 1842, processed_bytes: 73400320, current_path: "/home/user/data/projects/report.md" },
      result: null
    },
    {
      task_id: "demo-restore-041",
      type: "restore",
      status: "SUCCESS",
      created_at: new Date(Date.now() - 1000 * 60 * 31).toISOString(),
      started_at: new Date(Date.now() - 1000 * 60 * 30).toISOString(),
      finished_at: new Date(Date.now() - 1000 * 60 * 27).toISOString(),
      progress: { stage: "completed", processed_entries: 623, processed_bytes: 21495808, current_path: "/home/user/restore" },
      result: { status: "SUCCESS", message: "restore completed successfully", error_count: 0, warning_count: 0 }
    },
    {
      task_id: "demo-backup-040",
      type: "backup",
      status: "PARTIAL_SUCCESS",
      created_at: new Date(Date.now() - 1000 * 60 * 85).toISOString(),
      started_at: new Date(Date.now() - 1000 * 60 * 84).toISOString(),
      finished_at: new Date(Date.now() - 1000 * 60 * 79).toISOString(),
      source_path: "/home/user/data",
      progress: { stage: "completed", processed_entries: 291, processed_bytes: 10485760, current_path: "/home/user/data" },
      result: { status: "PARTIAL_SUCCESS", message: "2 entries could not be read", error_count: 2, warning_count: 1 }
    }
  ];

  const state = {
    view: "dashboard",
    mode: "backup",
    tasks: demoTasks,
    activities: [],
    apiOnline: false,
    bannerDismissed: false,
    selectedTask: null,
    pickerTarget: null,
    pickerPath: "/"
  };

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

  const apiBase = new URLSearchParams(window.location.search).get("api") ||
    window.BACKUP_API_BASE || "";

  const api = {
    async request(path, options = {}) {
      const response = await fetch(`${apiBase}${path}`, {
        headers: { "Content-Type": "application/json", ...(options.headers || {}) },
        ...options
      });
      let body = null;
      try { body = await response.json(); } catch (_) { body = null; }
      if (!response.ok) {
        const message = body?.error?.message || `请求失败（${response.status}）`;
        const error = new Error(message);
        error.status = response.status;
        error.body = body;
        throw error;
      }
      return body;
    },
    health() { return this.request("/api/health"); },
    roots() { return this.request("/api/filesystem/roots"); },
    tasks() { return this.request("/api/tasks"); },
    createDirectory(parentPath, name) {
      return this.request("/api/filesystem/directories", {
        method: "POST",
        body: JSON.stringify({ parent_path: parentPath, name })
      });
    },
    createBackup(payload) { return this.request("/api/backup", { method: "POST", body: JSON.stringify(payload) }); },
    createRestore(payload) { return this.request("/api/restore", { method: "POST", body: JSON.stringify(payload) }); },
    task(id) { return this.request(`/api/tasks/${encodeURIComponent(id)}`); },
    cancel(id) { return this.request(`/api/tasks/${encodeURIComponent(id)}/cancel`, { method: "POST", body: "{}" }); },
    entries(path) { return this.request(`/api/filesystem/entries?path=${encodeURIComponent(path)}`); }
  };

  function escapeHtml(value) {
    return String(value ?? "").replace(/[&<>'"]/g, character => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" }[character]));
  }

  function formatBytes(bytes) {
    if (!bytes) return "0 B";
    const units = ["B", "KB", "MB", "GB", "TB"];
    const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
    return `${(bytes / (1024 ** index)).toFixed(index ? 1 : 0)} ${units[index]}`;
  }

  function formatTime(value) {
    if (!value) return "--";
    const date = new Date(value);
    return new Intl.DateTimeFormat("zh-CN", { hour: "2-digit", minute: "2-digit" }).format(date);
  }

  function formatDateTime(value) {
    if (!value) return "--";
    return new Intl.DateTimeFormat("zh-CN", { month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" }).format(new Date(value));
  }

  function statusLabel(status) {
    return ({ PENDING: "等待中", RUNNING: "执行中", SUCCESS: "已完成", PARTIAL_SUCCESS: "部分成功", FAILED: "失败", CANCELLED: "已取消" })[status] || status || "未知";
  }

  function typeLabel(type) { return type === "restore" ? "恢复" : "备份"; }
  function statusClass(status) { return String(status || "").toLowerCase(); }

  function taskTimestamp(task) {
    return Date.parse(task.finished_at || task.started_at || task.created_at || "") || 0;
  }

  function sortedBackups(successOnly = false) {
    return state.tasks
      .filter(task => task.type === "backup" && (!successOnly || task.status === "SUCCESS"))
      .sort((left, right) => taskTimestamp(right) - taskTimestamp(left));
  }

  function taskPath(task) {
    return task.progress?.current_path || (task.type === "backup" ? "等待保护目录" : "等待恢复点");
  }

  function directoryOf(path) {
    const separator = path.lastIndexOf("/");
    return separator > 0 ? path.slice(0, separator) : "/";
  }

  function displayTaskPath(task) {
    const path = taskPath(task);
    return task.type === "backup" && task.status === "SUCCESS"
      ? `保存位置 · ${directoryOf(path)}`
      : path;
  }

  function taskProgress(task) {
    if (task.status === "SUCCESS") return 100;
    if (task.status === "FAILED" || task.status === "CANCELLED" || task.status === "PARTIAL_SUCCESS") return 100;
    if (task.status === "PENDING") return 0;
    const entries = Number(task.progress?.processed_entries || 0);
    return Math.min(94, Math.max(8, Math.round(entries / 25)));
  }

  function addActivity(task, message) {
    state.activities.unshift({ task_id: task.task_id, status: task.status, message, time: new Date().toISOString() });
    state.activities = state.activities.slice(0, 8);
  }

  function setServiceState(online) {
    state.apiOnline = online;
    const label = online ? "在线" : "演示模式";
    const dotClass = online ? "is-online" : "is-offline";
    const apiStatus = $("#api-status");
    apiStatus.classList.toggle("is-online", online);
    apiStatus.classList.toggle("is-offline", !online);
    apiStatus.innerHTML = `<span class="state-dot ${dotClass}"></span><span>${label}</span>`;
    $("#sidebar-state-dot").className = `state-dot ${dotClass}`;
    $("#sidebar-state-text").textContent = label;
    $("#metric-service").textContent = online ? "在线" : "离线";
    $("#metric-service-note").textContent = online ? "API 响应正常" : "使用演示数据";
    const banner = $("#api-banner");
    banner.classList.toggle("is-hidden", online || state.bannerDismissed);
  }

  function render() {
    $$('[data-view-panel]').forEach(panel => panel.classList.toggle("is-active", panel.dataset.viewPanel === state.view));
    $$('[data-view="dashboard"], [data-view="create"]').forEach(button => {
      if (!button.classList.contains("nav-item")) return;
      button.classList.toggle("is-active", button.dataset.view === state.view && (state.view !== "create" || button.dataset.mode === state.mode));
    });
    $("#topbar-location").textContent = state.view === "dashboard" ? "备份概览" : (state.mode === "restore" ? "恢复数据" : "创建备份");
    renderProtection();
    renderMetrics();
    renderTasks();
    renderRestorePoints();
    renderActivity();
    renderCreateForm();
    renderTaskDetail();
  }

  function renderMetrics() {
    const active = state.tasks.filter(task => ["PENDING", "RUNNING"].includes(task.status)).length;
    const completed = state.tasks.filter(task => task.type === "backup" && task.status === "SUCCESS").length;
    const attention = state.tasks.filter(task => ["FAILED", "PARTIAL_SUCCESS"].includes(task.status)).length;
    $("#metric-active").textContent = active;
    $("#metric-completed").textContent = completed;
    $("#metric-attention").textContent = attention;
  }

  function renderProtection() {
    const latest = sortedBackups(true)[0];
    const activeBackup = state.tasks.some(task => task.type === "backup" && ["PENDING", "RUNNING"].includes(task.status));
    const title = $("#protection-title");
    const description = $("#protection-description");
    const latestLabel = $("#protection-latest");
    const restoreButton = $("#restore-latest");
    if (latest) {
      title.textContent = "最近一次备份已完成";
      description.textContent = "最近一次备份已经保存，可以随时恢复数据。";
      latestLabel.textContent = formatDateTime(latest.finished_at || latest.created_at);
      restoreButton.classList.remove("is-hidden");
      restoreButton.dataset.taskId = latest.task_id;
    } else if (activeBackup) {
      title.textContent = "正在创建第一份备份";
      description.textContent = "备份完成后，这里会出现可用的恢复点。";
      latestLabel.textContent = "进行中";
      restoreButton.classList.add("is-hidden");
    } else {
      title.textContent = "还没有备份记录";
      description.textContent = "创建一次备份后，这里会显示最近一次成功保护的数据。";
      latestLabel.textContent = "--";
      restoreButton.classList.add("is-hidden");
    }
  }

  function renderTasks() {
    const body = $("#task-table-body");
    const empty = $("#task-empty");
    if (!state.tasks.length) {
      body.innerHTML = "";
      empty.classList.remove("is-hidden");
      return;
    }
    empty.classList.add("is-hidden");
    body.innerHTML = state.tasks.slice(0, 12).map(task => {
      const progress = taskProgress(task);
      const path = displayTaskPath(task);
      return `<tr data-task-id="${escapeHtml(task.task_id)}">
        <td><div class="task-name"><span class="task-type-mark ${task.type === "restore" ? "restore" : ""}">${task.type === "restore" ? "↓" : "↑"}</span><div><strong>${task.type === "restore" ? "恢复数据" : "保护目录"}</strong><small title="${escapeHtml(path)}">${escapeHtml(path)}</small></div></div></td>
        <td>${task.type === "restore" ? "恢复" : "备份"}</td>
        <td><span class="status-pill ${statusClass(task.status)}">${escapeHtml(statusLabel(task.status))}</span></td>
        <td class="progress-cell"><div class="progress-track"><div class="progress-value" style="width:${progress}%"></div></div><span class="progress-text">${progress}% · ${escapeHtml(task.progress?.stage || "queued")}</span></td>
        <td>${escapeHtml(formatTime(task.finished_at || task.started_at || task.created_at))}</td>
        <td><button class="row-action" data-action="open-task" data-task-id="${escapeHtml(task.task_id)}" type="button" aria-label="查看任务详情" title="查看任务详情">›</button></td>
      </tr>`;
    }).join("");
  }

  function renderRestorePoints() {
    const container = $("#restore-points");
    const points = sortedBackups(true).slice(0, 5);
    if (!points.length) {
      container.innerHTML = '<div class="restore-point-empty">完成备份后，恢复点会显示在这里。</div>';
      return;
    }
    container.innerHTML = points.map(task => `<div class="restore-point">
      <span class="restore-point-mark" aria-hidden="true">✓</span>
      <div class="restore-point-copy"><strong>备份 · ${escapeHtml(formatDateTime(task.finished_at || task.created_at))}</strong><small title="${escapeHtml(directoryOf(taskPath(task)))}">保存位置 · ${escapeHtml(directoryOf(taskPath(task)))}</small></div>
      <button class="restore-point-action" data-action="restore-task" data-task-id="${escapeHtml(task.task_id)}" type="button">恢复</button>
    </div>`).join("");
  }

  function renderActivity() {
    const activity = state.activities.length ? state.activities : state.tasks.map(task => ({
      task_id: task.task_id,
      status: task.status,
      message: task.result?.message || `${typeLabel(task.type)}任务${statusLabel(task.status)}`,
      time: task.finished_at || task.started_at || task.created_at
    }));
    $("#activity-list").innerHTML = activity.slice(0, 6).map(item => `<div class="activity-item">
      <span class="activity-dot ${["FAILED", "PARTIAL_SUCCESS"].includes(item.status) ? "failed" : item.status === "PENDING" ? "pending" : ""}"></span>
      <div class="activity-copy"><strong>${escapeHtml(item.task_id)}</strong><span>${escapeHtml(item.message)}</span></div>
      <time class="activity-time">${escapeHtml(formatTime(item.time))}</time>
    </div>`).join("");
  }

  function renderCreateForm() {
    const restore = state.mode === "restore";
    $("#create-title").textContent = restore ? "恢复数据" : "创建备份";
    $("#create-subtitle").textContent = restore ? "选择恢复点和目标目录，把文件取回本机。" : "选择要保护的目录和备份保存位置。";
    $("#path-section-title").textContent = restore ? "恢复位置" : "保护位置";
    $("#backup-fields").classList.toggle("is-hidden", restore);
    $("#restore-fields").classList.toggle("is-hidden", !restore);
    $("#filter-section").classList.toggle("is-hidden", restore);
    $("#conflict-section").classList.toggle("is-hidden", !restore);
    $("#submit-task").innerHTML = restore ? '<span aria-hidden="true">↓</span> 开始恢复' : '<span aria-hidden="true">↑</span> 创建备份';
    $$(".mode-tab").forEach(button => {
      const active = button.dataset.mode === state.mode;
      button.classList.toggle("is-active", active);
      button.setAttribute("aria-selected", active ? "true" : "false");
    });
  }

  function renderTaskDetail() {
    const drawer = $("#task-drawer");
    const task = state.selectedTask;
    if (!task) {
      drawer.classList.remove("is-open");
      drawer.setAttribute("aria-hidden", "true");
      $("#drawer-backdrop").classList.add("is-hidden");
      return;
    }
    const progress = taskProgress(task);
    const canCancel = ["PENDING", "RUNNING"].includes(task.status);
    $("#task-detail-content").innerHTML = `<div class="detail-summary">
      <span class="detail-id">${escapeHtml(task.task_id)}</span>
      <div class="detail-status"><span class="status-pill ${statusClass(task.status)}">${escapeHtml(statusLabel(task.status))}</span></div>
    </div>
    <div class="detail-grid">
      <div><span class="detail-label">类型</span><strong class="detail-value">${escapeHtml(typeLabel(task.type))}</strong></div>
      <div><span class="detail-label">开始时间</span><strong class="detail-value">${escapeHtml(formatDateTime(task.started_at || task.created_at))}</strong></div>
      <div><span class="detail-label">处理条目</span><strong class="detail-value">${Number(task.progress?.processed_entries || 0).toLocaleString("zh-CN")}</strong></div>
      <div><span class="detail-label">处理大小</span><strong class="detail-value">${escapeHtml(formatBytes(Number(task.progress?.processed_bytes || 0)))}</strong></div>
    </div>
    <div class="detail-progress"><span class="detail-label">${escapeHtml(task.progress?.stage || "queued")} · ${progress}%</span><div class="progress-track"><div class="progress-value" style="width:${progress}%"></div></div></div>
    <div class="detail-message ${["FAILED", "PARTIAL_SUCCESS"].includes(task.status) ? "failed" : ""}">${escapeHtml(task.result?.message || task.progress?.current_path || "任务已进入队列，等待执行。")}</div>
    <div class="detail-actions">${canCancel ? '<button class="button button-secondary" data-action="cancel-task" data-task-id="' + escapeHtml(task.task_id) + '" type="button">取消任务</button>' : ""}<button class="button button-primary" data-action="close-drawer" type="button">关闭</button></div>`;
    drawer.classList.add("is-open");
    drawer.setAttribute("aria-hidden", "false");
    $("#drawer-backdrop").classList.remove("is-hidden");
  }

  function showView(view, mode = state.mode) {
    state.view = view;
    if (view === "create") state.mode = mode;
    render();
    window.scrollTo({ top: 0, behavior: "smooth" });
  }

  function restoreFromTask(taskId) {
    const task = state.tasks.find(item => item.task_id === taskId);
    if (!task) return;
    showView("create", "restore");
    $("#archive-path").value = taskPath(task);
    $("#target-path").value = task.source_path || "";
    toast(task.source_path
      ? "已选择备份，默认恢复到原目录；如有需要可以修改。"
      : "已选择备份，请指定恢复到的目录。");
  }

  function splitPatterns(value) { return value.split(",").map(item => item.trim()).filter(Boolean); }

  function epochSeconds(value) {
    if (!value) return 0;
    const time = new Date(value).getTime();
    return Number.isFinite(time) ? Math.floor(time / 1000) : 0;
  }

  function buildFilterRules() {
    const min = Number($("#min-size").value || 0);
    const max = Number($("#max-size").value || 0);
    const newer = epochSeconds($("#newer-than").value);
    const older = epochSeconds($("#older-than").value);
    if (min < 0 || max < 0 || (min && max && min > max)) throw new Error("大小范围无效，请检查最小和最大值。");
    if (newer && older && newer >= older) throw new Error("时间范围无效，‘晚于’必须早于‘早于’。");
    const uids = splitPatterns($("#include-uids").value).map(Number);
    if (uids.some(uid => !Number.isInteger(uid) || uid < 0)) throw new Error("UID 必须是非负整数。");
    return {
      include_paths: splitPatterns($("#include-paths").value),
      exclude_paths: splitPatterns($("#exclude-paths").value),
      include_types: $$('[name="include_types"]:checked').map(input => input.value),
      include_names: splitPatterns($("#include-names").value),
      exclude_names: splitPatterns($("#exclude-names").value),
      newer_than_sec: newer,
      older_than_sec: older,
      min_size: min,
      max_size: max,
      include_uids: uids
    };
  }

  function demoCreateTask(type, payload) {
    const id = `demo-${type}-${String(Date.now()).slice(-6)}`;
    const task = { task_id: id, type, status: "PENDING", created_at: new Date().toISOString(), source_path: type === "backup" ? payload.source_path : undefined, progress: { stage: "queued", processed_entries: 0, processed_bytes: 0, current_path: type === "backup" ? payload.source_path : payload.archive_path }, result: null };
    state.tasks.unshift(task);
    addActivity(task, `${typeLabel(type)}任务已创建`);
    render();
    window.setTimeout(() => {
      task.status = "RUNNING";
      task.started_at = new Date().toISOString();
      task.progress = { stage: type === "backup" ? "scanning" : "validating_archive", processed_entries: 24, processed_bytes: 1048576, current_path: task.progress.current_path };
      addActivity(task, `${typeLabel(type)}任务正在执行`);
      render();
    }, 700);
    window.setTimeout(() => {
      task.status = "SUCCESS";
      task.finished_at = new Date().toISOString();
      task.progress = { stage: "completed", processed_entries: 86, processed_bytes: 7340032, current_path: task.progress.current_path };
      task.result = { status: "SUCCESS", message: `${typeLabel(type)}任务已完成`, error_count: 0, warning_count: 0 };
      addActivity(task, task.result.message);
      render();
    }, 2500);
    return task;
  }

  async function submitTask(event) {
    event.preventDefault();
    try {
      let payload;
      if (state.mode === "backup") {
        const source = $("#source-path").value.trim();
        const output = $("#output-path").value.trim();
        const archiveName = $("#archive-name").value.trim();
        if (!source || !output) throw new Error("请填写源目录和输出目录。");
        payload = { source_path: source, output_path: output, filter_rules: buildFilterRules() };
        if (archiveName) payload.archive_name = archiveName;
      } else {
        const archive = $("#archive-path").value.trim();
        const target = $("#target-path").value.trim();
        if (!archive || !target) throw new Error("请填写备份文件和恢复目录。");
        payload = { archive_path: archive, target_path: target, conflict_policy: $("[name=conflict_policy]:checked").value };
      }
      let created;
      if (state.apiOnline) {
        created = state.mode === "backup" ? await api.createBackup(payload) : await api.createRestore(payload);
        await loadTasks(false);
        toast(state.mode === "backup" ? "备份已提交。" : "恢复已提交。");
      } else {
        created = demoCreateTask(state.mode, payload);
        toast("演示任务已创建。");
      }
      state.selectedTask = state.tasks.find(task => task.task_id === created?.task_id) || created;
      showView("dashboard");
    } catch (error) {
      toast(error.message || "任务提交失败。", true);
    }
  }

  async function loadTasks(showError = true) {
    if (!state.apiOnline) return;
    try {
      const data = await api.tasks();
      const summaries = data?.tasks || [];
      const detailed = await Promise.all(summaries.map(async task => {
        if (task.progress) return task;
        try { return await api.task(task.task_id); } catch (_) { return task; }
      }));
      state.tasks = detailed.map(task => ({ ...task, progress: task.progress || {}, result: task.result || null }));
      if (state.selectedTask) {
        state.selectedTask = state.tasks.find(task => task.task_id === state.selectedTask.task_id) || null;
      }
      render();
    } catch (error) {
      state.apiOnline = false;
      setServiceState(false);
      if (showError) toast("后端连接已断开，已切换到演示模式。", true);
      render();
    }
  }

  async function cancelTask(id) {
    const task = state.tasks.find(item => item.task_id === id);
    if (!task || !["PENDING", "RUNNING"].includes(task.status)) return;
    try {
      if (state.apiOnline) await api.cancel(id);
      task.status = "CANCELLED";
      task.finished_at = new Date().toISOString();
      task.result = { status: "CANCELLED", message: "任务已由用户取消", error_count: 0, warning_count: 0 };
      addActivity(task, task.result.message);
      state.selectedTask = task;
      render();
      toast("任务已取消。");
    } catch (error) { toast(error.message || "取消任务失败。", true); }
  }

  function toast(message, error = false) {
    const element = document.createElement("div");
    element.className = `toast ${error ? "error" : ""}`;
    element.textContent = message;
    $("#toast-region").append(element);
    window.setTimeout(() => element.remove(), 3800);
  }

  function openTask(id) {
    state.selectedTask = state.tasks.find(task => task.task_id === id) || null;
    renderTaskDetail();
  }

  function closeDrawer() {
    state.selectedTask = null;
    renderTaskDetail();
  }

  function demoEntries(path) {
    const normalized = path.replace(/\/+$/, "") || "/";
    const entries = normalized === "/home/user" ? [
      { name: "data", path: "/home/user/data", type: "directory" },
      { name: "restore", path: "/home/user/restore", type: "directory" },
      { name: "notes.txt", path: "/home/user/notes.txt", type: "regular_file", size: 2480 }
    ] : normalized === "/backup" ? [
      { name: "data-2026-07-16.bak", path: "/backup/data-2026-07-16.bak", type: "regular_file", size: 7340032 }
    ] : [];
    return { path: normalized, entries };
  }

  async function loadPickerPath(path) {
    const normalized = path.trim() || "/";
    try {
      const data = state.apiOnline ? await api.entries(normalized) : demoEntries(normalized);
      state.pickerPath = data.path || normalized;
      $("#picker-path").value = state.pickerPath;
      $("#picker-location").textContent = state.pickerPath;
      const entries = data.entries || [];
      $("#picker-list").innerHTML = entries.length ? entries.map(entry => `<button class="picker-entry" data-picker-path="${escapeHtml(entry.path)}" data-picker-type="${escapeHtml(entry.type)}" type="button"><span>□ ${escapeHtml(entry.name)}</span><small>${escapeHtml(entry.type === "directory" ? "目录" : formatBytes(entry.size || 0))}</small></button>`).join("") : '<div class="picker-empty">此目录没有可显示的条目。</div>';
    } catch (error) { toast(error.message || "无法读取目录。", true); }
  }

  function openPathPicker(target) {
    state.pickerTarget = target;
    const targetValue = $("#" + target)?.value.trim();
    if (targetValue) state.pickerPath = targetValue;
    $("#path-modal").classList.remove("is-hidden");
    $("#path-backdrop").classList.remove("is-hidden");
    $("#picker-path").value = state.pickerPath;
    $("#picker-new-folder").value = "";
    loadPickerPath(state.pickerPath);
  }

  function closePathPicker() {
    $("#path-modal").classList.add("is-hidden");
    $("#path-backdrop").classList.add("is-hidden");
    state.pickerTarget = null;
  }

  function selectPickerPath() {
    if (state.pickerTarget) $("#" + state.pickerTarget).value = state.pickerPath;
    closePathPicker();
  }

  async function createPickerDirectory() {
    const name = $("#picker-new-folder").value.trim();
    if (!name) {
      toast("请输入文件夹名称。", true);
      return;
    }
    if (!state.apiOnline) {
      toast("后端 API 不可用，演示模式不能创建文件夹。", true);
      return;
    }
    try {
      const created = await api.createDirectory(state.pickerPath, name);
      state.pickerPath = created.path;
      if (state.pickerTarget) $("#" + state.pickerTarget).value = state.pickerPath;
      $("#picker-new-folder").value = "";
      await loadPickerPath(state.pickerPath);
      toast("文件夹已创建。");
    } catch (error) {
      toast(error.message || "文件夹创建失败。", true);
    }
  }

  async function refresh() {
    try {
      await api.health();
      state.apiOnline = true;
      setServiceState(true);
      const roots = await api.roots();
      if (roots?.roots?.length) state.pickerPath = roots.roots[0].path;
      await loadTasks(false);
      toast("已刷新任务状态。");
    } catch (_) {
      state.apiOnline = false;
      setServiceState(false);
      toast("后端 API 暂不可用，继续使用演示数据。", true);
    }
    render();
  }

  function bindEvents() {
    document.addEventListener("click", event => {
      const viewButton = event.target.closest("[data-view]");
      if (viewButton) { showView(viewButton.dataset.view, viewButton.dataset.mode || state.mode); return; }
      const modeButton = event.target.closest("[data-mode]");
      if (modeButton && modeButton.classList.contains("mode-tab")) { state.mode = modeButton.dataset.mode; render(); return; }
      const taskRow = event.target.closest("tr[data-task-id]");
      if (taskRow && !event.target.closest("button")) { openTask(taskRow.dataset.taskId); return; }
      const pickerEntry = event.target.closest("[data-picker-path]");
      if (pickerEntry) {
        if (pickerEntry.dataset.pickerType === "directory") loadPickerPath(pickerEntry.dataset.pickerPath);
        else if (state.pickerTarget === "archive-path") {
          state.pickerPath = pickerEntry.dataset.pickerPath;
          $("#picker-path").value = state.pickerPath;
          $("#picker-location").textContent = state.pickerPath;
        } else toast("当前字段需要选择目录。", true);
        return;
      }
      const browseButton = event.target.closest("[data-browse-target]");
      if (browseButton) { openPathPicker(browseButton.dataset.browseTarget); return; }
      const action = event.target.closest("[data-action]");
      if (!action) return;
      const name = action.dataset.action;
      if (name === "refresh") refresh();
      if (name === "dismiss-banner") { state.bannerDismissed = true; $("#api-banner").classList.add("is-hidden"); }
      if (name === "open-task") openTask(action.dataset.taskId);
      if (name === "cancel-task") cancelTask(action.dataset.taskId);
      if (name === "restore-latest") restoreFromTask(action.dataset.taskId);
      if (name === "restore-task") restoreFromTask(action.dataset.taskId);
      if (name === "close-drawer") closeDrawer();
      if (name === "load-picker-path") loadPickerPath($("#picker-path").value);
      if (name === "close-path-picker") closePathPicker();
      if (name === "select-picker-path") selectPickerPath();
      if (name === "create-picker-directory") createPickerDirectory();
    });
    $("#task-form").addEventListener("submit", submitTask);
    $("#drawer-backdrop").addEventListener("click", closeDrawer);
    $("#path-backdrop").addEventListener("click", closePathPicker);
    document.addEventListener("keydown", event => {
      if (event.key === "Escape") { closeDrawer(); closePathPicker(); }
      if (event.key === "Enter" && event.target.id === "picker-path") {
        event.preventDefault();
        loadPickerPath(event.target.value);
      }
      if (event.key === "Enter" && event.target.id === "picker-new-folder") {
        event.preventDefault();
        createPickerDirectory();
      }
    });
  }

  async function init() {
    bindEvents();
    render();
    await refresh();
    window.setInterval(() => { if (state.apiOnline && state.tasks.some(task => ["PENDING", "RUNNING"].includes(task.status))) loadTasks(false); }, 1000);
  }

  init();
})();
