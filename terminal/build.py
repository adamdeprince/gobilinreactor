#!/usr/bin/env python3
"""Build the pinned upstream kitty engine and CPython for Android arm64.

Requires Python 3.12+, CMake, Ninja, pkg-config, patch, a host C/C++ toolchain,
Android SDK and NDK 29.0.14206865. Downloads are verified before extraction.
The harness builder consumes terminal/build without network access.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

HERE = Path(__file__).resolve().parent


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class Build:
    def __init__(self, args):
        self.args = args
        self.root = args.cache.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.downloads = self.root / 'downloads'
        self.downloads.mkdir(exist_ok=True)
        self.lock = json.loads((HERE / 'sources.lock.json').read_text())
        self.ndk = args.sdk.resolve() / 'ndk' / args.ndk
        self.tools = next((self.ndk / 'toolchains/llvm/prebuilt').iterdir()) / 'bin'
        self.cc = self.tools / 'aarch64-linux-android29-clang'
        self.cxx = self.tools / 'aarch64-linux-android29-clang++'
        self.prefix = self.root / 'native-prefix'
        self.env = os.environ.copy()
        self.env.update(ANDROID_HOME=str(args.sdk.resolve()), ANDROID_API_LEVEL='29')
        self.sources = {}

    def download(self, item):
        path = self.downloads / item['sha256']
        if not path.exists():
            if self.args.offline:
                raise RuntimeError(f"Missing cached archive: {item['url']}")
            temporary = path.with_suffix('.part')
            run(['curl', '--fail', '--location', '--retry', '3', '--proto', '=https',
                 '--proto-redir', '=https', '--output', temporary, item['url']])
            if digest(temporary) != item['sha256']:
                temporary.unlink()
                raise RuntimeError(f"SHA256 mismatch: {item['url']}")
            temporary.replace(path)
        if digest(path) != item['sha256']:
            raise RuntimeError(f'Corrupt cached archive: {path}')
        return path

    def source(self, name, force=False):
        item = self.lock[name]
        archive = self.download(item)
        dest = self.root / item['directory']
        marker = dest / '.goblin-source-sha256'
        if force or not marker.exists() or marker.read_text() != item['sha256']:
            if dest.exists():
                shutil.rmtree(dest)
            if name == 'meson':
                dest.mkdir()
                with zipfile.ZipFile(archive) as z:
                    for entry in z.infolist():
                        if not (dest / entry.filename).resolve().is_relative_to(dest):
                            raise RuntimeError('Invalid wheel path')
                    z.extractall(dest)
            else:
                with tarfile.open(archive) as tar:
                    if any(Path(m.name).parts[0] != item['directory'] for m in tar):
                        raise RuntimeError(f'Unexpected archive layout: {name}')
                    tar.extractall(self.root, filter='data')
            marker.write_text(item['sha256'])
        self.sources[name] = dest
        return dest

    def prepare(self):
        for name in self.lock:
            self.source(name)
        patch = HERE / 'patches/kitty-android.patch'
        stamp = self.sources['kitty'] / '.goblin-patch-sha256'
        if not stamp.exists() or stamp.read_text() != digest(patch):
            if stamp.exists():
                self.source('kitty', force=True)
            with patch.open() as stream:
                run(['patch', '--batch', '-p1'], cwd=self.sources['kitty'], stdin=stream)
            stamp.write_text(digest(patch))
        self.env['PYTHONPATH'] = str(self.sources['meson'])
        self.env['PKG_CONFIG_LIBDIR'] = str(self.prefix / 'lib/pkgconfig')
        self.env['PKG_CONFIG_PATH'] = ''

    def python(self):
        source = self.sources['python']
        prefix = source / 'cross-build/aarch64-linux-android/prefix'
        deps = json.loads((HERE / 'python-deps.lock.json').read_text())
        archives = [(item, self.download(item)) for item in deps]
        recipe = hashlib.sha256((self.args.ndk + json.dumps(deps) + self.lock['python']['sha256']).encode()).hexdigest()
        stamp = source / '.goblin-python-build'
        if stamp.exists() and stamp.read_text() == recipe and (prefix / 'lib/libpython3.13.so').exists():
            return
        # Populate only verified dependencies. Upstream then skips its unpinned
        # download helper because the prefix already exists.
        prefix.mkdir(parents=True, exist_ok=True)
        for item, archive in archives:
            with tarfile.open(archive) as tar:
                tar.extractall(prefix, filter='data')
        script = source / 'Android/android-env.sh'
        script.write_text(re.sub(r'^ndk_version=.*$', 'ndk_version=' + self.args.ndk,
                                 script.read_text(), flags=re.MULTILINE))
        script = source / 'Android/android.py'
        script.write_text(script.read_text().replace('str(os.cpu_count())', str(repr(str(self.args.jobs)))))
        env = self.env.copy()
        # CPython configures its own pkg-config path for the host and Android
        # builds; do not contaminate the native build with Android static libs.
        for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'PYTHONPATH'):
            env.pop(name, None)
        run([sys.executable, source / 'Android/android.py', 'build', 'aarch64-linux-android',
             '--', '--without-ensurepip'], env=env)
        stamp.write_text(recipe)

    def cmake(self, name, options, subdirectory=''):
        dest = self.root / ('build-' + name)
        run(['cmake', '-G', 'Ninja', '-S', self.sources[name] / subdirectory, '-B', dest,
             f'-DCMAKE_TOOLCHAIN_FILE={self.ndk}/build/cmake/android.toolchain.cmake',
             '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_static',
             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DBUILD_SHARED_LIBS=OFF',
             f'-DCMAKE_INSTALL_PREFIX={self.prefix}', f'-DCMAKE_PREFIX_PATH={self.prefix}',
             f'-DCMAKE_FIND_ROOT_PATH={self.prefix}', *options], env=self.env)
        run(['cmake', '--build', dest, '-j', self.args.jobs], env=self.env)
        run(['cmake', '--install', dest], env=self.env)

    def meson(self, name, options):
        dest = self.root / ('build-' + name)
        command = [sys.executable, '-m', 'mesonbuild.mesonmain']
        if not (dest / 'build.ninja').exists():
            run([*command, 'setup', dest, self.sources[name], '--cross-file', self.root / 'android-cross.ini',
                 '--prefix', self.prefix, '--libdir', 'lib', '--buildtype', 'release',
                 '--default-library', 'static', '--wrap-mode', 'nodownload', *options], env=self.env)
        run([*command, 'compile', '-C', dest, '-j', self.args.jobs], env=self.env)
        run([*command, 'install', '-C', dest], env=self.env)

    def libraries(self):
        self.cmake('libpng', ['-DPNG_SHARED=OFF', '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF'])
        self.cmake('freetype', ['-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BROTLI=ON'])
        self.cmake('harfbuzz', ['-DHB_HAVE_FREETYPE=ON', '-DHB_BUILD_SUBSET=OFF', '-DHB_BUILD_RASTER=OFF',
                              '-DHB_BUILD_VECTOR=OFF', '-DHB_BUILD_GPU=OFF', '-DHB_BUILD_GPU_DEMO=OFF'])
        self.cmake('lcms', ['-DLCMS2_BUILD_SHARED=OFF', '-DLCMS2_BUILD_TESTS=OFF', '-DLCMS2_BUILD_TOOLS=OFF'])
        self.cmake('expat', ['-DEXPAT_SHARED_LIBS=OFF', '-DEXPAT_BUILD_TOOLS=OFF', '-DEXPAT_BUILD_TESTS=OFF',
                            '-DEXPAT_BUILD_EXAMPLES=OFF', '-DEXPAT_BUILD_DOCS=OFF'], 'expat')
        # Meson must resolve platform zlib from the NDK, never host Homebrew.
        (self.prefix / 'lib/pkgconfig/zlib.pc').write_text(
            'Name: zlib\nDescription: Android platform zlib\nVersion: 1.2.13\nLibs: -lz\n')
        (self.root / 'android-cross.ini').write_text(f"""[binaries]
c = '{self.cc}'
cpp = '{self.cxx}'
ar = '{self.tools}/llvm-ar'
strip = '{self.tools}/llvm-strip'
pkg-config = '{shutil.which('pkg-config')}'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']
c_link_args = ['-Wl,-z,max-page-size=16384']
cpp_link_args = ['-Wl,-z,max-page-size=16384']
""")
        self.meson('pixman', ['-Dtests=disabled', '-Ddemos=disabled', '-Dgtk=disabled', '-Dlibpng=disabled'])
        self.meson('fontconfig', ['-Ddoc=disabled', '-Dnls=disabled', '-Dtests=disabled',
                                 '-Dtests-external-fonts=disabled', '-Dtools=disabled', '-Dcache-build=disabled',
                                 '-Ddefault-fonts-dirs=/system/fonts', '-Dadditional-fonts-dirs=[]', '-Dxml-backend=expat'])
        self.meson('cairo', ['-Dtests=disabled', '-Dxlib=disabled', '-Dxcb=disabled', '-Dglib=disabled',
                            '-Dfontconfig=enabled', '-Dfreetype=enabled', '-Dpng=enabled', '-Dlzo=disabled',
                            '-Dspectre=disabled', '-Dsymbol-lookup=disabled'])

    def kitty(self):
        src = self.sources['kitty']
        py = self.sources['python'] / 'cross-build/aarch64-linux-android/prefix'
        out = self.root / 'kitty-build'
        out.mkdir(exist_ok=True)
        run([sys.executable, '-c', "import runpy; s=runpy.run_path('setup.py'); "
             "s['build_ref_map'](); s['build_cli_parser_specs'](); s['build_uniforms_header']()"], cwd=src)
        includes = [py / 'include', py / 'include/python3.13', self.prefix / 'include',
                    *(self.prefix / ('include/' + x) for x in ('freetype2', 'harfbuzz', 'cairo', 'pixman-1')),
                    self.sources['simde'], self.sources['xxhash'], src / '3rdparty/base64', src / 'kitty', src, HERE]
        flags = ['-std=gnu11', '-O2', '-fPIC', '-ffunction-sections', '-fdata-sections',
                 '-Wno-unused-function', '-Wno-unused-variable', '-DKITTY_VERSION="0.48.2"',
                 '-DKITTY_VCS_REV="v0.48.2"', '-DWRAPPED_KITTENS=""', '-DPRIMARY_VERSION=4000',
                 '-DSECONDARY_VERSION=4802', '-DXT_VERSION="kitty(0.48.2-goblin)"',
                 '-D_KITTY_FONTCONFIG_LIBRARY="libgoblinkitty.so"', *(f'-I{x}' for x in includes)]
        sources = [p for p in sorted((src / 'kitty').glob('*.c')) if p.name not in ('macos_process_info.c', 'utmp.c')]
        sources += [HERE / 'engine.c', src / '3rdparty/ringbuf/ringbuf.c', self.sources['xxhash'] / 'xxhash.c',
                    *sorted((src / '3rdparty/base64/lib/arch').glob('*/codec.c')),
                    *(src / ('3rdparty/base64/lib/' + p) for p in ('tables/tables.c', 'codec_choose.c', 'lib.c'))]

        def compile_source(p):
            name = str(p.relative_to(src)) if p.is_relative_to(src) else p.name
            dest = out / (name.replace('/', '-') + '.o')
            extra = []
            if '/base64/' in str(p):
                extra = [f'-DHAVE_{x}={int(x == "NEON64")}' for x in
                         ('AVX512', 'AVX2', 'NEON32', 'NEON64', 'SSSE3', 'SSE41', 'SSE42', 'AVX')]
            if 'simd-string-' in p.name:
                extra += ['-fopenmp-simd', '-DSIMDE_ENABLE_OPENMP']
            run([self.cc, *flags, *extra, '-c', p, '-o', dest])
            return dest

        # Recompile all translation units: options and upstream generated headers
        # are dependencies too. Avoid stale source-only mtime caches.
        with concurrent.futures.ThreadPoolExecutor(self.args.jobs) as pool:
            objects = list(pool.map(compile_source, sources))
        dump = out / 'vt-parser-dump.o'
        run([self.cc, *flags, '-DDUMP_COMMANDS', '-c', src / 'kitty/vt-parser.c', '-o', dump])
        render = out / 'android-renderer.o'
        run([self.cxx, '-std=c++17', '-O2', '-fPIC', '-c', HERE / 'renderer.cpp', '-o', render])
        libs = [self.prefix / f'lib/lib{x}.a' for x in ('cairo', 'pixman-1', 'freetype', 'harfbuzz', 'png16', 'lcms2')]
        native = out / 'libgoblinkitty.so'
        run([self.cxx, '-shared', '-static-libstdc++', '-Wl,--no-undefined', '-Wl,--gc-sections',
             '-Wl,-z,max-page-size=16384', '-Wl,-soname,libgoblinkitty.so', *objects, dump, render,
             '-Wl,--start-group', *libs, '-Wl,--whole-archive', self.prefix / 'lib/libfontconfig.a',
             '-Wl,--no-whole-archive', self.prefix / 'lib/libexpat.a', '-Wl,--end-group',
             f'-L{py}/lib', '-lpython3.13', '-lcrypto', '-lGLESv3', '-llog', '-landroid', '-lz', '-ldl', '-lm', '-o', native])
        licenses = self.root / 'licenses'
        licenses.mkdir(exist_ok=True)
        for name, source in self.sources.items():
            for path in source.rglob('*'):
                if path.is_file() and path.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'FTL.TXT')):
                    relative = path.relative_to(source)
                    if not any(x in relative.parts for x in ('cross-build', '.git')):
                        dest = licenses / name / relative
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, dest)
        for item in json.loads((HERE / 'python-deps.lock.json').read_text()):
            with tarfile.open(self.download(item)) as tar:
                for member in tar:
                    if member.isfile() and any(x in member.name.lower() for x in ('license', 'licence', 'copyright', 'copying')):
                        path = licenses / item['name'] / member.name
                        if not path.resolve().is_relative_to(licenses):
                            raise RuntimeError('Invalid license path')
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(tar.extractfile(member).read())
        for item in json.loads((HERE / 'licenses.lock.json').read_text()):
            archive = self.download(item)
            if 'member' in item:
                with tarfile.open(archive) as tar:
                    data = tar.extractfile(item['member']).read()
            else:
                data = archive.read_bytes()
            (licenses / (item['name'] + '-LICENSE.txt')).write_bytes(data)
        run([sys.executable, HERE / 'package.py', '--python-prefix', py, '--kitty-source', src,
             '--native', native, '--strip', self.tools / 'llvm-strip', '--licenses', licenses,
             '--output', self.args.output.resolve()])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('ANDROID_HOME', os.environ.get('ANDROID_SDK_ROOT', Path.home() / 'Library/Android/sdk'))))
    parser.add_argument('--ndk', default='29.0.14206865')
    parser.add_argument('--cache', type=Path, default=HERE / 'build/cache')
    parser.add_argument('--output', type=Path, default=HERE / 'build')
    parser.add_argument('--jobs', type=int, default=min(6, os.cpu_count() or 1))
    parser.add_argument('--offline', action='store_true', help='require all archives in the SHA256 cache')
    parser.add_argument('--runtime-only', action='store_true', help='reuse previously built Android dependency libraries')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    for tool in ('cmake', 'ninja', 'pkg-config', 'patch', 'curl'):
        if not shutil.which(tool):
            parser.error(f'Missing host tool: {tool}')
    build = Build(args)
    build.prepare()
    if not args.runtime_only:
        build.python()
        build.libraries()
    build.kitty()


if __name__ == '__main__':
    main()
