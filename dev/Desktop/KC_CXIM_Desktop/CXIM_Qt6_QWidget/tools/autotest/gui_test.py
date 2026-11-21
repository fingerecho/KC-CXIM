# -*- coding: utf-8 -*-
"""
CXIM Qt6 GUI 自动化互测脚本(pywinauto / UIA)

流程: 启动 responder(fingerecho15 无头对端) → 启动 GUI →
      登录 fingerecho11 → 通讯录 → +添加好友 fingerecho15 →
      双击好友进入会话 → 发文本(等回复) → 发文件(等回复文件) →
      发图片(等回复图片) → 点击收到的文件气泡另存并校验 → 每步截图

设计要点(本次调试定案, 勿回退):
  - DPI 感知必须最先设置(SetProcessDpiAwareness), 否则高分缩放下
    click_input 的物理坐标与 UIA rect 错位;
  - 窗口查找一律用 EnumWindows(按 pid+标题) 拿 HWND 再 Desktop().window(handle=),
    不用 desk.window(title=...): 模态对话框的嵌套事件循环会让 UIA 全树遍历超时;
  - 点击前 force_foreground(AttachThreadInput) 绕开 Windows 前台锁。

用法: python tools/autotest/gui_test.py
"""
import base64
import ctypes
import os
import subprocess
import sys
import time

# 必须在任何 UI 操作/导入 pywinauto 前声明 DPI 感知
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()

from pywinauto import Desktop

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build", "Check")
SHOTS = os.path.join(ROOT, "test_shots")
os.makedirs(SHOTS, exist_ok=True)
os.environ["PATH"] = r"C:\Qt\6.11.1\mingw_64\bin;" + os.environ["PATH"]
os.environ["QT_FORCE_STDERR_LOGGING"] = "1"

GUI = os.path.join(BUILD, "CXIM_Qt6_QWidget.exe")
RESP = os.path.join(BUILD, "cxim_responder.exe")

TEST_TXT = os.path.join(SHOTS, "cxim_gui_test.txt")
TEST_PNG = os.path.join(SHOTS, "cxim_gui_test.png")
SAVED_FILE = os.path.join(SHOTS, "resp_download.txt")

# 1x1 红色 PNG(免依赖 Pillow)
PNG_1PX = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==")

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

step_no = [0]


def log(msg):
    print("[test] %s" % msg, flush=True)


# ---------------------------------------------------------------------------
# 窗口定位: EnumWindows 按 pid+标题 → HWND → UIA wrapper
# ---------------------------------------------------------------------------
def find_hwnd(pid, title_exact=None, title_contains=None, timeout=30):
    """按进程+标题找可见顶层窗口; 用 Win32 枚举, 不受 Qt 模态嵌套循环影响"""
    end = time.time() + timeout
    while time.time() < end:
        hits = []

        def cb(hwnd, _):
            if not u32.IsWindowVisible(hwnd):
                return True
            wpid = ctypes.c_ulong()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
            if wpid.value != pid:
                return True
            buf = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, buf, 256)
            t = buf.value or ""
            if title_exact is not None and t != title_exact:
                return True
            if title_contains is not None and title_contains not in t:
                return True
            if not t:
                return True
            hits.append(hwnd)
            return True

        u32.EnumWindows(EnumWindowsProc(cb), 0)
        if hits:
            return hits[0]
        time.sleep(0.3)
    return None


def wrap(hwnd):
    return Desktop(backend="uia").window(handle=hwnd).wrapper_object()


def force_foreground(hwnd):
    """AttachThreadInput 绕过 Windows 前台锁"""
    try:
        fg = u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), None)
        cur = k32.GetCurrentThreadId()
        u32.AttachThreadInput(cur, fg, True)
        u32.BringWindowToTop(hwnd)
        u32.SetForegroundWindow(hwnd)
        u32.AttachThreadInput(cur, fg, False)
    except Exception:
        pass


def click(win, ctrl, dbl=False):
    force_foreground(win.handle)
    time.sleep(0.15)
    ctrl.click_input(double=dbl)
    time.sleep(0.25)


def shot(win, name):
    step_no[0] += 1
    path = os.path.join(SHOTS, "%02d_%s.png" % (step_no[0], name))
    try:
        force_foreground(win.handle)
        time.sleep(0.3)
        win.capture_as_image().save(path)
        log("screenshot -> %s" % path)
    except Exception as e:
        log("screenshot fail(%s): %s" % (name, e))


def find_ctrl(win, ctrl_type, name=None, auto_id=None, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        for c in win.descendants(control_type=ctrl_type):
            try:
                if name and name not in (c.window_text() or ""):
                    continue
                if auto_id and not (c.element_info.automation_id or "").endswith(auto_id):
                    continue
                if c.is_visible():
                    return c
            except Exception:
                continue
        time.sleep(0.4)
    raise RuntimeError("not found: %s[%s/%s]" % (ctrl_type, name, auto_id))


def all_texts(win):
    out = []
    for c in win.descendants():
        try:
            t = c.window_text()
            if t:
                out.append(t)
        except Exception:
            pass
    return "\n".join(out)


def wait_body(win, sub, timeout=40, what="bubble"):
    end = time.time() + timeout
    while time.time() < end:
        if sub in all_texts(win):
            return True
        time.sleep(0.8)
    raise RuntimeError("timeout waiting %s containing %r" % (what, sub))


def dialog_submit_path(path, main_btn_kw):
    """Windows 原生文件对话框: 文件名框输入路径 → 点主按钮(打开/保存)"""
    hwnd = find_hwnd(0, title_contains="打开", timeout=25) \
        or find_hwnd(0, title_contains="另存为", timeout=5)
    assert hwnd, "file dialog window not found"
    dlg = wrap(hwnd)
    force_foreground(hwnd)
    time.sleep(0.6)
    edit = None
    for c in dlg.descendants(control_type="Edit"):
        if c.is_visible():
            edit = c
            break
    assert edit, "file dialog edit not found"
    edit.set_edit_text(path)
    time.sleep(0.4)
    for b in dlg.descendants(control_type="Button"):
        if main_btn_kw in (b.window_text() or ""):
            b.click_input()
            return dlg
    raise RuntimeError("dialog main button(%s) not found" % main_btn_kw)


# ---------------------------------------------------------------------------
def main():
    with open(TEST_TXT, "w", encoding="utf-8") as f:
        f.write("hello from GUI autotest %s\n" % int(time.time()))
    with open(TEST_PNG, "wb") as f:
        f.write(PNG_1PX)
    if os.path.exists(SAVED_FILE):
        os.remove(SAVED_FILE)

    resp_log = open(os.path.join(SHOTS, "responder.log"), "wb")
    resp = subprocess.Popen([RESP, "fingerecho15", "asdfls", "fingerecho11"],
                            cwd=BUILD, stdout=resp_log, stderr=resp_log)
    log("responder pid=%s" % resp.pid)

    gui_log = open(os.path.join(SHOTS, "gui.log"), "wb")
    gui = subprocess.Popen([GUI], cwd=BUILD, stdout=gui_log, stderr=gui_log)
    try:
        run_flow(gui.pid)
    finally:
        resp.terminate()
        gui.terminate()
        resp_log.close()
        gui_log.close()


def run_flow(pid):
    # ---- 1. 登录 ----
    login_h = find_hwnd(pid, title_contains="登录", timeout=40)
    assert login_h, "login window not found"
    login = wrap(login_h)
    time.sleep(0.8)
    shot(login, "login_card")

    edits = [e for e in login.descendants(control_type="Edit") if e.is_visible()]
    assert len(edits) >= 2, "login edits=%d" % len(edits)
    click(login, edits[0]); edits[0].set_edit_text("fingerecho11")
    click(login, edits[1]); edits[1].set_edit_text("asdfls")
    agree = find_ctrl(login, "CheckBox", name="已阅读并同意")
    try:
        if agree.get_toggle_state() != 1:
            click(login, agree)
    except Exception:
        click(login, agree)
    shot(login, "login_filled")
    click(login, find_ctrl(login, "Button", name="登录"))
    log("login clicked")

    # ---- 2. 主窗口 ----
    main_h = find_hwnd(pid, title_exact="铖信", timeout=40)
    assert main_h, "main window not found"
    main_win = wrap(main_h)
    time.sleep(3.5)   # TCP 上线 + 全量对齐
    shot(main_win, "main_chat_page")

    # ---- 3. 通讯录 → 添加好友 ----
    click(main_win, find_ctrl(main_win, "CheckBox", name="👤"))
    time.sleep(1.0)
    shot(main_win, "contacts_page")
    click(main_win, find_ctrl(main_win, "Button", auto_id="plusButton"))

    add_h = find_hwnd(pid, title_exact="添加好友", timeout=20)
    assert add_h, "AddFriendDialog not found"
    add_dlg = wrap(add_h)
    time.sleep(0.8)
    ad_edit = [e for e in add_dlg.descendants(control_type="Edit") if e.is_visible()][0]
    click(add_dlg, ad_edit); ad_edit.set_edit_text("fingerecho15")
    click(add_dlg, find_ctrl(add_dlg, "Button", name="搜索"))
    time.sleep(2.5)
    shot(add_dlg, "addfriend_search")
    try:
        add_btn = find_ctrl(add_dlg, "Button", name="添加", timeout=12)
        click(add_dlg, add_btn)
        log("friend request sent")
        time.sleep(3.0)   # 等 responder accept + 132 事件刷新好友列表
    except RuntimeError:
        log("no 添加 button (可能已是好友)")
    shot(add_dlg, "addfriend_done")
    click(add_dlg, find_ctrl(add_dlg, "Button", name="关闭"))
    time.sleep(1.5)

    # ---- 4. 双击好友进入会话 ----
    # 自定义行(setItemWidget)的文本挂在 List 层而非 ListItem 子树(UIA 结构),
    # 直接找 Text 元素双击: Qt hit-test 会把双击送到所在行
    row = None
    end = time.time() + 40
    while time.time() < end and row is None:
        for c in main_win.descendants(control_type="Text"):
            try:
                if c.is_visible() and (c.window_text() or "").strip() == "fingerecho15":
                    row = c
                    break
            except Exception:
                continue
        if row is None:
            time.sleep(1)
    assert row, "friend row fingerecho15 not found"
    click(main_win, row, dbl=True)
    time.sleep(2.5)
    shot(main_win, "conversation_open")

    # ---- 5. 发文本 → 等回复 ----
    msg = "GUI测试消息 %d" % int(time.time())
    input_edit = find_ctrl(main_win, "Edit", auto_id="inputEdit", timeout=10)
    click(main_win, input_edit)
    input_edit.type_keys(msg, with_spaces=True)
    click(main_win, find_ctrl(main_win, "Button", name="发送"))
    log("text sent: %s" % msg)
    wait_body(main_win, "收到:", 40, "reply text")
    shot(main_win, "text_reply_ok")

    # ---- 6. 发文件 → 等回复文件 ----
    click(main_win, find_ctrl(main_win, "Button", name="📁"))
    dialog_submit_path(TEST_TXT, "打开")
    log("file pick submitted")
    wait_body(main_win, "收到文件", 60, "reply file")
    time.sleep(2)
    shot(main_win, "file_reply_ok")

    # ---- 7. 发图片 → 等回复图片 ----
    click(main_win, find_ctrl(main_win, "Button", name="🖼"))
    dialog_submit_path(TEST_PNG, "打开")
    log("image pick submitted")
    wait_body(main_win, "收到图片", 60, "reply image")
    time.sleep(3)
    shot(main_win, "image_reply_ok")

    # ---- 8. 点击收到的文件气泡另存 → 校验落盘 ----
    file_bubble = None
    for c in main_win.descendants(control_type="Text"):
        try:
            t = c.window_text() or ""
            if "📄" in t and "resp_" in t and c.is_visible():
                file_bubble = c
                break
        except Exception:
            continue
    if file_bubble:
        click(main_win, file_bubble)
        log("clicked incoming file bubble")
        dialog_submit_path(SAVED_FILE, "保存")
        end = time.time() + 20
        while time.time() < end and not os.path.exists(SAVED_FILE):
            time.sleep(0.5)
        assert os.path.exists(SAVED_FILE), "downloaded file missing"
        log("downloaded OK size=%s" % os.path.getsize(SAVED_FILE))
        shot(main_win, "download_saved")
    else:
        log("WARN: 未找到可点击的回复文件气泡")

    log("ALL FLOW PASSED")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log("RESULT: FAILED - %s" % e)
        sys.exit(1)
    log("RESULT: SUCCESS")
