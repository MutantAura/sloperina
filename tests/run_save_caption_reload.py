"""Run the actual save-caption reload controller against modeled native callbacks."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    args = ap.parse_args()
    out = ROOT / 'obj/tests'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'src/ootgz/savebackup.c').read_text(encoding='utf-8')
    begin = source.index('/* --- the game\'s Load list after a restore')
    end = source.index('\nbool savebackup_restore(', begin)
    (out / 'save_caption_reload.generated.h').write_text(source[begin:end], encoding='utf-8')
    exe = out / ('save_caption_reload.exe' if os.name == 'nt' else 'save_caption_reload')
    env = os.environ.copy()
    if Path(args.cc).is_absolute():
        env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
    options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}
    for command in [[args.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                     '-I' + str(out), 'tests/save_caption_reload.c', '-o', str(exe)], [str(exe)]]:
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, **options)
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        result.check_returncode()


if __name__ == '__main__':
    main()
