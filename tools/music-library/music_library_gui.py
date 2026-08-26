#!/usr/bin/env python3
"""Tkinter desktop manager for the Mini Synth device music library."""

from __future__ import annotations

from dataclasses import asdict
import json
from pathlib import Path
import queue
import sys
import tempfile
import threading
import traceback
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import music_library as library


class MusicLibraryApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Mini Synth 音乐库管理器")
        self.root.geometry("980x680")
        self.root.minsize(760, 560)
        self.root.option_add("*tearOff", False)

        self.desired: list[library.PreparedSource] = []
        self.device_files: list[tuple[str, int]] = []
        self.project_dir: Path | None = None
        self.busy = False
        self.event_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.controls: list[tk.Widget] = []

        self.port_var = tk.StringVar()
        self.status_var = tk.StringVar(value="尚未连接设备")
        self.plan_var = tk.StringVar(value="选择歌曲后可预览同步计划。")
        self.progress_var = tk.DoubleVar(value=0)
        self.progress_text_var = tk.StringVar(value="就绪")

        self._build_ui()
        self.root.after(100, self._drain_events)
        self.refresh_ports()

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        top = ttk.Frame(self.root, padding=(12, 10, 12, 6))
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(1, weight=1)
        ttk.Label(top, text="设备串口：").grid(row=0, column=0, sticky="w")
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, state="readonly", width=24)
        self.port_combo.grid(row=0, column=1, sticky="ew", padx=(0, 8))
        refresh_ports = ttk.Button(top, text="刷新串口", command=self.refresh_ports)
        refresh_ports.grid(row=0, column=2, padx=(0, 8))
        connect = ttk.Button(top, text="连接 / 刷新状态", command=self.refresh_device)
        connect.grid(row=0, column=3)
        ttk.Label(top, textvariable=self.status_var).grid(row=1, column=0, columnspan=4, sticky="w", pady=(6, 0))
        self.controls.extend([self.port_combo, refresh_ports, connect])

        paned = ttk.Panedwindow(self.root, orient=tk.HORIZONTAL)
        paned.grid(row=1, column=0, sticky="nsew", padx=12, pady=6)

        device_panel = ttk.LabelFrame(paned, text="设备当前音乐库", padding=8)
        desired_panel = ttk.LabelFrame(paned, text=f"本地待处理歌曲（最多 {library.MAX_LIBRARY_SONGS} 首）", padding=8)
        paned.add(device_panel, weight=1)
        paned.add(desired_panel, weight=2)

        device_panel.columnconfigure(0, weight=1)
        device_panel.rowconfigure(0, weight=1)
        self.device_tree = ttk.Treeview(
            device_panel, columns=("name", "size"), show="headings", height=12, selectmode="extended"
        )
        self.device_tree.heading("name", text="设备文件名")
        self.device_tree.heading("size", text="大小")
        self.device_tree.column("name", width=190, anchor="w")
        self.device_tree.column("size", width=90, anchor="e")
        device_scroll = ttk.Scrollbar(device_panel, orient=tk.VERTICAL, command=self.device_tree.yview)
        self.device_tree.configure(yscrollcommand=device_scroll.set)
        self.device_tree.grid(row=0, column=0, sticky="nsew")
        device_scroll.grid(row=0, column=1, sticky="ns")
        delete_device_button = ttk.Button(
            device_panel, text="删除设备选中歌曲…", command=self.confirm_delete_selected
        )
        delete_device_button.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        ttk.Label(
            device_panel,
            text="这里只显示文件歌曲。固件内置歌曲属于无文件时的备用内容，不会被普通同步修改。",
            wraplength=280,
        ).grid(row=2, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.controls.append(delete_device_button)

        desired_panel.columnconfigure(0, weight=1)
        desired_panel.rowconfigure(0, weight=1)
        self.desired_tree = ttk.Treeview(
            desired_panel, columns=("order", "source", "device", "size"), show="headings", height=12, selectmode="extended"
        )
        for column, title, width, anchor in (
            ("order", "顺序", 45, "center"),
            ("source", "来源", 180, "w"),
            ("device", "设备文件名", 170, "w"),
            ("size", "大小", 75, "e"),
        ):
            self.desired_tree.heading(column, text=title)
            self.desired_tree.column(column, width=width, anchor=anchor)
        desired_scroll = ttk.Scrollbar(desired_panel, orient=tk.VERTICAL, command=self.desired_tree.yview)
        self.desired_tree.configure(yscrollcommand=desired_scroll.set)
        self.desired_tree.grid(row=0, column=0, sticky="nsew")
        desired_scroll.grid(row=0, column=1, sticky="ns")

        buttons = ttk.Frame(desired_panel)
        buttons.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        add_button = ttk.Button(buttons, text="添加文件…", command=self.add_files)
        remove_button = ttk.Button(buttons, text="移除选中", command=self.remove_selected)
        up_button = ttk.Button(buttons, text="上移", command=lambda: self.move_selected(-1))
        down_button = ttk.Button(buttons, text="下移", command=lambda: self.move_selected(1))
        clear_button = ttk.Button(buttons, text="清空列表", command=self.clear_desired)
        for index, button in enumerate((add_button, remove_button, up_button, down_button, clear_button)):
            button.grid(row=0, column=index, padx=(0, 6), pady=2)
        self.controls.extend([add_button, remove_button, up_button, down_button, clear_button])
        ttk.Label(
            desired_panel,
            text="“导入并保留”只新增/覆盖同名歌曲；“精确同步”会让设备曲库与此列表完全一致。",
            wraplength=560,
        ).grid(row=2, column=0, columnspan=2, sticky="ew", pady=(6, 0))

        bottom = ttk.Frame(self.root, padding=(12, 6, 12, 12))
        bottom.grid(row=2, column=0, sticky="ew")
        bottom.columnconfigure(0, weight=1)

        plan_box = ttk.LabelFrame(bottom, text="精确同步计划", padding=8)
        plan_box.grid(row=0, column=0, columnspan=3, sticky="ew")
        plan_box.columnconfigure(0, weight=1)
        ttk.Label(plan_box, textvariable=self.plan_var, wraplength=820, justify=tk.LEFT).grid(row=0, column=0, sticky="ew")

        action_row = ttk.Frame(bottom)
        action_row.grid(row=1, column=0, columnspan=3, sticky="ew", pady=(8, 0))
        action_row.columnconfigure(0, weight=1)
        save_button = ttk.Button(action_row, text="保存项目…", command=self.save_project)
        load_button = ttk.Button(action_row, text="打开项目…", command=self.load_project)
        import_button = ttk.Button(action_row, text="导入并保留设备歌曲", command=self.confirm_import)
        reorder_button = ttk.Button(action_row, text="按右侧顺序重排设备歌曲…", command=self.confirm_reorder)
        sync_button = ttk.Button(action_row, text="精确同步到设备", command=self.confirm_sync)
        save_button.grid(row=0, column=0, sticky="w", padx=(0, 6))
        load_button.grid(row=0, column=1, sticky="w", padx=(0, 6))
        import_button.grid(row=0, column=2, sticky="e", padx=(0, 6))
        reorder_button.grid(row=0, column=3, sticky="e", padx=(0, 6))
        sync_button.grid(row=0, column=4, sticky="e")
        self.controls.extend([save_button, load_button, import_button, reorder_button, sync_button])

        progress = ttk.Progressbar(bottom, variable=self.progress_var, maximum=100)
        progress.grid(row=2, column=0, columnspan=3, sticky="ew", pady=(8, 2))
        ttk.Label(bottom, textvariable=self.progress_text_var).grid(row=3, column=0, columnspan=3, sticky="w")

        log_frame = ttk.LabelFrame(bottom, text="操作记录", padding=6)
        log_frame.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(6, 0))
        log_frame.columnconfigure(0, weight=1)
        self.log = tk.Text(log_frame, height=5, wrap="word", state="disabled")
        log_scroll = ttk.Scrollbar(log_frame, orient=tk.VERTICAL, command=self.log.yview)
        self.log.configure(yscrollcommand=log_scroll.set)
        self.log.grid(row=0, column=0, sticky="ew")
        log_scroll.grid(row=0, column=1, sticky="ns")

        destructive = ttk.LabelFrame(bottom, text="恢复工具（会删除全部文件歌曲）", padding=8)
        destructive.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(8, 0))
        destructive.columnconfigure(0, weight=1)
        ttk.Label(
            destructive,
            text="仅在音乐存储无法挂载或损坏时使用。它会格式化设备的 FFat 音乐分区，删除所有文件歌曲；不会刷写固件，也不会修改固件内置备用歌曲。",
            wraplength=680,
        ).grid(row=0, column=0, sticky="w")
        recovery_button = ttk.Button(destructive, text="初始化 / 修复音乐存储…", command=self.confirm_recovery)
        recovery_button.grid(row=0, column=1, sticky="e", padx=(10, 0))
        self.controls.append(recovery_button)

    def _log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text.rstrip() + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _port(self) -> str:
        port = self.port_var.get().strip()
        if not port:
            raise ValueError("请先选择或输入设备的 COM 串口，再点击刷新状态。")
        return port

    def _run_background(self, label: str, function) -> None:
        if self.busy:
            return
        self._set_busy(True)
        self.progress_var.set(0)
        self.progress_text_var.set(label)
        self._log(label)

        def worker() -> None:
            try:
                result = function()
                self.event_queue.put(("done", result))
            except Exception as exc:
                self.event_queue.put(("error", (exc, traceback.format_exc())))

        threading.Thread(target=worker, daemon=True).start()

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        for control in self.controls:
            try:
                if isinstance(control, ttk.Combobox):
                    control.configure(state="disabled" if busy else "readonly")
                else:
                    control.configure(state="disabled" if busy else "normal")
            except tk.TclError:
                pass

    def _drain_events(self) -> None:
        try:
            while True:
                kind, payload = self.event_queue.get_nowait()
                if kind == "progress":
                    event, details = payload
                    self._handle_progress(str(event), details)
                elif kind == "done":
                    self._set_busy(False)
                    self.progress_text_var.set("操作完成")
                    self._handle_done(payload)
                elif kind == "error":
                    self._set_busy(False)
                    self.progress_text_var.set("操作失败")
                    exc, trace = payload
                    self._log(trace)
                    messagebox.showerror("操作失败", self._friendly_error(exc))
        except queue.Empty:
            pass
        self.root.after(100, self._drain_events)

    def _friendly_error(self, exc: Exception) -> str:
        text = str(exc)
        lowered = text.lower()
        if "access is denied" in lowered or "permission" in lowered:
            return f"无法打开串口：{text}\n\n请关闭串口监视器或其他占用该 COM 口的软件，然后重试。"
        if "could not open port" in lowered or "filenotfounderror" in lowered:
            return f"找不到或无法打开所选串口：{text}\n\n请重新插拔设备，点击“刷新串口”，再选择正确的 COM 口。"
        if "timeout" in lowered or "no valid response" in lowered:
            return f"设备没有按时响应：{text}\n\n请确认选中的是 Mini Synth 的 USB 串口，并重新插拔后重试。"
        if "numpy" in lowered:
            return f"{text}\n\n打开命令提示符执行：py -3 -m pip install numpy"
        return f"{text}\n\n请查看下方操作记录；若设备仍可见，先点击“连接 / 刷新状态”后重试。"

    def _handle_done(self, payload: object) -> None:
        if not isinstance(payload, dict):
            return
        action = payload.get("action")
        if action != "ports":
            self.progress_var.set(100)
        if action == "ports":
            devices = payload["ports"]
            self.port_combo["values"] = devices
            if devices and self.port_var.get() not in devices:
                self.port_var.set(devices[0])
            self.progress_text_var.set(f"找到 {len(devices)} 个串口")
            self.progress_var.set(0)
        elif action == "show":
            report = payload["report"]
            self._apply_device_report(report)
            self._log("设备状态已刷新。")
        elif action == "prepared":
            added = payload["items"]
            self.desired.extend(added)
            self.project_dir = payload["project_dir"]
            self._renumber_desired_files()
            self._refresh_desired_tree()
            self._log(f"已准备 {len(added)} 首歌曲。")
        elif action == "sync":
            report = payload["report"]
            self.device_files = [(item["name"], item["size"]) for item in report["after"]]
            self._refresh_device_tree()
            self._update_plan_text()
            self._log("同步完成，最终文件名和大小校验通过。")
            messagebox.showinfo("同步完成", "设备音乐库已经与所选列表完全一致，最终校验通过。")
        elif action == "import":
            report = payload["report"]
            self.device_files = [(item["name"], item["size"]) for item in report["after"]]
            self._refresh_device_tree()
            self._update_plan_text()
            self._log(
                f"增量导入完成：保留 {len(report['preserved'])}，新增 {len(report['added'])}，"
                f"覆盖 {len(report['replaced'])}。"
            )
            messagebox.showinfo(
                "导入完成",
                f"设备原有歌曲已保留。\n新增 {len(report['added'])} 首，覆盖 {len(report['replaced'])} 首，最终校验通过。",
            )
        elif action == "reorder":
            report = payload["report"]
            self.device_files = [(item["name"], item["size"]) for item in report["after"]]
            self._refresh_device_tree()
            self._update_plan_text()
            self._log(f"设备重排完成：{len(report['renamed'])} 个文件名改变，连续编号和大小校验通过。")
            messagebox.showinfo(
                "重排完成",
                f"设备歌曲已按右侧顺序重排。\n改名 {len(report['renamed'])} 首，最终校验通过。",
            )
        elif action == "delete":
            report = payload["report"]
            self.device_files = [(item["name"], item["size"]) for item in report["files"]]
            self._refresh_device_tree()
            self._update_plan_text()
            self._log(f"已删除 {len(report['deleted'])} 首设备歌曲，最终校验通过。")
            messagebox.showinfo("删除完成", f"已删除 {len(report['deleted'])} 首设备歌曲。")
        elif action == "recovery":
            self.device_files = []
            self._refresh_device_tree()
            self._update_plan_text()
            self._log("音乐存储已初始化，全部文件歌曲已删除。")
            messagebox.showinfo("恢复完成", "音乐存储已初始化。全部文件歌曲已删除；固件未被刷写。")

    def refresh_ports(self) -> None:
        def task():
            ports = library.list_ports_data()
            return {"action": "ports", "ports": [item["device"] for item in ports]}

        self._run_background("正在查找串口…", task)

    def refresh_device(self) -> None:
        try:
            port = self._port()
        except ValueError as exc:
            messagebox.showwarning("请选择设备", str(exc))
            return

        def task():
            return {"action": "show", "report": library.show_device(port)}

        self._run_background("正在读取设备状态和音乐库…", task)

    def _apply_device_report(self, report: dict) -> None:
        status = report["status"]
        self.status_var.set(
            f"{report['port']} · 状态 {status['state_name']} · 音乐存储 {'可用' if status['mounted'] else '不可用'} · 文件 {len(report['files'])} 个"
        )
        self.device_files = [(item["name"], item["size"]) for item in report["files"]]
        self._refresh_device_tree()
        self._update_plan_text()

    def add_files(self) -> None:
        remaining = library.MAX_LIBRARY_SONGS - len(self.desired)
        if remaining <= 0:
            messagebox.showwarning("已达到上限", f"设备最多保存 {library.MAX_LIBRARY_SONGS} 首文件歌曲。请先移除一首。")
            return
        selected = filedialog.askopenfilenames(
            title=f"选择歌曲文件（还可添加 {remaining} 首）",
            filetypes=[
                ("支持的歌曲", "*.mspkg *.msp *.musicxml *.xml"),
                ("MSPKG 包", "*.mspkg *.msp"),
                ("MusicXML", "*.musicxml *.xml"),
                ("所有文件", "*.*"),
            ],
        )
        if not selected:
            return
        if len(selected) > remaining:
            messagebox.showwarning("超过上限", f"本次最多还能添加 {remaining} 首，请重新选择。")
            return

        project_dir = Path(tempfile.mkdtemp(prefix="mini-synth-gui-"))

        def progress(event: str, details: dict[str, object]) -> None:
            self.event_queue.put(("progress", (event, details)))

        def task():
            items = library.prepare_sources([Path(item) for item in selected], project_dir, progress)
            return {"action": "prepared", "items": items, "project_dir": project_dir}

        self._run_background("正在转换 / 检查所选歌曲…", task)

    def _renumber_desired_files(self) -> None:
        if not self.desired:
            return
        names = library.generate_safe_device_names([Path(item.source).name for item in self.desired])
        moves: list[tuple[library.PreparedSource, Path, Path]] = []
        for index, (item, new_name) in enumerate(zip(self.desired, names)):
            old_path = Path(item.prepared_path)
            temporary = old_path.with_name(f".renumber-{index:02d}-{item.sha256[:8]}.tmp")
            if temporary.exists():
                temporary.unlink()
            if old_path != temporary:
                old_path.rename(temporary)
            moves.append((item, temporary, temporary.with_name(new_name)))

        updated: list[library.PreparedSource] = []
        for item, temporary, new_path in moves:
            if new_path.exists():
                new_path.unlink()
            temporary.rename(new_path)
            updated.append(
                library.PreparedSource(
                    source=item.source,
                    source_type=item.source_type,
                    prepared_path=str(new_path.resolve()),
                    device_name=new_path.name,
                    size=item.size,
                    sha256=item.sha256,
                    converted=item.converted,
                    package=item.package,
                )
            )
        self.desired = updated

    def remove_selected(self) -> None:
        selected = set(self.desired_tree.selection())
        if not selected:
            return
        self.desired = [item for index, item in enumerate(self.desired) if str(index) not in selected]
        self._renumber_desired_files()
        self._refresh_desired_tree()

    def clear_desired(self) -> None:
        if self.desired and messagebox.askyesno("清空列表", "从目标列表中移除全部歌曲？这不会立刻修改设备。"):
            self.desired.clear()
            self._refresh_desired_tree()

    def move_selected(self, direction: int) -> None:
        selected = self.desired_tree.selection()
        if len(selected) != 1:
            messagebox.showinfo("请选择一首", "请只选择一首歌曲后再上移或下移。")
            return
        index = int(selected[0])
        target = index + direction
        if target < 0 or target >= len(self.desired):
            return
        self.desired[index], self.desired[target] = self.desired[target], self.desired[index]
        self._renumber_desired_files()
        self._refresh_desired_tree(select_index=target)

    def _refresh_device_tree(self) -> None:
        self.device_tree.delete(*self.device_tree.get_children())
        for index, (name, size) in enumerate(self.device_files):
            self.device_tree.insert("", "end", iid=str(index), values=(name, self._format_size(size)))

    def _refresh_desired_tree(self, select_index: int | None = None) -> None:
        self.desired_tree.delete(*self.desired_tree.get_children())
        for index, item in enumerate(self.desired):
            self.desired_tree.insert(
                "", "end", iid=str(index), values=(index + 1, Path(item.source).name, item.device_name, self._format_size(item.size))
            )
        if select_index is not None and 0 <= select_index < len(self.desired):
            self.desired_tree.selection_set(str(select_index))
            self.desired_tree.focus(str(select_index))
        self._update_plan_text()

    @staticmethod
    def _format_size(size: int) -> str:
        if size < 1024:
            return f"{size} B"
        if size < 1024 * 1024:
            return f"{size / 1024:.1f} KB"
        return f"{size / (1024 * 1024):.2f} MiB"

    def _current_plan(self) -> library.SyncPlan:
        return library.calculate_sync_plan(
            self.device_files,
            [(item.device_name, item.size) for item in self.desired],
            allow_empty=True,
        )

    @staticmethod
    def _names_summary(names: tuple[str, ...], limit: int = 3) -> str:
        if len(names) <= limit:
            return "、".join(names)
        return "、".join(names[:limit]) + f" 等 {len(names)} 个"

    def _plan_description(self, plan: library.SyncPlan) -> str:
        parts = [f"最终保留 {len(plan.desired)} 首"]
        if plan.delete:
            parts.append("删除：" + self._names_summary(plan.delete))
        if plan.add:
            parts.append("新增：" + self._names_summary(plan.add))
        if plan.replace:
            parts.append("替换：" + self._names_summary(plan.replace))
        if plan.exact:
            parts.append("同名同大小但仍会原子重写：" + self._names_summary(plan.exact))
        if not plan.delete and not plan.upload:
            parts.append("无需写入")
        return "；".join(parts) + "。"

    def _capacity_description(self) -> str:
        capacity = library.preflight_library_capacity(
            self.device_files,
            [(item.device_name, item.size) for item in self.desired],
        )
        return (
            f"容量预检通过：目标 {self._format_size(capacity.desired_bytes)}，"
            f"估算峰值 {self._format_size(capacity.estimated_peak_bytes)} / "
            f"安全上限 {self._format_size(capacity.safe_library_bytes)}。"
        )

    def _import_preview(self) -> tuple[library.ImportPlan, library.CapacityPreflight]:
        if not self.desired:
            raise ValueError("请先在右侧添加至少一首待导入歌曲")
        plan = library.calculate_import_plan(
            self.device_files,
            [(item.device_name, item.size) for item in self.desired],
        )
        capacity = library.preflight_library_capacity(
            self.device_files,
            plan.final,
            upload_names=plan.upload,
        )
        return plan, capacity

    def _import_plan_description(self, plan: library.ImportPlan) -> str:
        parts = [f"最终 {len(plan.final)} 首", f"保留 {len(plan.preserved)} 首"]
        if plan.added:
            parts.append("新增：" + self._names_summary(plan.added))
        if plan.replaced:
            parts.append("同名覆盖：" + self._names_summary(plan.replaced))
        return "；".join(parts) + "；不会删除其他设备歌曲。"

    def _reorder_preview(self) -> tuple[library.ReorderPlan, library.CapacityPreflight]:
        if not self.device_files:
            raise ValueError("请先连接设备并读取当前音乐库")
        if not self.desired:
            raise ValueError("请在右侧加入设备全部歌曲对应的本地源文件")
        plan = library.calculate_reorder_plan(
            self.device_files,
            [(item.device_name, item.size) for item in self.desired],
        )
        capacity = library.preflight_library_capacity(self.device_files, plan.final)
        return plan, capacity

    def _update_plan_text(self) -> None:
        try:
            plan = self._current_plan()
            self.plan_var.set(self._plan_description(plan) + " " + self._capacity_description())
        except Exception as exc:
            self.plan_var.set(str(exc))

    def confirm_sync(self) -> None:
        try:
            port = self._port()
            plan = self._current_plan()
            capacity_description = self._capacity_description()
        except ValueError as exc:
            messagebox.showwarning("无法同步", str(exc))
            return
        description = self._plan_description(plan)
        empty_warning = "\n\n目标列表为空：继续会删除设备上的全部文件歌曲。" if not self.desired else ""
        if not messagebox.askyesno(
            "确认同步",
            f"将使设备的文件音乐库与右侧列表完全一致。\n\n{description}\n{capacity_description}{empty_warning}\n\n这不是固件刷写。同步过程中请勿拔掉 USB。是否继续？",
        ):
            return

        def progress(event: str, details: dict[str, object]) -> None:
            self.event_queue.put(("progress", (event, details)))

        prepared_snapshot = list(self.desired)

        def task():
            report = library.sync_prepared(port, prepared_snapshot, allow_empty=not prepared_snapshot, progress=progress)
            return {"action": "sync", "report": report}

        self._run_background("正在同步设备音乐库…", task)

    def confirm_import(self) -> None:
        try:
            port = self._port()
            plan, capacity = self._import_preview()
        except ValueError as exc:
            messagebox.showwarning("无法导入", str(exc))
            return
        description = self._import_plan_description(plan)
        capacity_description = (
            f"容量预检通过：目标 {self._format_size(capacity.desired_bytes)}，"
            f"估算峰值 {self._format_size(capacity.estimated_peak_bytes)} / "
            f"安全上限 {self._format_size(capacity.safe_library_bytes)}。"
        )
        if not messagebox.askyesno(
            "确认增量导入",
            f"{description}\n\n{capacity_description}\n\n"
            "同名按忽略两位序号、扩展名和大小写匹配；覆盖时保留原顺序。\n"
            "导入过程中请勿拔掉 USB。是否继续？",
        ):
            return

        expected_current = list(self.device_files)
        prepared_snapshot = list(self.desired)

        def progress(event: str, details: dict[str, object]) -> None:
            self.event_queue.put(("progress", (event, details)))

        def task():
            report = library.import_prepared(
                port,
                prepared_snapshot,
                expected_current=expected_current,
                progress=progress,
            )
            return {"action": "import", "report": report}

        self._run_background("正在增量导入；设备原有歌曲将保留…", task)

    def confirm_reorder(self) -> None:
        try:
            port = self._port()
            plan, capacity = self._reorder_preview()
        except ValueError as exc:
            messagebox.showwarning("无法重排", str(exc))
            return
        old_order = "\n".join(f"{index:02d}. {name}" for index, (name, _size) in enumerate(plan.current, 1))
        new_order = "\n".join(f"{index:02d}. {name}" for index, (name, _size) in enumerate(plan.final, 1))
        capacity_description = (
            f"目标 {self._format_size(capacity.desired_bytes)}；"
            f"估算峰值 {self._format_size(capacity.estimated_peak_bytes)} / "
            f"安全上限 {self._format_size(capacity.safe_library_bytes)}"
        )
        if not messagebox.askyesno(
            "确认设备重排",
            f"将使用右侧本地源重写设备全部 {len(plan.final)} 首文件歌曲。\n\n"
            f"当前顺序：\n{old_order}\n\n新顺序：\n{new_order}\n\n"
            f"改名 {len(plan.renamed)} 首；{capacity_description}。\n\n"
            "本操作不增删逻辑歌曲，但会删除旧编号文件并重新上传本地内容。"
            "过程中请勿拔掉 USB。是否继续？",
        ):
            return

        expected_current = list(self.device_files)
        prepared_snapshot = list(self.desired)

        def progress(event: str, details: dict[str, object]) -> None:
            self.event_queue.put(("progress", (event, details)))

        def task():
            report = library.reorder_prepared(
                port,
                prepared_snapshot,
                expected_current=expected_current,
                progress=progress,
            )
            return {"action": "reorder", "report": report}

        self._run_background("正在按右侧顺序重排设备歌曲…", task)

    def confirm_delete_selected(self) -> None:
        selected = sorted((int(item) for item in self.device_tree.selection()))
        if not selected:
            messagebox.showinfo("请选择设备歌曲", "请在左侧设备列表中选择一首或多首歌曲。")
            return
        try:
            port = self._port()
        except ValueError as exc:
            messagebox.showwarning("请选择设备", str(exc))
            return
        names = [self.device_files[index][0] for index in selected]
        summary = "\n".join(f"- {name}" for name in names)
        if not messagebox.askyesno(
            "确认删除设备歌曲",
            f"将从 {port} 永久删除以下 {len(names)} 首文件歌曲：\n\n{summary}\n\n"
            "不会删除固件内置备用歌曲。是否继续？",
        ):
            return

        expected_current = list(self.device_files)

        def progress(event: str, details: dict[str, object]) -> None:
            self.event_queue.put(("progress", (event, details)))

        def task():
            report = library.delete_device_files(
                port,
                names,
                expected_current=expected_current,
                progress=progress,
            )
            return {"action": "delete", "report": report}

        self._run_background("正在删除选中的设备歌曲…", task)

    def _handle_progress(self, event: str, details: dict[str, object]) -> None:
        if event == "prepare_start":
            index, total = int(details["index"]), int(details["total"])
            self.progress_var.set((index - 1) * 100 / max(total, 1))
            self.progress_text_var.set(f"准备 {index}/{total}：{Path(str(details['source'])).name}")
        elif event == "prepare_done":
            self._log(f"已通过预检：{details['item']['device_name']}")
        elif event == "capacity":
            raw = details["capacity"]
            if isinstance(raw, dict):
                self._log(
                    f"容量预检通过：估算峰值 {self._format_size(int(raw['estimated_peak_bytes']))} / "
                    f"安全上限 {self._format_size(int(raw['safe_library_bytes']))}"
                )
        elif event == "plan":
            raw = details["plan"]
            if not isinstance(raw, dict):
                return
            plan = library.SyncPlan(
                desired=tuple(tuple(item) for item in raw["desired"]),
                current=tuple(tuple(item) for item in raw["current"]),
                delete=tuple(raw["delete"]),
                add=tuple(raw["add"]),
                replace=tuple(raw["replace"]),
                exact=tuple(raw["exact"]),
                upload=tuple(raw["upload"]),
            )
            self.plan_var.set(self._plan_description(plan) + " 容量预检已通过；详情见操作记录。")
            self._log(self._plan_description(plan))
        elif event == "import_plan":
            raw = details["plan"]
            if not isinstance(raw, dict):
                return
            self._log(
                f"增量导入计划：保留 {len(raw['preserved'])}，新增 {len(raw['added'])}，"
                f"覆盖 {len(raw['replaced'])}，最终 {len(raw['final'])} 首。"
            )
        elif event == "reorder_plan":
            raw = details["plan"]
            if isinstance(raw, dict):
                self._log(
                    f"设备重排计划：改名 {len(raw['renamed'])}，"
                    f"重写 {len(raw['upload'])}，最终 {len(raw['final'])} 首。"
                )
        elif event == "delete_start":
            self.progress_text_var.set(f"正在删除：{details['name']}")
            self._log(f"删除 {details['name']}")
        elif event == "delete_done":
            self._log(f"已删除并刷新：{details['name']}")
        elif event == "upload_start":
            self.progress_text_var.set(f"正在写入 {details['index']}/{details['total']}：{details['name']}")
            self._log(f"开始写入 {details['name']}")
        elif event == "upload_progress":
            index = int(details["index"])
            count = max(int(details["count"]), 1)
            sent, total = int(details["sent"]), max(int(details["total"]), 1)
            self.progress_var.set(((index - 1) + sent / total) * 100 / count)
        elif event == "upload_done":
            self._log(f"已原子提交 {details['name']}")
        elif event == "verified":
            self.progress_var.set(100)
            self.progress_text_var.set("最终校验通过")

    def confirm_recovery(self) -> None:
        try:
            port = self._port()
        except ValueError as exc:
            messagebox.showwarning("请选择设备", str(exc))
            return
        message = (
            "此操作会格式化 Mini Synth 的 FFat 音乐分区，并永久删除全部文件歌曲。\n\n"
            "不会刷写固件，也不会删除固件内置备用歌曲。仅在音乐存储无法挂载/损坏时使用。\n\n"
            "若确认继续，请输入：ERASE-MUSIC-LIBRARY"
        )
        confirmation = simpledialog.askstring("初始化 / 修复音乐存储", message, parent=self.root)
        if confirmation is None:
            return
        if confirmation != "ERASE-MUSIC-LIBRARY":
            messagebox.showerror("确认文字不正确", "未执行任何操作。必须逐字输入 ERASE-MUSIC-LIBRARY。")
            return
        if not messagebox.askyesno("最后确认", f"将删除 {port} 上全部文件歌曲。确定执行？"):
            return

        def task():
            report = library.initialize_device_storage(port, confirmation)
            return {"action": "recovery", "report": report}

        self._run_background("正在初始化音乐存储；请勿拔掉 USB…", task)

    def save_project(self) -> None:
        if not self.desired:
            messagebox.showinfo("没有内容", "请先添加歌曲，再保存项目。")
            return
        filename = filedialog.asksaveasfilename(
            title="保存音乐库项目", defaultextension=".json", filetypes=[("JSON 项目", "*.json")]
        )
        if not filename:
            return
        data = {"format": "mini-synth-music-library-project", "version": 1, "sources": [item.source for item in self.desired]}
        Path(filename).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        self._log(f"项目已保存：{filename}")

    def load_project(self) -> None:
        filename = filedialog.askopenfilename(title="打开音乐库项目", filetypes=[("JSON 项目", "*.json"), ("所有文件", "*.*")])
        if not filename:
            return
        try:
            data = json.loads(Path(filename).read_text(encoding="utf-8"))
            if data.get("format") != "mini-synth-music-library-project" or data.get("version") != 1:
                raise ValueError("不是受支持的 Mini Synth 音乐库项目")
            sources = data.get("sources")
            if not isinstance(sources, list) or not all(isinstance(item, str) for item in sources):
                raise ValueError("项目中的 sources 格式不正确")
            if len(sources) > library.MAX_LIBRARY_SONGS:
                raise ValueError(f"项目超过 {library.MAX_LIBRARY_SONGS} 首歌曲")
        except Exception as exc:
            messagebox.showerror("无法打开项目", str(exc))
            return
        project_dir = Path(tempfile.mkdtemp(prefix="mini-synth-gui-project-"))

        def progress(event: str, details: dict[str, object]) -> None:
            self.event_queue.put(("progress", (event, details)))

        def task():
            items = library.prepare_sources([Path(item) for item in sources], project_dir, progress)
            return {"action": "prepared", "items": items, "project_dir": project_dir, "replace": True}

        self.desired.clear()
        self._refresh_desired_tree()
        self._run_background("正在打开项目并重新检查歌曲…", task)


def main() -> int:
    root = tk.Tk()
    try:
        ttk.Style(root).theme_use("vista")
    except tk.TclError:
        pass
    MusicLibraryApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
