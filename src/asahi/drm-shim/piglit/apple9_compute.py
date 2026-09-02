# SPDX-License-Identifier: MIT

"""Full-oracle native Apple9 compute tests, batched in one GPU process."""

import os
import subprocess

from framework import status
from framework.profile import TestProfile
from framework.test.base import ReducedProcessMixin
from framework.test.piglit_test import PiglitBaseTest


_RUNNER = os.environ["T8132_PIGLIT_NATIVE_RUNNER"]
_CASES = subprocess.check_output([_RUNNER, "--list"], text=True).splitlines()

_ENVIRONMENT = {
    "LD_PRELOAD": os.environ["T8132_PIGLIT_CHILD_LD_PRELOAD"],
    "LD_LIBRARY_PATH": os.environ["T8132_PIGLIT_CHILD_LD_LIBRARY_PATH"],
    "PYTHONPATH": os.environ["T8132_PIGLIT_CHILD_PYTHONPATH"],
    "PYTHONUNBUFFERED": "1",
    "M1N1_SHIM_ROOT": os.environ["T8132_PIGLIT_M1N1_ROOT"],
    "M1N1DEVICE": os.environ.get("M1N1DEVICE", "/dev/m1n1"),
    "__EGL_VENDOR_LIBRARY_FILENAMES": os.environ[
        "T8132_PIGLIT_CHILD_EGL_VENDOR"
    ],
    "MESA_LOADER_DRIVER_OVERRIDE": "asahi",
    "MESA_SHADER_CACHE_DISABLE": "true",
    "PIGLIT_PLATFORM": "surfaceless_egl",
    "PIGLIT_NO_WINDOW": "1",
}

class Apple9ComputeBatch(ReducedProcessMixin, PiglitBaseTest):
    """Run named native cases in one context, resuming after a failed case."""

    def __init__(self, cases):
        self._runner = _RUNNER
        self._cases = cases
        super().__init__(
            [self._runner] + cases,
            subtests=cases,
            run_concurrent=False,
            env=_ENVIRONMENT,
        )

    def _is_subtest(self, line):
        return line.startswith("PIGLIT TEST:")

    def _resume(self, current):
        return [self._runner] + self._cases[current:]

    def _stop_status(self):
        if self.result.returncode > 0:
            return status.FAIL
        return status.CRASH

    def _is_cherry(self):
        completed = sum(
            line.startswith('PIGLIT: {"subtest":')
            for line in self.result.out.splitlines()
        )
        return completed == len(self._expected)


profile = TestProfile()
profile.test_list["apple9-native-compute"] = Apple9ComputeBatch(_CASES)
