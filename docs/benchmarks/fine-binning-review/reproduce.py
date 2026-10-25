#!/usr/bin/env python3
"""Build and measure the review-only fine-binning prototype without editing source."""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

OBJECTS = (
    'setup', 'hw_mult18x18', 'hw_bsram', 'hw_tile_pipeline', 'hw_perspective',
    'hw_texture', 'hw_walker2d', 'hw_blit', 'hw_sdram', 'hw_host_link',
    'hw_scanout', 'hw_protocol', 'hw_system_copy', 'hw_system', 'hw_reciprocal',
    'hw_fragment_pipeline', 'demo', 'main-headless',
)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[3] if len(Path(__file__).resolve().parents) > 3 else Path.cwd()
    parser.add_argument('--repo', type=Path, default=default_root, help='repository root')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--no-build', action='store_true', help='reuse existing baseline objects')
    args = parser.parse_args()
    root = args.repo.resolve()
    build = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    output = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix='nano-finebin-review-'))
    output.mkdir(parents=True, exist_ok=True)
    patch = Path(__file__).with_name('prototype.patch').read_text().splitlines(keepends=True)
    starts = [index for index, line in enumerate(patch) if line.startswith('@@')]
    if len(starts) != 1:
        raise RuntimeError('review patch must contain exactly one hunk')
    hunk = patch[starts[0] + 1:]
    before = ''.join(line[1:] for line in hunk if line[:1] in (' ', '-'))
    after = ''.join(line[1:] for line in hunk if line[:1] in (' ', '+'))
    source = (root / 'reference/src/renderer.cpp').read_text()
    if not before or source.count(before) != 1:
        raise RuntimeError('baseline source no longer matches the single review hunk')
    copied = output / 'renderer.cpp'
    copied.write_text(source.replace(before, after))
    if not args.no_build:
        subprocess.run(['make', '-j2', 'BUILD_DIR=' + str(build), 'headless'], cwd=root, check=True)
    for name in OBJECTS:
        if not (build / (name + '.o')).is_file():
            raise RuntimeError('missing baseline object: ' + name + '.o')
    object_path = output / 'renderer.o'
    subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Wshadow', '-O2', '-g',
        '-I' + str(root / 'reference/include'), '-I' + str(build / 'generated'),
        '-c', str(copied), '-o', str(object_path)], cwd=root, check=True)
    executable = output / ('nano-gpu-ref-finebin' + ('.exe' if os.name == 'nt' else ''))
    subprocess.run([args.cxx, '-O2', '-g', str(object_path)] +
        [str(build / (name + '.o')) for name in OBJECTS] + ['-o', str(executable)], cwd=root, check=True)
    report = output / 'benchmark.json'
    subprocess.run([sys.executable,
        str(root / 'scripts/benchmark-system.py'), '--executable', str(executable), '--verify',
        '--baseline', str(root / 'docs/benchmarks/system-after-dataflow.json'),
        '--output', str(report)], cwd=root, check=True)
    print('Review prototype and report: ' + str(output))

if __name__ == '__main__':
    main()
