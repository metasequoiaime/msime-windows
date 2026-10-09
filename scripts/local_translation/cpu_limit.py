"""Windows CPU hard cap, expressed as a percentage of total CPU capacity."""
from __future__ import annotations

import ctypes
import math
import os


class CpuRate(ctypes.Structure):
    _fields_ = [("ControlFlags", ctypes.c_uint32), ("CpuRate", ctypes.c_uint32)]


def rate_units(percent: float) -> int:
    if not math.isfinite(percent) or not 0.01 <= percent <= 5:
        raise ValueError("CPU limit must be between 0.01 and 5 percent")
    return round(percent * 100)


class CpuLimit:
    """Keep the job handle alive; refuse to load a model without a verified cap."""

    def __init__(self, percent: float = 1):
        units = rate_units(percent)
        if os.name != "nt":
            raise OSError("Local translation CPU hard caps require Windows 10/11")
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        api.CreateJobObjectW.restype = ctypes.c_void_p
        api.GetCurrentProcess.restype = ctypes.c_void_p
        api.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        api.QueryInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                                  ctypes.c_uint32, ctypes.c_void_p]
        api.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        api.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        api.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        self.api = api
        self.handle = api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            requested = CpuRate(0x1 | 0x4, units)  # ENABLE | HARD_CAP
            if not api.SetInformationJobObject(self.handle, 15, ctypes.byref(requested), ctypes.sizeof(requested)):
                raise ctypes.WinError(ctypes.get_last_error())
            process = api.GetCurrentProcess()
            if not api.AssignProcessToJobObject(self.handle, process):
                raise ctypes.WinError(ctypes.get_last_error())
            member = ctypes.c_int()
            if not api.IsProcessInJob(process, self.handle, ctypes.byref(member)) or not member.value:
                raise OSError("Process was not assigned to the CPU-limited job")
            actual = CpuRate()
            if not api.QueryInformationJobObject(self.handle, 15, ctypes.byref(actual), ctypes.sizeof(actual), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if actual.ControlFlags != requested.ControlFlags or actual.CpuRate != units:
                raise OSError("Windows did not retain the requested CPU hard cap")
            if not api.SetPriorityClass(process, 0x4000):  # BELOW_NORMAL_PRIORITY_CLASS
                raise ctypes.WinError(ctypes.get_last_error())
            self.percent = actual.CpuRate / 100
        except BaseException:
            api.CloseHandle(self.handle)
            self.handle = None
            raise

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None
