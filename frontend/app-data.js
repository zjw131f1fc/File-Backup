export const demoTasks = [
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

export const ACTIVE_STATUSES = new Set(["PENDING", "RUNNING"]);
export const FAILURE_STATUSES = new Set(["FAILED", "PARTIAL_SUCCESS"]);
export const TERMINAL_STATUSES = new Set(["SUCCESS", "PARTIAL_SUCCESS", "FAILED", "CANCELLED"]);

const apiBase = new URLSearchParams(window.location.search).get("api") ||
  window.BACKUP_API_BASE || "";

export const api = {
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
  createBackup(payload) {
    return this.request("/api/backup", { method: "POST", body: JSON.stringify(payload) });
  },
  createRestore(payload) {
    return this.request("/api/restore", { method: "POST", body: JSON.stringify(payload) });
  },
  cancel(id) {
    return this.request(`/api/tasks/${encodeURIComponent(id)}/cancel`, { method: "POST", body: "{}" });
  },
  entries(path) {
    return this.request(`/api/filesystem/entries?path=${encodeURIComponent(path)}`);
  }
};

export function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>'"]/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;"
  }[character]));
}

export function formatBytes(bytes) {
  if (!bytes) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  return `${(bytes / (1024 ** index)).toFixed(index ? 1 : 0)} ${units[index]}`;
}

export function formatTime(value) {
  if (!value) return "--";
  return new Intl.DateTimeFormat("zh-CN", { hour: "2-digit", minute: "2-digit" }).format(new Date(value));
}

export function formatDateTime(value) {
  if (!value) return "--";
  return new Intl.DateTimeFormat("zh-CN", { month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" }).format(new Date(value));
}

export function statusLabel(status) {
  return ({
    PENDING: "等待中", RUNNING: "执行中", SUCCESS: "已完成",
    PARTIAL_SUCCESS: "部分成功", FAILED: "失败", CANCELLED: "已取消"
  })[status] || status || "未知";
}

export function typeLabel(type) { return type === "restore" ? "恢复" : "备份"; }
export function statusClass(status) { return String(status || "").toLowerCase(); }
