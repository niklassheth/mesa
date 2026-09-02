# SPDX-License-Identifier: MIT

"""Small batched Piglit profile for T8132 Apple9 compute bring-up."""

import os

from framework.profile import TestProfile
from framework.test.opengl import FastSkipDisabled
from framework.test.shader_test import MultiShaderTest


_PROFILE_DIR = os.path.dirname(os.path.realpath(__file__))
_TEST_DIR = os.path.join(_PROFILE_DIR, "tests")
_TESTS = [
    os.path.join(_TEST_DIR, "01-global-invocation-id.shader_test"),
    os.path.join(_TEST_DIR, "02-integer-add.shader_test"),
    os.path.join(_TEST_DIR, "03-two-load-add.shader_test"),
    os.path.join(_TEST_DIR, "04-uvec4-load-store.shader_test"),
    os.path.join(_TEST_DIR, "05-dependent-buffer-index.shader_test"),
]

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

profile = TestProfile()
test = MultiShaderTest.new(_TESTS)
# Piglit's fast-skip probe runs a separate wflinfo process without the
# per-test DRM-shim environment. The shader runner performs the real context
# and version checks, so keep this controlled profile to one GPU process.
test.skips = [FastSkipDisabled() for _ in test.skips]
test.env.update(_ENVIRONMENT)
profile.test_list["apple9-compute"] = test
