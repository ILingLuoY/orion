"""找出所有"编译产物比源码旧"的 SPV，重编，并把真正发生变化的部署到 run/shaders。

判据不靠手写 include 图：对每个有 .spv 的源文件，递归算出"它自己 + 它 include 到的
所有文件"里最新的 mtime，与 .spv 的 mtime 比 —— 这就是"共享 include 改了但没重编"
的直接检测（2026-10-07 另一个会话漏编 sky_atmosphere/volumetric_clouds/volumetric_light
正是这种情况）。

重编之后再和 run/shaders 里的副本逐字节比：只有真的变了才覆盖 —— 于是"其实没受影响"
的着色器不会被无谓地换掉（也能当作影响面的证据）。
"""

import glob
import hashlib
import os
import re
import subprocess
import sys

ROOT = 'cpp/shaders'
RUN = 'run/shaders'
GLSLANG = r'C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe'
# 规范编译口径（2026-10-21 起）：生产档（材质在命中着色器里求值）
# + 地形 32B 顶点。两者都是**编译期**决定，漏一个就会静默换档 / 换顶点 ABI。
# 与 tools/audit_spv_freshness.py 的 BASE_FLAGS 必须逐项一致。
BASE_FLAGS = ['-DRT_BUILD_PRODUCTION_MODE', '-DSECTION_VERTEX_32B=1']
# 2026-11-16：RT 阶段三计数器编译期总闸。与 C++ 侧 getenv 同名；缺省（未设 / 0 / f）
# 不追加宏 ⇒ 着色器整段裁掉，编出的 spv 与基线逐位可比。采集时置
# ORION_RT_STAGE_COUNTERS=1 再跑本脚本，才会把 -DORION_RT_STAGE_COUNTERS=1 带进去。
_RT_COUNTERS_ENV = os.environ.get('ORION_RT_STAGE_COUNTERS')
RT_COUNTERS_FLAGS = (
    ['-DORION_RT_STAGE_COUNTERS=1']
    if _RT_COUNTERS_ENV and _RT_COUNTERS_ENV not in ('0', 'f', 'F', '')
    else []
)
# 2026-11-02：玻璃"解析半侧"的拆分是跨 TU 的 —— rgen 把确定性反射从 RR 输入里
# 减掉，显示变换再把旁路图加回来。三个 TU 必须同一个宏值，否则那部分能量直接消失
# （玻璃反射全黑）或重复计入。rgen 侧的定义在 rt_test.rgen；这两个 comp 靠这里传。
EXTRA_FLAGS = {
    'rr_sdr_post.comp.spv': [],
    'rr_hdr_encode.comp.spv': [],
    # 2026-11-08：关掉 edge-aware，回到 2026-11-02 的纯 rate 掩码。
    # 打开时（11-04 21:35 起）掩码会把高对比像素抬回全速率
    # （sparse_active_mask.comp:56  bestDepth>0.15 || bestLuma>0.35），
    # 实际活跃率远高于请求值 ⇒ 30% 档的收益从 -46% 掉到约 -30%。
    # 回退：把下面这行改回 ['-DSPARSE_EDGE_AWARE=1'] 并重编该 comp。
    'sparse_active_mask.comp.spv': ['-DSPARSE_ORDERED_BAYER=0'],
    # 2026-11-05 B：K1/K2 变体的编译期开关。stale 路径（EXTRA_FLAGS.get）与
    # 下方 FORCE 式补录共用同一份 ⇒ 漏了会把变体重编成主变体。
    'rt_test_k1.rgen.spv': ['-DORION_K1_PRIMARY'],
    'rt_test_k1_ser.rgen.spv': ['-DORION_K1_PRIMARY', '-DRT_ENABLE_SER'],
    'rt_test_k2.rgen.spv': ['-DORION_K2_TRANSPORT'],
    'rt_test_k2_ser.rgen.spv': ['-DORION_K2_TRANSPORT', '-DRT_ENABLE_SER'],
    'rt_test_k1.rchit.spv': ['-DORION_K1_PRIMARY'],
    # 2026-11-05 D1/B①：K2 内联材质版（ORION_K2_VARIANT=2 加载）
    'rt_test_k2_inline.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE'],
    # 2026-11-05 ③：K2-D（直射级）/ K2-I（间接级）拆分变体（ORION_K2_SPLIT=1 加载）
    'rt_test_k2d.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_STAGE'],
    'rt_test_k2d_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_STAGE', '-DRT_ENABLE_SER'],
    'rt_test_k2i.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_INDIRECT_STAGE'],
    'rt_test_k2i_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_INDIRECT_STAGE', '-DRT_ENABLE_SER'],
    # 2026-11-12 ④：K2-D 再拆分三腿变体（ORION_K2_SPLIT2=0 回退 ③ 两腿）。
    # SUN = 水+太阳直射/GGX/SSS/RTAO（写 binding 89）；LOC = 局部光/水面反射
    # 太阳/GGX 镜面（load-add 复用 binding 89）。K2-I 不变。
    'rt_test_k2sun.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE'],
    'rt_test_k2sun_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE', '-DRT_ENABLE_SER'],
    'rt_test_k2loc.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE'],
    'rt_test_k2loc_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DRT_ENABLE_SER'],
}
# 宏值改了但源码没改 ⇒ mtime 判据看不出脏，必须无条件重编。
FORCE_SPV = ['rr_sdr_post.comp.spv', 'rr_hdr_encode.comp.spv', 'sparse_active_mask.comp.spv']
STAGE = {'.rgen': 'rgen', '.rchit': 'rchit', '.rahit': 'rahit', '.rmiss': 'rmiss', '.comp': 'comp',
         # 2026-11-05 D1：材质 callable TU
         '.rcall': 'rcall'}


def read(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as handle:
            return handle.read()
    except OSError:
        return ''


def includes_of(path):
    out = []
    for line in read(path).split('\n'):
        if '#include' in line:
            match = re.search(r'"([^"]+)"', line)
            if match:
                candidate = os.path.join(ROOT, os.path.basename(match.group(1)))
                if os.path.exists(candidate):
                    out.append(candidate)
    return out


def newest_mtime(path, seen=None):
    if seen is None:
        seen = set()
    path = os.path.normpath(path)
    if path in seen:
        return 0.0
    seen.add(path)
    if not os.path.exists(path):
        return 0.0
    newest = os.path.getmtime(path)
    for include in includes_of(path):
        newest = max(newest, newest_mtime(include, seen))
    return newest


def digest(path):
    try:
        with open(path, 'rb') as handle:
            return hashlib.md5(handle.read()).hexdigest()
    except OSError:
        return None


def build(source, spv, target_env, extra):
    stage = STAGE[os.path.splitext(source)[1]]
    command = [GLSLANG, '-V', '--target-env', target_env, '-S', stage, '-e', 'main']
    command += BASE_FLAGS
    command += RT_COUNTERS_FLAGS
    command += extra
    command += ['-o', spv, source]
    result = subprocess.run(command, capture_output=True, text=True)
    return result.returncode == 0, (result.stdout + result.stderr).strip()


def main():
    targets = []
    stale = []

    for path in sorted(glob.glob(ROOT + '/*')):
        extension = os.path.splitext(path)[1]
        if extension not in STAGE:
            continue
        spv = path + '.spv'
        if not os.path.exists(spv):
            continue
        newest = newest_mtime(path)
        if newest > os.path.getmtime(spv):
            stale.append(path)
            targets.append((path, spv, EXTRA_FLAGS.get(os.path.basename(spv), [])))

    for _forced in FORCE_SPV:
        _source = ROOT + '/' + _forced[:-len('.spv')]
        _spv = ROOT + '/' + _forced
        if os.path.exists(_source) and _spv not in [t[1] for t in targets]:
            targets.append((_source, _spv, EXTRA_FLAGS.get(_forced, [])))

    # 2026-10-28：**主 raygen 变体无条件重编**。
    # 今天踩到的坑：rt_test.rgen.spv 的 mtime（20:15）比源码（19:53）新，于是被判"不脏"，
    # 但它与"当前源码 + 规范 flags"编出来的结果不一致（618900 vs 619200 B）——
    # 也就是说运行中的 raygen 并不是仓库里的源码，一整轮诊断跑在旧代码上。
    # mtime 不可信（拷贝/回滚都会改它），raygen 一律重编。
    _main_rgen = (ROOT + '/rt_test.rgen', ROOT + '/rt_test.rgen.spv', [])
    if _main_rgen[1] not in [t[1] for t in targets]:
        targets.append(_main_rgen)

    # SER 变体：同一份源码 + -DRT_ENABLE_SER（只有这两个存在）。
    # 它们与 fallback 是两套独立模块，但源码相同 ⇒ 共享 include 一改就必须一起重编，
    # 否则 perfSerAb（SER A/B 开关）两边会跑在不同版本的光照上。
    for source, spv in (('rt_test.rgen', 'rt_test_ser.rgen.spv'),
                        ('rt_test.rmiss', 'rt_test_ser.rmiss.spv')):
        full_source = ROOT + '/' + source
        full_spv = ROOT + '/' + spv
        # ⚠ 2026-10-28 修正：去重判据必须按**产物名**（t[1]），不能按源文件名（t[0]）。
        # 上面"主 raygen 无条件重编"会把 rt_test.rgen 放进 targets，于是按源名判断时
        # rt_test_ser.rgen.spv 永远命中"该源已在列表里" ⇒ 被静默跳过。
        # 2026-10-28 实测：rt_test.rgen 主变体已重编、SER 变体仍停在上一轮（631884 B），
        # 也就是 SER 臂会跑在旧代码上 —— 正是这段判据造成的。
        if os.path.exists(full_spv) and full_spv not in [t[1] for t in targets]:
            targets.append((full_source, full_spv, ['-DRT_ENABLE_SER']))

    # SHaRC update 变体：同一份源码 + -DSHARC_UPDATE=1（照参考的 permutation 做法，
    # 见 tools/audit_spv_freshness.py 里 VARIANTS 的说明）。它与渲染 pass 共享同一
    # 个 include 闭包 ⇒ 共享头一改必须一起重编，否则 update 与 query 会跑在
    # 不同版本的 SHaRC 语义上（那正是本工程在 camera_ubo.glsl 上吃过的亏）。
    # 不设 os.path.exists 门：首次生成时产物尚不存在。
    _sharc_update_spv = ROOT + '/rt_test_sharc_update.rgen.spv'
    if _sharc_update_spv not in [t[1] for t in targets]:
        targets.append((ROOT + '/rt_test.rgen', _sharc_update_spv, ['-DSHARC_UPDATE=1']))

    # 2026-10-24 半分辨率 GI 变体：同一份源码 + -DGI_HALFRES=1。
    # 它把 pixelCoord 换成 launchID*2+phase（子像素相位逐帧轮转），并在 GI 块末尾
    # 写 giHalfImage 后 return。与主变体**共享同一个 include 闭包** ⇒ 共享头一改
    # 必须一起重编，否则两臂的材质/相机语义会悄悄分叉（照 SHaRC update 的教训）。
    # 不设 os.path.exists 门：首次生成时产物尚不存在。
    _gi_half_spv = ROOT + '/rt_test_gi_half.rgen.spv'
    if _gi_half_spv not in [t[1] for t in targets]:
        targets.append((ROOT + '/rt_test.rgen', _gi_half_spv, ['-DGI_HALFRES=1']))

    # POM 用途分级变体：同一份源码 + -DPOM_DEFERRED_ONLY=1。
    # 关（默认宏=0）时预处理后与改动前逐字节相同，所以"开"只能靠这个独立产物；
    # 两者共享同一个 include 闭包 ⇒ 共享头一改必须一起重编，否则 A/B 两臂会跑在
    # 不同版本的材质/相机语义上。说明见 rt_test.rchit 里 POM_DEFERRED_ONLY 处。
    _pom_deferred_spv = ROOT + '/rt_test_pomdeferred.rchit.spv'
    if _pom_deferred_spv not in [t[1] for t in targets]:
        targets.append((ROOT + '/rt_test.rchit', _pom_deferred_spv, ['-DPOM_DEFERRED_ONLY=1']))

    # 2026-11-05 B：K1/K2 两趟拆分的 4 个变体。
    #   K1（-DORION_K1_PRIMARY）= 主视线 + payload 捕获 + landing 落盘；
    #   K2（-DORION_K2_TRANSPORT）= 从 landing 续传输段。
    #   SER 版照抄主 raygen 的选择条件（Sharc.cpp:2437 优先加载 _ser）。
    # 与主变体共享同一个 include 闭包 ⇒ 共享头一改必须一起重编，否则两趟会跑在
    # 不同版本的相机/材质语义上（照 SHaRC update / GI-half 的既有教训）。
    # 不设 os.path.exists 门：首次生成时产物尚不存在。
    for _k12_spv, _k12_defines in (
            ('rt_test_k1.rgen.spv', ['-DORION_K1_PRIMARY']),
            ('rt_test_k1_ser.rgen.spv', ['-DORION_K1_PRIMARY', '-DRT_ENABLE_SER']),
            ('rt_test_k2.rgen.spv', ['-DORION_K2_TRANSPORT']),
            ('rt_test_k2_ser.rgen.spv', ['-DORION_K2_TRANSPORT', '-DRT_ENABLE_SER'])):
        _k12_full = ROOT + '/' + _k12_spv
        if _k12_full not in [t[1] for t in targets]:
            targets.append((ROOT + '/rt_test.rgen', _k12_full, _k12_defines))

    # 2026-11-05 ③：K2 内联材质版（B① 复用）+ K2-D/K2-I 直射/间接拆分变体。
    # 同样以 rt_test.rgen 为源、EXTRA_FLAGS 提供各自的宏组合。
    for _k2x_spv in ('rt_test_k2_inline.rgen.spv',
                     'rt_test_k2d.rgen.spv',
                     'rt_test_k2d_ser.rgen.spv',
                     'rt_test_k2i.rgen.spv',
                     'rt_test_k2i_ser.rgen.spv',
                     'rt_test_k2sun.rgen.spv',
                     'rt_test_k2sun_ser.rgen.spv',
                     'rt_test_k2loc.rgen.spv',
                     'rt_test_k2loc_ser.rgen.spv'):
        _k2x_full = ROOT + '/' + _k2x_spv
        if _k2x_full not in [t[1] for t in targets]:
            targets.append((ROOT + '/rt_test.rgen', _k2x_full,
                            EXTRA_FLAGS[_k2x_spv]))

    # 2026-11-05 B①：K1 主命中组的精简 rchit（只落命中身份，材质解析延期 K2）。
    for _k1r_spv, _k1r_defines in (('rt_test_k1.rchit.spv', ['-DORION_K1_PRIMARY']),):
        _k1r_full = ROOT + '/' + _k1r_spv
        if _k1r_full not in [t[1] for t in targets]:
            targets.append((ROOT + '/rt_test.rchit', _k1r_full, _k1r_defines))

    # 2026-11-05 D1：材质 callable TU。生成文件（.tmp/rgen_split/gen_material_callable.py
    # 从 rt_test.rchit 前言逐字节生成），单一事实源是 rt_test.rchit ⇒ rchit 前言
    # 改动后它自动变 stale（mtime 判据走 newest_mtime）。
    _mat_spv_full = ROOT + '/rt_test_material.spv'
    if _mat_spv_full not in [t[1] for t in targets]:
        targets.append((ROOT + '/rt_test_material.rcall', _mat_spv_full, []))

    print('== 过期（源或其 include 比 .spv 新）==')
    for path in stale:
        print('   ', os.path.basename(path))
    print()
    print('== 重编 %d 个产物 ==' % len(targets))

    changed = []
    same = []
    failed = []

    for source, spv, extra in targets:
        before = digest(spv)
        ok, output = build(source, spv, 'vulkan1.2', extra)
        if not ok:
            ok, output = build(source, spv, 'vulkan1.4', extra)
            note = ' (vulkan1.4)'
        else:
            note = ' (vulkan1.2)'
        name = os.path.basename(spv)
        if not ok:
            failed.append((name, output))
            print('   FAIL %-34s%s' % (name, note))
            continue
        after = digest(spv)
        if after == before:
            same.append(name)
            print('   same %-34s%s  （逐字节未变 => 该着色器其实不受影响）' % (name, note))
        else:
            changed.append(name)
            print('   DIFF %-34s%s  %s -> %s' % (name, note, (before or '-')[:8], (after or '-')[:8]))

    print()
    # 2026-10-26：**必须同时写三个位置**，否则白干。
    #   run/shaders/                      —— 运行时唯一读取位置
    #                                        （loadSpv = <rtx_backend.dll 所在目录>/shaders/x.spv，
    #                                         开发环境即 run/shaders）
    #   src/main/resources/shaders/       —— 随包资源（真源头，打包进 jar）
    #   build/resources/main/shaders/     —— 开发态的 mod root（jar 的等价物）
    # 关键：NativeVulkan.extractBundledShaders() 每次启动都会用 Files.mismatch
    # 把 mod root 里的副本同步到 run/shaders/（不同就覆盖）。只写 run/ 的话，
    # 下一次启动就被旧档刷回去 —— 2026-10-26 因此白改了好几轮。
    DEPLOY_DIRS = [RUN, 'src/main/resources/shaders', 'build/resources/main/shaders']
    print('== 部署到 %d 处：%s ==' % (len(DEPLOY_DIRS), ', '.join(DEPLOY_DIRS)))
    deployed = 0
    for source, spv, extra in targets:
        name = os.path.basename(spv)
        if name not in changed:
            continue
        mine = digest(spv)
        wrote = []
        for target_dir in DEPLOY_DIRS:
            if not os.path.isdir(target_dir):
                continue
            destination = os.path.join(target_dir, name)
            if digest(destination) == mine:
                continue
            with open(spv, 'rb') as src_handle, open(destination, 'wb') as dst_handle:
                dst_handle.write(src_handle.read())
            wrote.append(target_dir)
        if wrote:
            deployed += 1
            print('   %-34s 已部署 %s -> %s' % (name, mine[:8], ' + '.join(wrote)))
        else:
            print('   %-34s 三处已是同一份' % name)

    print()
    print('汇总：过期 %d / 重编 %d / 有变化 %d / 已部署 %d / 未变 %d / 失败 %d'
          % (len(stale), len(targets), len(changed), deployed, len(same), len(failed)))
    for name, output in failed:
        print('---- %s ----' % name)
        print(output[:1500])
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
