# SPDX-License-Identifier: MIT

"""Batched desktop OpenGL ARB_compute_shader profile for T8132."""

import glob
import os

from framework.profile import TestProfile
from framework.test.opengl import FastSkipDisabled
from framework.test.shader_test import MultiShaderTest, ShaderTest


_TEST_ROOT = os.path.join(
    os.environ["T8132_PIGLIT_ROOT"], "tests", "spec", "arb_compute_shader"
)
_GL_TEST_ROOT = os.environ["T8132_PIGLIT_GL_TEST_DIR"]
_SUPPORTED_EXECUTION_TESTS = sorted(
    glob.glob(os.path.join(_GL_TEST_ROOT, "*.shader_test"))
)
_SMOKE_TEST = _SUPPORTED_EXECUTION_TESTS[0]
_LINKER_DIR = os.path.join(_TEST_ROOT, "linker")
_LINKER_TESTS = [
    os.path.join(_LINKER_DIR, "one_local_work_size.shader_test"),
    os.path.join(_LINKER_DIR, "no_local_work_size.shader_test"),
    os.path.join(_LINKER_DIR, "matched_local_work_sizes.shader_test"),
    os.path.join(_LINKER_DIR, "mismatched_local_work_sizes.shader_test"),
    os.path.join(_LINKER_DIR, "mix_compute_and_non_compute.shader_test"),
    os.path.join(_LINKER_DIR, "explicit_location_partial_array_use.shader_test"),
    os.path.join(_LINKER_DIR, "bug-93840.shader_test"),
]
_EXECUTION_TESTS = sorted(
    glob.glob(os.path.join(_TEST_ROOT, "execution", "*.shader_test"))
)
_SUPPORTED_LINKER_TESTS = [
    os.path.join(_LINKER_DIR, "no_local_work_size.shader_test"),
    os.path.join(_LINKER_DIR, "mismatched_local_work_sizes.shader_test"),
    os.path.join(_LINKER_DIR, "mix_compute_and_non_compute.shader_test"),
]
_VARIABLE_GROUP_ROOT = os.path.join(
    os.environ["T8132_PIGLIT_ROOT"],
    "tests",
    "spec",
    "arb_compute_variable_group_size",
    "execution",
)
_VARIABLE_GROUP_TESTS = [
    os.path.join(_VARIABLE_GROUP_ROOT, "global-invocation-id.shader_test"),
    os.path.join(_VARIABLE_GROUP_ROOT, "separate-global-id.shader_test"),
    os.path.join(_VARIABLE_GROUP_ROOT, "separate-global-id-2.shader_test"),
]
if (
    len(_SUPPORTED_EXECUTION_TESTS) != 9
    or len(_LINKER_TESTS) != 7
    or len(_EXECUTION_TESTS) != 28
):
    raise RuntimeError(
        "expected 9 supported execution, 7 linker, and 28 discovery "
        "execution tests; found {}, {}, and {}".format(
            len(_SUPPORTED_EXECUTION_TESTS),
            len(_LINKER_TESTS),
            len(_EXECUTION_TESTS),
        )
    )

_SUBSET = os.environ.get("T8132_PIGLIT_GL_SUBSET", "supported")
if _SUBSET == "smoke":
    _TESTS = [_SMOKE_TEST]
elif _SUBSET == "supported":
    _TESTS = (
        _SUPPORTED_LINKER_TESTS
        + _VARIABLE_GROUP_TESTS
        + _SUPPORTED_EXECUTION_TESTS
    )
elif _SUBSET == "linker-basic":
    _TESTS = _LINKER_TESTS[:5]
elif _SUBSET == "linker":
    _TESTS = _LINKER_TESTS
elif _SUBSET == "execution":
    _TESTS = _EXECUTION_TESTS
elif _SUBSET == "all":
    _TESTS = _LINKER_TESTS + _EXECUTION_TESTS
else:
    raise RuntimeError("unknown T8132_PIGLIT_GL_SUBSET: {}".format(_SUBSET))

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
if len(_TESTS) == 1:
    test = ShaderTest.new(_TESTS[0])
else:
    test = MultiShaderTest.new(_TESTS)
    # Avoid a separate wflinfo process touching the device. The shader runner
    # performs the actual context, version, and extension checks.
    test.skips = [FastSkipDisabled() for _ in test.skips]
test.env.update(_ENVIRONMENT)
profile.test_list["apple9-desktop-compute-{}".format(_SUBSET)] = test
