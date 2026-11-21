#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
调试辅助（本机无 cdb/windbg 时的取证手段）：ctypes 起 DEBUG_PROCESS 跑目标，
打印异常码/出错地址（dbghelp 按同目录 .pdb 符号化）。用法：
   python mini_dbg.py <exe> <日志文件>
脚本本身只负责"跑起来并等崩溃"，负载由外部 curl/测试客户端制造。
"""
import ctypes as C
import ctypes.wintypes as W
import sys, os

exe, logf = sys.argv[1], sys.argv[2]
k32 = C.windll.kernel32
dbgh = C.windll.dbghelp
psapi = C.windll.psapi

class STARTUPINFOW(C.Structure):
    _fields_=[("cb",W.DWORD),("lpReserved",W.LPWSTR),("lpDesktop",W.LPWSTR),
        ("lpTitle",W.LPWSTR),("dwX",W.DWORD),("dwY",W.DWORD),("dwXSize",W.DWORD),
        ("dwYSize",W.DWORD),("dwXCountChars",W.DWORD),("dwYCountChars",W.DWORD),
        ("dwFillAttribute",W.DWORD),("dwFlags",W.DWORD),("wShowWindow",W.WORD),
        ("cbReserved2",W.WORD),("lpReserved2",C.POINTER(C.c_byte)),
        ("hStdInput",W.HANDLE),("hStdOutput",W.HANDLE),("hStdError",W.HANDLE)]
class PROCESS_INFORMATION(C.Structure):
    _fields_=[("hProcess",W.HANDLE),("hThread",W.HANDLE),
        ("dwProcessId",W.DWORD),("dwThreadId",W.DWORD)]
class EXCEPTION_RECORD(C.Structure): pass
EXCEPTION_RECORD._fields_=[("ExceptionCode",W.DWORD),("ExceptionFlags",W.DWORD),
    ("ExceptionRecord",C.POINTER(EXCEPTION_RECORD)),("ExceptionAddress",C.c_void_p),
    ("NumberParameters",W.DWORD),("ExceptionInformation",C.c_ulonglong*15)]
class EXCEPTION_DEBUG_INFO(C.Structure):
    _fields_=[("ExceptionCode",W.DWORD),("ExceptionFlags",W.DWORD),
        ("ExceptionRecord",C.POINTER(EXCEPTION_RECORD)),
        ("ExceptionAddress",C.c_void_p),("NumberParameters",W.DWORD),
        ("ExceptionInformation",C.c_ulonglong*15),
        ("dwFirstChance",W.DWORD)]
class EXCEPTION_EVENT(C.Structure):
    _fields_=[("dwDebugEventCode",W.DWORD),("dwProcessId",W.DWORD),
        ("dwThreadId",W.DWORD),("u",EXCEPTION_DEBUG_INFO)]
class DEBUG_EVENT(C.Structure):
    _fields_=[("dwDebugEventCode",W.DWORD),("dwProcessId",W.DWORD),
        ("dwThreadId",W.DWORD),("rest",C.c_byte*168)]
class EXIT_INFO(C.Structure):
    _fields_=[("code",W.DWORD)]

log = k32.CreateFileW(logf, 0x40000000, 0, None, 2, 0, None)
def w(s):
    b=s.encode("utf-8","replace"); n=W.DWORD(0)
    k32.WriteFile(log, b, len(b), C.byref(n), None)

si=STARTUPINFOW(); si.cb=C.sizeof(si)
pi=PROCESS_INFORMATION()
if not k32.CreateProcessW(exe, None, None, None, False, 1, None,
                          os.path.dirname(exe), C.byref(si), C.byref(pi)):
    print("CreateProcess failed", C.get_last_error()); sys.exit(2)
w(f"started pid={pi.dwProcessId}\n")

dbgh.SymSetOptions(0x18)   # UNDNAME | LOAD_LINES
dbgh.SymInitialize(pi.hProcess, exe, True)

MAXN=1024
class SYM(C.Structure):
    _fields_=[("SizeOfStruct",W.DWORD),("Index",W.DWORD),("Type",W.DWORD),
        ("Reserved0",W.DWORD),("Reserved1",W.DWORD),("Addr",C.c_ulonglong),
        ("SymbolSize",W.DWORD),("MaxNameLen",W.DWORD),("Name",C.c_char*MAXN)]
def sym(addr):
    if not addr: return "0"
    s=SYM(); s.SizeOfStruct=C.sizeof(SYM)-MAXN; s.MaxNameLen=MAXN-1
    d=C.c_ulonglong(0)
    if dbgh.SymFromAddr(pi.hProcess, C.c_ulonglong(addr), C.byref(d), C.byref(s)):
        return f"{s.Name.decode(errors='replace')}+0x{d.value:x}"
    mod=C.c_void_p()
    try:
        if dbgh.GetModuleHandleExW(0x4|0x2, C.c_void_p(addr), C.byref(mod)):
            base=mod.value
            buf=C.create_unicode_buffer(260)
            psapi.GetModuleFileNameExW(pi.hProcess, base, buf, 260)
            return f"{os.path.basename(buf.value)}+{addr-base:#x}"
    except Exception: pass
    return f"{addr:#x}"

IGN={0x40010006,0x406D1388,0x34A4A5E0,0xE06D7363}   # 调试输出/C++抛出通知等
# first chance 必须用 NOT_CONTINUED "交还"给目标进程：
# 传 DBG_CONTINUE 等于调试器替程序吞掉异常——上次"调试器下不崩"就是这个原因，
# 崩溃本身是异常，被我们吃掉了。
DBG_EXC_NOT_CONTINUED=0x00010002
DBG_CONTINUE=0x00010004
while True:
    de=DEBUG_EVENT()
    if not k32.WaitForDebugEvent(C.byref(de), 900000):
        w("TIMEOUT 15min\n"); break
    code=de.dwDebugEventCode
    if code==1:
        ee=EXCEPTION_EVENT()
        C.memmove(C.byref(ee), C.byref(de), C.sizeof(ee))
        c=ee.u.ExceptionCode & 0xFFFFFFFF
        first = ee.u.dwFirstChance
        if c not in IGN:
            w(f"EXC code={c:08x} addr={sym(ee.u.ExceptionAddress)} firstchance={first} tid={ee.dwThreadId}\n")
            if c==0xC0000005 and ee.u.NumberParameters>=2:
                w(f"    ACCESS {'WRITE' if ee.u.ExceptionInformation[1] else 'READ'} @{ee.u.ExceptionInformation[0]:#x}\n")
            if c==0xC0000409 and ee.u.NumberParameters>=1:
                w(f"    fastfail subcode={ee.u.ExceptionInformation[0]:.0f}\n")
            if first==0:
                # second chance：ExceptionRecord 指向**被调试进程的栈**，
                # 跨进程解引用会直接把本调试脚本踩崩（上一轮就这么丢过现场）。
                # 只记录顶层信息，然后终止留全日志。
                w("    SECOND CHANCE — terminating (see addr above)\n")
                k32.TerminateProcess(pi.hProcess, 0x7F)
                k32.ContinueDebugEvent(ee.dwProcessId, ee.dwThreadId, DBG_EXC_NOT_CONTINUED)
                break
        if c==0x80000003:   # 加载器断点属调试器基建，吞掉是标准做法
            k32.ContinueDebugEvent(ee.dwProcessId, ee.dwThreadId, DBG_CONTINUE)
        else:
            k32.ContinueDebugEvent(ee.dwProcessId, ee.dwThreadId, DBG_EXC_NOT_CONTINUED)
    elif code==5:   # EXIT_PROCESS_DEBUG_EVENT（6 是 LOAD_DLL，别搞混）
        ei=EXIT_INFO()
        C.memmove(C.byref(ei), C.byref(de, 12), 4)
        w(f"PROCESS_EXIT code={ei.code:#x} ({ei.code})\n")
        break
    else:
        k32.ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE)

k32.CloseHandle(pi.hProcess); k32.CloseHandle(pi.hThread)
w("done\n"); print("mini_dbg done ->", logf)
