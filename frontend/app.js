import {
  ACTIVE_STATUSES,
  FAILURE_STATUSES,
  TERMINAL_STATUSES,
  api,
  demoTasks,
  escapeHtml,
  formatBytes,
  formatDateTime,
  formatTime,
  statusClass,
  statusLabel,
  typeLabel
} from "./app-data.js";

(() => {
  "use strict";

  // 页面唯一状态：当前视图、任务列表、后端连接状态和弹窗选择状态都从这里读取。
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

  // 简化单个元素和多个元素的 DOM 查询。
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

  // 取任务最后一次有意义的时间，用于排序任务和活动。
  function taskTimestamp(task) {
    return Date.parse(task.finished_at || task.started_at || task.created_at || "") || 0;
  }

  // 返回按时间倒序排列的备份任务，可选只保留成功备份。
  function sortedBackups(successOnly = false) {
    return state.tasks
      .filter(task => task.type === "backup" && (!successOnly || task.status === "SUCCESS"))
      .sort((left, right) => taskTimestamp(right) - taskTimestamp(left));
  }

  // 取得任务当前处理路径，缺省时显示等待提示。
  function taskPath(task) {
    return task.progress?.current_path || (task.type === "backup" ? "等待保护目录" : "等待恢复点");
  }

  // 取得路径所在目录，用于显示归档保存位置。
  function directoryOf(path) {
    const separator = path.lastIndexOf("/");
    return separator > 0 ? path.slice(0, separator) : "/";
  }

  // 根据任务类型和状态决定列表中展示源路径还是保存目录。
  function displayTaskPath(task) {
    const path = taskPath(task);
    return task.type === "backup" && task.status === "SUCCESS"
      ? `保存位置 · ${directoryOf(path)}`
      : path;
  }

  // 判断任务是否仍在等待或执行，可以被取消。
  function isActive(task) { return ACTIVE_STATUSES.has(task.status); }
  // 判断任务是否失败或部分成功，需要用户关注。
  function isFailure(task) { return FAILURE_STATUSES.has(task.status); }

  // 根据任务状态和已处理条目估算页面进度；后端没有总量时避免进度卡死。
  function taskProgress(task) {
    if (TERMINAL_STATUSES.has(task.status)) return 100;
    if (task.status === "PENDING") return 0;
    const entries = Number(task.progress?.processed_entries || 0);
    return Math.min(94, Math.max(8, Math.round(entries / 25)));
  }

  // 把一条任务变化放入最近活动列表，并限制列表长度。
  function addActivity(task, message) {
    state.activities.unshift({ task_id: task.task_id, status: task.status, message, time: new Date().toISOString() });
    state.activities = state.activities.slice(0, 8);
  }

  // 更新页面顶部和侧边栏的 API 在线/演示模式标识。
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

  // 重新渲染当前页面的所有区域。
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

  // 渲染任务数量指标：执行中、成功备份和需要关注的任务。
  function renderMetrics() {
    const active = state.tasks.filter(isActive).length;
    const completed = state.tasks.filter(task => task.type === "backup" && task.status === "SUCCESS").length;
    const attention = state.tasks.filter(isFailure).length;
    $("#metric-active").textContent = active;
    $("#metric-completed").textContent = completed;
    $("#metric-attention").textContent = attention;
  }

  // 渲染“保护状态”区域，展示最近成功备份或当前首次备份状态。
  function renderProtection() {
    const latest = sortedBackups(true)[0];
    const activeBackup = state.tasks.some(task => task.type === "backup" && isActive(task));
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

  // 渲染任务表格和每个任务的进度、状态、操作入口。
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

  // 渲染最近成功备份，作为可直接发起还原的恢复点。
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

  // 渲染任务活动时间线；没有本地活动时从任务结果生成初始内容。
  function renderActivity() {
    const activity = state.activities.length ? state.activities : state.tasks.map(task => ({
      task_id: task.task_id,
      status: task.status,
      message: task.result?.message || `${typeLabel(task.type)}任务${statusLabel(task.status)}`,
      time: task.finished_at || task.started_at || task.created_at
    }));
    $("#activity-list").innerHTML = activity.slice(0, 6).map(item => `<div class="activity-item">
      <span class="activity-dot ${FAILURE_STATUSES.has(item.status) ? "failed" : item.status === "PENDING" ? "pending" : ""}"></span>
      <div class="activity-copy"><strong>${escapeHtml(item.task_id)}</strong><span>${escapeHtml(item.message)}</span></div>
      <time class="activity-time">${escapeHtml(formatTime(item.time))}</time>
    </div>`).join("");
  }

  // 根据备份/还原模式切换表单字段和按钮文字。
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

  // 渲染任务详情抽屉，并根据状态决定是否显示取消按钮。
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
    const canCancel = isActive(task);
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
    <div class="detail-message ${isFailure(task) ? "failed" : ""}">${escapeHtml(task.result?.message || task.progress?.current_path || "任务已进入队列，等待执行。")}</div>
    <div class="detail-actions">${canCancel ? '<button class="button button-secondary" data-action="cancel-task" data-task-id="' + escapeHtml(task.task_id) + '" type="button">取消任务</button>' : ""}<button class="button button-primary" data-action="close-drawer" type="button">关闭</button></div>`;
    drawer.classList.add("is-open");
    drawer.setAttribute("aria-hidden", "false");
    $("#drawer-backdrop").classList.remove("is-hidden");
  }

  // 切换主视图，并在进入创建页时同步备份/还原模式。
  function showView(view, mode = state.mode) {
    state.view = view;
    if (view === "create") state.mode = mode;
    render();
    window.scrollTo({ top: 0, behavior: "smooth" });
  }

  // 从已有备份任务填充还原表单，并默认恢复到原源目录。
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

  // 把输入框中的逗号分隔文本转换成后端需要的字符串数组。
  function splitPatterns(value) { return value.split(",").map(item => item.trim()).filter(Boolean); }

  // 把日期输入框转换成后端使用的 Unix 秒数。
  function epochSeconds(value) {
    if (!value) return 0;
    const time = new Date(value).getTime();
    return Number.isFinite(time) ? Math.floor(time / 1000) : 0;
  }

  // 从筛选表单读取并校验所有筛选规则。
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
      include_types: $("#include-all-types").checked ? [] : $$('[name="include_types"]:checked').map(input => input.value),
      include_names: splitPatterns($("#include-names").value),
      exclude_names: splitPatterns($("#exclude-names").value),
      newer_than_sec: newer,
      older_than_sec: older,
      min_size: min,
      max_size: max,
      include_uids: uids
    };
  }

  // 根据当前模式组装备份或还原 API 请求体。
  function buildTaskPayload() {
    if (state.mode === "backup") {
      const source = $("#source-path").value.trim();
      const output = $("#output-path").value.trim();
      const archiveName = $("#archive-name").value.trim();
      if (!source || !output) throw new Error("请填写源目录和输出目录。");
      const payload = { source_path: source, output_path: output, filter_rules: buildFilterRules() };
      if (archiveName) payload.archive_name = archiveName;
      return payload;
    }

    const archive = $("#archive-path").value.trim();
    const target = $("#target-path").value.trim();
    if (!archive || !target) throw new Error("请填写备份文件和恢复目录。");
    return { archive_path: archive, target_path: target, conflict_policy: $("[name=conflict_policy]:checked").value };
  }

  // 根据当前模式选择调用备份接口还是还原接口。
  function createTask(payload) {
    return state.mode === "backup" ? api.createBackup(payload) : api.createRestore(payload);
  }

  // 后端不可用时创建一个本地演示任务，并用定时器模拟执行过程。
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

  // 处理创建表单提交：校验、调用 API/演示逻辑、刷新任务并返回概览页。
  async function submitTask(event) {
    event.preventDefault();
    try {
      const payload = buildTaskPayload();
      let created;
      if (state.apiOnline) {
        created = await createTask(payload);
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

  // 从后端重新加载任务列表，并同步当前打开的任务详情。
  async function loadTasks(showError = true) {
    if (!state.apiOnline) return;
    try {
      const data = await api.tasks();
      state.tasks = (data?.tasks || []).map(task => ({
        ...task,
        progress: task.progress || {},
        result: task.result || null
      }));
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

  // 取消任务并立即更新页面；后端在线时先发送取消请求。
  async function cancelTask(id) {
    const task = state.tasks.find(item => item.task_id === id);
    if (!task || !isActive(task)) return;
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

  // 创建短暂提示消息，用于成功、失败和连接状态反馈。
  function toast(message, error = false) {
    const element = document.createElement("div");
    element.className = `toast ${error ? "error" : ""}`;
    element.textContent = message;
    $("#toast-region").append(element);
    window.setTimeout(() => element.remove(), 3800);
  }

  // 打开指定任务的详情抽屉。
  function openTask(id) {
    state.selectedTask = state.tasks.find(task => task.task_id === id) || null;
    renderTaskDetail();
  }

  // 关闭任务详情抽屉。
  function closeDrawer() {
    state.selectedTask = null;
    renderTaskDetail();
  }

  // 返回演示模式下的固定目录内容，供路径选择器离线展示。
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

  // 加载当前目录并刷新路径选择器中的条目列表。
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

  // 打开路径选择器，并把当前输入框内容作为初始目录。
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

  // 关闭路径选择器并清除当前选择目标。
  function closePathPicker() {
    $("#path-modal").classList.add("is-hidden");
    $("#path-backdrop").classList.add("is-hidden");
    state.pickerTarget = null;
  }

  // 把路径选择器当前目录写回发起选择的表单字段。
  function selectPickerPath() {
    if (state.pickerTarget) $("#" + state.pickerTarget).value = state.pickerPath;
    closePathPicker();
  }

  // 调用后端在当前目录创建文件夹，然后刷新目录列表。
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

  // 主动检查 API、加载根目录和任务；失败时切换到演示模式。
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

  // 注册页面点击、提交、键盘和筛选控件事件。
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
    const allTypes = $("#include-all-types");
    const typeInputs = $$('[name="include_types"]');
    const syncTypeFilter = () => {
      if (allTypes.checked) {
        typeInputs.forEach(input => { input.checked = false; input.disabled = true; });
      } else if (!typeInputs.some(input => input.checked)) {
        allTypes.checked = true;
        syncTypeFilter();
      } else {
        typeInputs.forEach(input => { input.disabled = false; });
      }
    };
    allTypes.addEventListener("change", () => {
      if (!allTypes.checked) typeInputs.forEach(input => { input.checked = true; });
      syncTypeFilter();
    });
    typeInputs.forEach(input => input.addEventListener("change", syncTypeFilter));
    syncTypeFilter();
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

  // 初始化事件、首次渲染、后端连接和任务进度定时刷新。
  async function init() {
    bindEvents();
    render();
    await refresh();
    window.setInterval(() => { if (state.apiOnline && state.tasks.some(isActive)) loadTasks(false); }, 1000);
  }

  init();
})();
