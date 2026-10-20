import os
from pathlib import Path
import shutil
import subprocess
import tempfile

source = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='bg3le-fetch-') as tmp:
    root = Path(tmp)
    (root / 'tools').mkdir()
    shutil.copy(source / 'tools/fetch-externals.sh', root / 'tools')
    ext = root / 'external/third_party'
    ext.mkdir(parents=True)
    for name in ('glm', 'imgui', 'lua', 'rapidjson', 'tinycrypt', 'Vulkan', 'optick'):
        (ext / name).mkdir()
    header = ext / 'Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include/NsCore/TypePropertyImpl.h'
    header.parent.mkdir(parents=True)
    header.write_text('void Get(const void* ptr, void* dest) const override;\n')
    bin_dir = root / 'bin'
    bin_dir.mkdir()
    log = root / 'commands'
    for name, body in {
        'git': 'printf "%s\\n" "$*" >> "$TEST_LOG"\nfor last; do :; done\nmkdir -p "$last"',
        'cmake': 'if [ "${ALLOW_HOST_BUILD:-}" != 1 ]; then echo "Unexpected host build" >&2; exit 99; fi\nprintf "cmake %s\\n" "$*" >> "$TEST_LOG"',
    }.items():
        command = bin_dir / name
        command.write_text('#!/bin/bash\nset -eu\n' + body + '\n')
        command.chmod(0o755)
    env = dict(os.environ, PATH=str(bin_dir) + ':' + os.environ['PATH'], TEST_LOG=str(log))
    result = subprocess.run(['bash', str(root / 'tools/fetch-externals.sh'), '--sources-only'], env=env, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (ext / 'abseil').is_dir() and (ext / 'protobuf').is_dir()
    assert not (ext / '.libcxx-abi-flags').exists()
    assert 'override' not in header.read_text()
    # Source fetching must leave existing host builds and ABI metadata untouched.
    archive = ext / 'abseil/build/keep.a'
    archive.parent.mkdir(parents=True)
    archive.write_text('host build')
    stamp = ext / '.libcxx-abi-flags'
    stamp.write_text('old ABI\n')
    subprocess.run(['bash', str(root / 'tools/fetch-externals.sh'), '--sources-only'], env=env, check=True, capture_output=True)
    assert archive.read_text() == 'host build'
    assert stamp.read_text() == 'old ABI\n'
    # Default invocation still builds host dependencies and records their ABI.
    env['ALLOW_HOST_BUILD'] = '1'
    subprocess.run(['bash', str(root / 'tools/fetch-externals.sh')], env=env, check=True, capture_output=True)
    assert not archive.exists()
    assert stamp.read_text().strip() == '-D_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION'
    commands = log.read_text()
    assert 'cmake -S abseil' in commands and 'cmake -S protobuf' in commands
    invalid = subprocess.run(['bash', str(root / 'tools/fetch-externals.sh'), '--invalid'], env=env, capture_output=True)
    assert invalid.returncode == 2
print('Dependency-fetch regression checks passed')
