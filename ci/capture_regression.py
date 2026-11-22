# -*- coding: utf-8 -*-
"""确定性抓帧哈希：收集与比对（拆 rgen 等重构的回归安全网）。

背景
----
拆 raygen / 折叠 traceRay / 提取 main 里的步骤函数，改的都是求值路径。
现有 benchmark（tools/orion_bench）只记帧时间，回答不了"画面有没有变"；
而 glslang 不会内联 GLSL 函数，函数化后 SPIR-V 必然多出 OpFunctionCall，
所以"SPV 是否逐字节一致"不能当判据。唯一可靠的判据是渲染结果本身。

做法
----
native 侧（cpp/backend/Frame.cpp 的 orionCaptureDeterministicFrameHash）
在 ORION_CAPTURE_HASH=<帧号> 时，把 g_rtOutputImage —— 也就是 DLSS-RR 的
输入、渲染分辨率的 HDR 结果（rgba16f）—— 读回并算 FNV-1a 64，写一行：

    [CAPTURE] frame=300 hash=0x0123456789abcdef w=1920 h=1080 nonzero=...

这张图不受超分、补帧、swapchain/present 路径影响，是重构最直接的作用对象。

用法
----
    # 1. 改前基线（PowerShell）
    $env:ORION_CAPTURE_HASH=300
    .\\gradlew.bat runClient
    #    进世界，用固定相机路径走到该帧，画面稳定后退出

    # 2. 收集
    python ci/capture_regression.py collect --tag baseline

    # 3. 改完重跑（同样的环境变量与相机路径），再收集
    python ci/capture_regression.py collect --tag after

    # 4. 比对
    python ci/capture_regression.py compare baseline after

退出码：一致 0，不一致 1。

前置条件（缺一个比对就无意义）
------------------------------
- 相机必须固定：用 orion.campath 回放同一路点，两次输入才会逐位相同。
- 关掉 ORION_RT_PIPE_HOT_RELOAD，避免中途换管线。
- 分辨率、rrMode、sparseRatePercent、giBounces 等必须相同（存档在 json 里供人工核对）。
- 日志是 append 模式：同一帧号出现多次时取**最后一条**（本次运行）。
"""

import argparse
import io
import json
import os
import re
import sys
import time

DEFAULT_LOG = os.path.join('run', 'rt_debug.log')
DEFAULT_STORE = os.path.join('run', 'benchmark', 'captures')

CAPTURE_RE = re.compile(
    r'\[CAPTURE\]\s+frame=(\d+)\s+hash=(0x[0-9a-fA-F]+)\s+'
    r'w=(\d+)\s+h=(\d+)\s+nonzero=(\d+)'
)


def parse_captures(log_path):
    """返回 {frame(int): {...}}；同帧号后出现的覆盖先出现的。"""
    if not os.path.exists(log_path):
        sys.exit('日志不存在：%s' % log_path)

    records = {}
    seen = 0

    with io.open(log_path, 'r', encoding='utf-8', errors='replace') as handle:
        for line in handle:
            match = CAPTURE_RE.search(line)
            if not match:
                continue

            seen += 1
            frame = int(match.group(1))
            records[frame] = {
                'frame': frame,
                'hash': match.group(2).lower(),
                'width': int(match.group(3)),
                'height': int(match.group(4)),
                'nonzero': int(match.group(5)),
            }

    return records, seen


def cmd_collect(args):
    records, seen = parse_captures(args.log)

    if not records:
        sys.exit(
            '日志里没有 [CAPTURE] 记录。请确认：\n'
            '  1) ORION_CAPTURE_HASH=<帧号> 已设置且客户端确实跑到该帧；\n'
            '  2) run/rtx_backend.dll 是最新构建（含 orionCaptureDeterministicFrameHash）；\n'
            '  3) 抓帧发生在帧首，读的是上一帧结果，所以目标帧号不能太靠近启动。'
        )

    os.makedirs(args.store, exist_ok=True)
    out_path = os.path.join(args.store, '%s.json' % args.tag)

    payload = {
        'tag': args.tag,
        'log': os.path.abspath(args.log),
        'collectedAt': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'logHasCaptureLines': seen,
        'frames': {str(k): v for k, v in sorted(records.items())},
    }

    with io.open(out_path, 'w', encoding='utf-8', newline='') as handle:
        handle.write(json.dumps(payload, indent=2, ensure_ascii=False))
        handle.write(u'\n')

    print('收集完成：%s' % out_path)
    print('  帧号：%s' % ', '.join(str(k) for k in sorted(records)))
    print('  （本次日志共 %d 条 [CAPTURE] 行，同帧号取最后一条）' % seen)

    return 0


def cmd_compare(args):
    a_path = os.path.join(args.store, '%s.json' % args.a)
    b_path = os.path.join(args.store, '%s.json' % args.b)

    for path in (a_path, b_path):
        if not os.path.exists(path):
            sys.exit('缺少快照：%s' % path)

    with io.open(a_path, 'r', encoding='utf-8') as handle:
        a = json.load(handle)
    with io.open(b_path, 'r', encoding='utf-8') as handle:
        b = json.load(handle)

    a_frames = a.get('frames', {})
    b_frames = b.get('frames', {})

    common = sorted(set(a_frames) & set(b_frames), key=int)

    if not common:
        sys.exit(
            '两份快照没有共同帧号（%s: %s / %s: %s）。'
            % (args.a, sorted(a_frames)[:8], args.b, sorted(b_frames)[:8])
        )

    failures = []
    shape_mismatch = []

    for frame in common:
        left = a_frames[frame]
        right = b_frames[frame]

        if (left['width'], left['height']) != (right['width'], right['height']):
            shape_mismatch.append(
                'frame %s: %dx%d vs %dx%d'
                % (frame, left['width'], left['height'],
                   right['width'], right['height'])
            )
            continue

        if left['hash'] != right['hash']:
            failures.append(
                'frame %s: %s (nonzero=%d) vs %s (nonzero=%d)'
                % (frame, left['hash'], left['nonzero'],
                   right['hash'], right['nonzero'])
            )

    print('比对 %s vs %s（共同帧 %d 个）' % (args.a, args.b, len(common)))

    if shape_mismatch:
        print('\n分辨率不一致（比对前提被破坏，请先对齐设置）：')
        for line in shape_mismatch:
            print('  ' + line)

    if failures:
        print('\n哈希不一致 %d 帧 —— 渲染结果发生了变化：' % len(failures))
        for line in failures:
            print('  ' + line)
        print(
            '\n下一步：先确认两次运行的设置/相机路径是否一致；'
            '一致则说明重构改变了行为，回到最小改动重试。'
        )
        return 1

    print('\n全部一致：%d 帧逐位相同。' % len(common))
    print('  %s' % '、'.join('frame %s = %s' % (
        f, a_frames[f]['hash']) for f in common[:4]))
    return 0


def main():
    parser = argparse.ArgumentParser(
        description='确定性抓帧哈希的收集与比对（DLSS 输入图，逐位判据）'
    )
    parser.add_argument(
        '--log', default=DEFAULT_LOG, help='rt_debug.log 路径'
    )
    parser.add_argument(
        '--store', default=DEFAULT_STORE, help='快照存放目录'
    )

    sub = parser.add_subparsers(dest='command')

    collect = sub.add_parser('collect', help='从日志提取 [CAPTURE] 记录')
    collect.add_argument('--tag', required=True, help='快照名，如 baseline')
    collect.set_defaults(func=cmd_collect)

    compare = sub.add_parser('compare', help='比对两份快照')
    compare.add_argument('a', help='快照 A 的 tag')
    compare.add_argument('b', help='快照 B 的 tag')
    compare.set_defaults(func=cmd_compare)

    args = parser.parse_args()

    if not getattr(args, 'command', None):
        parser.print_help()
        return 2

    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
