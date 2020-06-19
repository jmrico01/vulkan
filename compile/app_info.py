import os
import subprocess

from compile import Platform, TargetType, Define, PlatformTargetOptions, BuildTarget, CopyDir, LibExternal
from env_settings import WIN32_VCVARSALL

# TODO hack!
VULKAN_PATH = "C:\\VulkanSDK\\1.2.135.0"

TARGETS = [
    BuildTarget("vulkan",
        source_file="src/main.cpp",
        type=TargetType.EXECUTABLE,
        defines=[],
        platform_options={
            Platform.WINDOWS: PlatformTargetOptions(
                defines=[],
                compiler_flags=[
                    # "-GS-",       # Disable stack security cookie for CRT removal
                    # "-Gs9999999", # Only generate stack probe for stack > 9999999 (so like never)

                    "-wd4201",    # nonstandard extension used: nameless struct/union
                    "-I\"" + VULKAN_PATH + "\\Include\"", # HACK
                ],
                linker_flags=[
                    "-LIBPATH:\"" + VULKAN_PATH + "\\Lib\"", # HACK
                    "user32.lib",
                    "vulkan-1.lib",
                    # "-subsystem:windows",      # Windows application (no console)
                    # "-nodefaultlib",           # No sneaky CRT or kernel32.lib
                    # "-STACK:0x100000,0x100000" # Allocate and commit 1MB for main thread's stack
                ]
            )
        }
    )
]

COPY_DIRS = [
    CopyDir("data", "data"),
	CopyDir("src/shaders", "data/shaders")
]

DEPLOY_FILES = [
	"data",
	"logs",
    TARGETS[0].get_output_name()
]

LIBS_EXTERNAL = [
    LibExternal("freetype",
        path="freetype-2.8.1",
        compiledNames={
            "debug":   "freetype281MTd.lib",
            "release": "freetype281MT.lib"
        }
    ),
    LibExternal("stbimage",   path="stb_image-2.23"),
    LibExternal("stbsprintf", path="stb_sprintf-1.06")
]

PATHS = {
    "win32-vcvarsall": WIN32_VCVARSALL
}

def post_compile_custom(paths):
    shader_path = "build/data/shaders"
    for file in os.listdir(shader_path):
        if len(file) >= 5 and (file[-5:] == ".vert" or file[-5:] == ".frag"):
            shader_file_path = shader_path + "/" + file
            output_file_path = shader_path + "/" + file + ".spv"
            subprocess.call(VULKAN_PATH + "\\Bin\\glslc.exe " + shader_file_path + " -o " + output_file_path, shell=True)
            os.remove(shader_file_path)

