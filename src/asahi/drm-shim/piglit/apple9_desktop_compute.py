# SPDX-License-Identifier: MIT

"""Batched desktop OpenGL ARB_compute_shader profile for T8132."""

import glob
import os

from framework.profile import TestProfile
from framework.test.opengl import FastSkipDisabled
from framework.test.shader_test import MultiShaderTest


_TEST_ROOT = os.path.join(
    os.environ["T8132_PIGLIT_ROOT"], "tests", "spec", "arb_compute_shader"
)
_TESTS = sorted(glob.glob(os.path.join(_TEST_ROOT, "**", "*.shader_test"), recursive=True))
if len(_TESTS) != 35:
    raise RuntimeError(
        "expected 35 tests in the pinned ARB_compute_shader corpus, found {}".format(
            len(_TESTS)
        )
    )

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
}

profile = TestProfile()
test = MultiShaderTest.new(_TESTS)
# Avoid a separate wflinfo process touching the device. The shader runner
# performs the actual context, version, and extension checks.
test.skips = [FastSkipDisabled() for _ in test.skips]
test.env.update(_ENVIRONMENT)
profile.test_list["apple9-desktop-compute"] = test
