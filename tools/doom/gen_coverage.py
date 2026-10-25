#!/usr/bin/env python3
"""
gen_coverage.py — 由真实调用清单生成 docs/gl-coverage.md 的「实测」部分

用法：
  1. 直通模式跑一遍游戏（见 docs/doom.md）：
       set ESPGPU_GL_MODE=passthrough
       dsda-doom.exe -iwad doom2.wad -warp 1
     退出后得到 espgpu-gltrace.txt
  2. python tools/doom/gen_coverage.py path/to/espgpu-gltrace.txt > docs/gl-coverage-trace.md

脚本把清单里出现过的每个入口和 host/gl12/gen/coverage.tsv（espgpu 模式的实现状态）对照，
列出「被调用但仍是桩」与「wglGetProcAddress 请求了但我们没提供」的入口——这就是 M4 要补的清单。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_coverage():
    cov = {}
    path = os.path.join(ROOT, 'host', 'gl12', 'gen', 'coverage.tsv')
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        name, kind, status = line.rstrip('\n').split('\t')
        cov[name] = (kind, status)
    return cov


def parse_trace(path):
    calls, enums, procs = [], {}, []
    section = None
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('## functions'):
            section = 'fn'
            continue
        if line.startswith('## enum'):
            section = 'enum'
            continue
        if line.startswith('## wglGetProcAddress'):
            section = 'proc'
            continue
        if line.startswith('## '):
            section = None
            continue
        if section == 'fn':
            m = re.match(r'\s*(\d+)\s+([\d.]+)\s+(\w+)', line)
            if m:
                calls.append((int(m.group(1)), float(m.group(2)), m.group(3)))
        elif section == 'enum':
            m = re.match(r'^(\w+)\s+arg(\d+):', line)
            if m:
                cur = (m.group(1), int(m.group(2)))
                enums.setdefault(cur, [])
                continue
            m = re.match(r'\s+(0x[0-9A-Fa-f]+)\s*(\S*)', line)
            if m and cur:
                enums[cur].append(m.group(2) or m.group(1))
        elif section == 'proc':
            m = re.match(r'\s*(\d+)\s+(\S+)\s+(.*)$', line)
            if m:
                procs.append((m.group(2), m.group(3).strip()))
    return calls, enums, procs


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cov = load_coverage()
    calls, enums, procs = parse_trace(sys.argv[1])
    out = ['# GL 调用清单（实测）', '', '由 `tools/doom/gen_coverage.py %s` 生成。' % os.path.basename(sys.argv[1]), '']
    out.append('| 入口 | 调用次数 | 每帧 | espgpu 实现 | 见过的枚举 |')
    out.append('|---|---:|---:|---|---|')
    missing = []
    for n, per, name in calls:
        kind, status = cov.get(name, ('?', 'missing'))
        if status != 'yes':
            missing.append(name)
        ev = []
        for (fn, arg), vals in enums.items():
            if fn == name:
                ev.extend(vals)
        mark = '✅' if status == 'yes' else '⚠️ ' + status
        out.append('| `%s` | %d | %.1f | %s | %s |' % (name, n, per, mark, ', '.join(sorted(set(ev)))[:200]))
    out.append('')
    out.append('## 被调用但 espgpu 模式未实现')
    out.append('')
    out.extend(['- `%s`' % m for m in missing] or ['（无）'])
    out.append('')
    out.append('## wglGetProcAddress 请求')
    out.append('')
    for name, st in procs:
        out.append('- `%s` — %s' % (name, st))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
