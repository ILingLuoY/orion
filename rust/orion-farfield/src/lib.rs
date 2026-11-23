//! Orion 本体 Rust 加速 — `orion_farfield` cdylib。
//!
//! 当前内容：
//! 1. DH 远场转换核心（M2，已对拍 40/40，未接生产——M3 挂起，见 HANDOFF §5/§7）；
//! 2. 粒子天气几何内核（2026-11-17 本体加速第一枪）：`ParticleRtCapture.buildRtWeather`
//!    + `appendWeatherSnow` + `emitQuad/emit` 的 Rust 镜像，探针前置两段式 ABI
//!    （plan → Java 探针 → build）。
//!
//! FFI 纪律（所有导出）：
//! - `catch_unwind` 包住，panic → 各自的错误码，绝不跨 FFI 展开；
//! - ABI 全部 32 位标量 + 指针（Windows 上 C long 是 32 位，规避 JAVA_LONG 平台差异）；
//! - 输入只读；输出缓冲由调用方分配传容量，返回实际写入数；
//! - 结构体一律 `#[repr(C)]`，字段顺序冻结（Java 侧 ByteBuffer 按同序写入）。
//!
//! 对拍门（bench/OrionFarFieldBench.java）：
//! - DH 转换：逐位一致；
//! - 天气内核：除旋转影响的顶点 XYZ（容差 2e-4 绝对，JOML 公式 ulp 级差异）外逐位一致。

use std::panic::AssertUnwindSafe;

/// ABI 版本：调用方加载后先核对，不匹配立即拒绝加载。
#[no_mangle]
pub extern "C" fn off_abi_version() -> u32 {
    3
}

const ERR_PANIC: u32 = u32::MAX;
const ERR_CAP: u32 = u32::MAX - 1;

// Java int 字面量超过 i32::MAX 的常量（Java 溢出环绕 = u32 位模式重解读）
const H1: i32 = 0x9e3779b1u32 as i32; // 世界格 x 混入
const H2: i32 = 0x85ebca77u32 as i32; // 世界格 z 混入
const H3: i32 = 0x9E3779B9u32 as i32; // 雪片 seed 混入（× (flake+1)）
const H4: i32 = 0x85ebca6bu32 as i32; // 雪片尺寸 seed
const H5: i32 = 0xc2b2ae35u32 as i32; // 雪片 variant seed
const H6: i32 = 0x846ca68bu32 as i32; // hash32 第二乘数

// ===================================================================
// M0 基线
// ===================================================================

/// 基线导出：u32 求和（f64 返回，和 < 2^53 内精确）。
///
/// # Safety
/// `ptr` 可读 `len` 个 u32（或 ptr 为 null / len 为 0）。
#[no_mangle]
pub unsafe extern "C" fn off_sum_u32(ptr: *const u32, len: u32) -> f64 {
    match std::panic::catch_unwind(AssertUnwindSafe(|| {
        if ptr.is_null() || len == 0 {
            return 0.0f64;
        }
        std::slice::from_raw_parts(ptr, len as usize)
            .iter()
            .map(|&x| x as f64)
            .sum()
    })) {
        Ok(v) => v,
        Err(_) => f64::NAN,
    }
}

// ===================================================================
// DH 远场转换核心（M2，契约冻结）
// ===================================================================

#[inline(always)]
fn rd_u16(src: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([src[i], src[i + 1]])
}

#[inline(always)]
fn wr_u16(dst: &mut [u8], i: usize, v: u16) {
    dst[i] = v as u8;
    dst[i + 1] = (v >> 8) as u8;
}

/// # Safety
/// `src` 可读 `src_len` 字节；`dst` 可写 `dst_cap` 字节；两段内存不得重叠。
#[no_mangle]
pub unsafe extern "C" fn off_convert_dh_section(
    src: *const u8,
    src_len: u32,
    dst: *mut u8,
    dst_cap: u32,
    block_width: u32,
    texture_enabled: u32,
) -> u32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if src_len == 0 {
            return 0u32;
        }
        let src = std::slice::from_raw_parts(src, src_len as usize);
        let dst = std::slice::from_raw_parts_mut(dst, dst_cap as usize);
        convert_inner(src, dst, block_width, texture_enabled != 0)
    }));
    match result {
        Ok(n) => n,
        Err(_) => ERR_PANIC,
    }
}

fn convert_inner(src: &[u8], dst: &mut [u8], block_width: u32, texture_enabled: bool) -> u32 {
    let limit = block_width as u64 + 1;
    let quads = src.len() / 64;

    // 单趟融合（与 Java 逐位一致；Java 因定长分配被迫两趟）
    let mut at = 0usize;
    for q in 0..quads {
        let base = q * 64;
        let mut ok = true;
        for v in 0..4usize {
            let s = base + v * 16;
            if rd_u16(src, s) as u64 > limit || rd_u16(src, s + 4) as u64 > limit {
                ok = false;
                break;
            }
        }
        if !ok {
            continue;
        }
        if at + 64 > dst.len() {
            return ERR_CAP;
        }
        for v in 0..4usize {
            let s = base + v * 16;
            let d = at + v * 16;
            wr_u16(dst, d, rd_u16(src, s)); // x
            wr_u16(dst, d + 2, rd_u16(src, s + 2)); // y
            wr_u16(dst, d + 4, rd_u16(src, s + 4)); // z
            let tile = if texture_enabled { rd_u16(src, s + 14) } else { 0 };
            wr_u16(dst, d + 6, tile); // textureTileId
            wr_u16(dst, d + 8, (src[s + 13] & 0x0F) as u16); // normalIndex（仅低 4 位）
            dst[d + 10] = src[s + 8]; // R
            dst[d + 11] = src[s + 9]; // G
            dst[d + 12] = src[s + 10]; // B
            dst[d + 13] = src[s + 6]; // light（(sky<<4)|block，原样拷贝）
            dst[d + 14] = src[s + 12]; // material
            dst[d + 15] = 0; // 未用
        }
        at += 64;
    }
    at as u32
}

// ===================================================================
// 粒子天气几何内核（本体加速 S 级，2026-11-17）
// ===================================================================

/// 天气参数块。Java 侧 `weatherParamsBuf`（direct ByteBuffer）按同序写入，
/// 字段顺序冻结；全部 4 字节字，无对齐缝隙。
#[repr(C)]
pub struct WeatherParams {
    pub player_x: f32,
    pub player_z: f32,
    pub eye_y: f32,
    pub seconds: f32,
    pub cell_spacing: f32,
    pub anchor_x: i32,
    pub anchor_z: i32,
    pub grid_size: i32,
    pub max_rain_drops: i32,
    pub max_weather_quads: i32,
    pub any_fill: f32,
    pub rain_fill: f32,
    pub snow_fill: f32,
    pub rain_scale: f32,
    pub rain_half_width: f32,
    pub wind_x: f32,
    pub wind_z: f32,
    pub rain_color: i32,
    pub snow_color: i32,
    pub cam_x: f32,
    pub cam_y: f32,
    pub cam_z: f32,
    pub cam_w: f32,
    pub snow_fall_speed_scale: f32,
    pub snow_sway_blocks: f32,
    pub snow_size_scale: f32,
    pub near_fade_inner: f32,
    pub near_fade_outer: f32,
    pub snow_tile_v_base: f32,
    pub snow_fall_min: f32,
    pub snow_fall_max: f32,
    pub snow_sway_frequency: f32,
    pub snow_flake_min: f32,
    pub snow_flake_max: f32,
    pub rt_rain_marker: i32,
    pub rt_snow_marker: i32,
    pub rt_snow_flakes_per_cell: i32,
}

#[inline(always)]
fn hash32(mut v: i32) -> i32 {
    v ^= ((v as u32) >> 16) as i32;
    v = v.wrapping_mul(0x7feb352d);
    v ^= ((v as u32) >> 15) as i32;
    v = v.wrapping_mul(H6);
    v ^ ((v as u32) >> 16) as i32
}

/// Java `unitFromSeed`：(value & 0x00FFFFFF) * (1.0F / 16777216.0F)。
#[inline(always)]
fn unit32(v: i32) -> f32 {
    (v & 0x00FF_FFFF) as f32 * (1.0f32 / 16_777_216.0f32)
}

/// Java `fract`：value - (float)Math.floor(value)（floor 在 double 域做，逐位一致）。
#[inline(always)]
fn fract_java(v: f32) -> f32 {
    v - ((v as f64).floor() as f32)
}

/// Java `(float)Math.sin(x)`：参数先升 double，sin 在 double 域，再舍回 f32（逐位一致）。
#[inline(always)]
fn j_sin(v: f32) -> f32 {
    (v as f64).sin() as f32
}

/// Java `(float)Math.cos(x)`（同上）。
#[inline(always)]
fn j_cos(v: f32) -> f32 {
    (v as f64).cos() as f32
}

#[inline(always)]
fn smoothstep_java(edge0: f32, edge1: f32, value: f32) -> f32 {
    let t = ((value - edge0) / (edge1 - edge0)).clamp(0.0f32, 1.0f32);
    t * t * (3.0f32 - 2.0f32 * t)
}

/// JOML `Vector3f.set(nx,ny,0).rotate(q).mul(s).add(x,y,z)`。
/// JOML `Quaternionf.transform(FFF,dest)` 带 k = 1/(xx+yy+zz+ww) 归一化，此处按同构矩阵式
/// 复刻；公式阶序与 JOML 有 ulp 级差异 ⇒ 顶点 XYZ 走 2e-4 容差门（其余字段逐位一致）。
#[inline(always)]
fn q_vertex(
    qx: f32, qy: f32, qz: f32, qw: f32,
    nx: f32, ny: f32, scale: f32, px: f32, py: f32, pz: f32,
) -> (f32, f32, f32) {
    let xx = qx * qx;
    let yy = qy * qy;
    let zz = qz * qz;
    let ww = qw * qw;
    let k = 1.0f32 / (xx + yy + zz + ww);
    // 旋转矩阵（单位四元数；k 归一化与 JOML 同构）
    let m00 = (xx - yy - zz + ww) * k;
    let m01 = 2.0f32 * (qx * qy - qz * qw) * k;
    let m02 = 2.0f32 * (qx * qz + qy * qw) * k;
    let m10 = 2.0f32 * (qx * qy + qz * qw) * k;
    let m11 = (yy - xx - zz + ww) * k;
    let m12 = 2.0f32 * (qy * qz - qx * qw) * k;
    let m20 = 2.0f32 * (qx * qz - qy * qw) * k;
    let m21 = 2.0f32 * (qy * qz + qx * qw) * k;
    let m22 = (zz - xx - yy + ww) * k;
    let lx = nx * scale;
    let ly = ny * scale;
    (
        m00 * lx + m01 * ly + m02 * 0.0f32 + px,
        m10 * lx + m11 * ly + m12 * 0.0f32 + py,
        m20 * lx + m21 * ly + m22 * 0.0f32 + pz,
    )
}

/// 天气 quad 的紧凑运行时表示（从 params/探针推导，无 Minecraft 对象）。
struct WQuad {
    x: f32, y: f32, z: f32, scale: f32,
    u0: f32, u1: f32, v0: f32, v1: f32,
    xr: f32, yr: f32, zr: f32, wr: f32,
    color: i32, marker: i32,
    wind_x: f32, wind_z: f32,
}

#[inline(always)]
fn append_quad(buf: &mut Vec<WQuad>, cap: usize, q: WQuad) {
    if buf.len() < cap {
        buf.push(q);
    }
}

/// 每格计划：Java 探针坐标（7×i32/格）。
/// flags bit0 = 本格可能是 SNOW（cellUnit < snowFill），Java 才需要探 flake 高度。
#[repr(C)]
struct PlanCell {
    probe_bx: i32,
    probe_bz: i32,
    flake0_bx: i32,
    flake0_bz: i32,
    flake1_bx: i32,
    flake1_bz: i32,
    flags: i32,
}

fn plan_inner(p: &WeatherParams, plan: &mut [i32]) -> u32 {
    let n = p.max_rain_drops as usize;
    if plan.len() < n * 7 {
        return ERR_CAP;
    }
    let grid = p.grid_size;
    let half = grid / 2;
    // 锚点由 Java 前言在 double 域算好传入（f32 化会偶发错一格）
    let anchor_x = p.anchor_x;
    let anchor_z = p.anchor_z;
    for cell in 0..n {
        let grid_x = (cell % grid as usize) as i32;
        let grid_z = (cell / grid as usize) as i32;
        let world_cell_x = anchor_x + grid_x - half;
        let world_cell_z = anchor_z + grid_z - half;
        let seed = hash32(world_cell_x.wrapping_mul(H1) ^ world_cell_z.wrapping_mul(H2));
        let cell_unit = unit32(hash32(seed ^ 0x165667b1));
        let jitter_x = unit32(seed) - 0.5f32;
        let jitter_z = unit32(seed.wrapping_mul(1664525).wrapping_add(1013904223)) - 0.5f32;
        let world_x = (world_cell_x as f32 + jitter_x * 0.62f32) * p.cell_spacing;
        let world_z = (world_cell_z as f32 + jitter_z * 0.62f32) * p.cell_spacing;
        let snow_candidate = cell_unit < p.snow_fill;
        // 雪片摆动只依赖 seed/phase（纯数学），故 plan 期即可给出摆后探针坐标。
        // flakePhase/sway 用的 phase = unit32(seed*1103515245+12345)。
        let phase = unit32(seed.wrapping_mul(1103515245).wrapping_add(12345));
        let base_x = world_x.floor() as i32;
        let base_z = world_z.floor() as i32;
        let mut flake = [[0i32; 2]; 2];
        if snow_candidate {
            for f in 0..p.rt_snow_flakes_per_cell.max(0) as usize {
                let flake_seed = hash32(seed ^ H3.wrapping_mul(f as i32 + 1));
                let sway_phase = unit32(flake_seed ^ 0x51ed270b) * (std::f32::consts::PI * 2.0f32);
                let sway_angle = p.seconds * p.snow_sway_frequency * (std::f32::consts::PI * 2.0f32) + sway_phase;
                let sway_x = j_sin(sway_angle) * p.snow_sway_blocks;
                let sway_z = j_cos(sway_angle * 1.618f32) * p.snow_sway_blocks;
                flake[f][0] = (world_x + sway_x).floor() as i32;
                flake[f][1] = (world_z + sway_z).floor() as i32;
            }
        }
        let o = cell * 7;
        plan[o] = base_x;
        plan[o + 1] = base_z;
        plan[o + 2] = flake[0][0];
        plan[o + 3] = flake[0][1];
        plan[o + 4] = flake[1][0];
        plan[o + 5] = flake[1][1];
        plan[o + 6] = if snow_candidate { 1 } else { 0 };
    }
    n as u32
}

/// # Safety
/// `params` 指向一个完整的 `WeatherParams`；`plan` 可写 `plan_cells_cap × 7` 个 i32。
#[no_mangle]
pub unsafe extern "C" fn off_weather_plan(
    params: *const WeatherParams,
    plan: *mut i32,
    plan_cells_cap: u32,
) -> u32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let p = &*params;
        let plan = std::slice::from_raw_parts_mut(plan, plan_cells_cap as usize * 7);
        plan_inner(p, plan)
    }));
    result.unwrap_or(ERR_PANIC)
}

/// 天气构建：写入 `dst_v`（顶点，12×f32/顶点）与 `dst_m`（运动，3×f32/顶点），返回 quad 数。
///
// ===================================================================
// VPT 反照率累积 + 雨洼/植被标记（本体 A 级第一项，2026-11-17）
// 镜像 SectionCompilerMixin.accumulateVptAlbedoAndTagRainPuddles（两段式：plan→Java 探针→build）
// ===================================================================

/// 探针槽种类（plan 写，Java 按 kind+控制流探针，build 按 kind+谓词位复现生产分支）
const VPT_SLOT_ALBEDO_FRONT: u8 = 1;
const VPT_SLOT_ALBEDO_BACK: u8 = 2;
const VPT_SLOT_FOLIAGE_A: u8 = 3;
const VPT_SLOT_FOLIAGE_B: u8 = 4;
const VPT_SLOT_COLLAPSE_A: u8 = 5;
const VPT_SLOT_COLLAPSE_B: u8 = 6;
const VPT_SLOT_NONE: u8 = 0;
/// 探针位缓冲：已探槽 = bit0 isAir | bit1 leafFlower | bit2 canOcclude；
/// 槽存在但按控制流不需探 = 0xFF；槽未发（kind=0）= 0x00。
const VPT_BIT_UNPROBED: u8 = 0xFF;
const VPT_SLOT_BYTES: usize = 8; // kind u8, pad u8, x i16, y i16, z i16
const VPT_SLOTS_PER_QUAD: usize = 6;

/// 参数块（48 字节，字段序冻结；atlas_pixels 指针必须 8 对齐 → 放在 10 个 4 字节字之后）
#[repr(C)]
pub struct VptParams {
    pub min_x: i32,
    pub min_y: i32,
    pub min_z: i32,
    pub vertex_count: i32,
    pub stride: i32,
    pub atlas_width: i32,
    pub atlas_height: i32,
    pub cutout_layer: i32,
    pub do_albedo: i32,
    pub quad_cap: i32,
    pub atlas_pixels: *const u8,
}

#[inline(always)]
fn floor_i(v: f32) -> i32 {
    // Java (int) Math.floor(double(float_expr))：升 double 取整再饱和转 int
    (v as f64).floor() as i32
}

#[inline(always)]
fn sqrt_f(v: f32) -> f32 {
    // Java (float) Math.sqrt(double)：升 double 开方再舍回
    (v as f64).sqrt() as f32
}

#[inline(always)]
fn cross_normalize(x0: f32, y0: f32, z0: f32, x1: f32, y1: f32, z1: f32,
                   x2: f32, y2: f32, z2: f32) -> Option<(f32, f32, f32)> {
    let e1x = x1 - x0; let e1y = y1 - y0; let e1z = z1 - z0;
    let e2x = x2 - x0; let e2y = y2 - y0; let e2z = z2 - z0;
    let nx = e1y * e2z - e1z * e2y;
    let ny = e1z * e2x - e1x * e2z;
    let nz = e1x * e2y - e1y * e2x;
    let len = sqrt_f(nx * nx + ny * ny + nz * nz);
    if len > 1.0e-6f32 { Some((nx / len, ny / len, nz / len)) } else { None }
}

/// 与 Java quadSpan 一致（Math.max/min 对非 NaN 与 f32::max/min 等价）
#[inline(always)]
fn quad_span(a: f32, b: f32, c: f32, d: f32) -> f32 {
    let min = a.min(b).min(c).min(d);
    let max = a.max(b).max(c).max(d);
    max - min
}

fn vpt_write_slot(plan: &mut [u8], quad: usize, slot: usize, kind: u8, x: i32, y: i32, z: i32) {
    let o = (quad * VPT_SLOTS_PER_QUAD + slot) * VPT_SLOT_BYTES;
    if o + VPT_SLOT_BYTES > plan.len() { return; }
    plan[o] = kind;
    plan[o + 1] = 0;
    plan[o + 2..o + 4].copy_from_slice(&(x as i16).to_le_bytes());
    plan[o + 4..o + 6].copy_from_slice(&(y as i16).to_le_bytes());
    plan[o + 6..o + 8].copy_from_slice(&(z as i16).to_le_bytes());
}

fn vpt_plan_inner(p: &VptParams, src: &[u8], plan: &mut [u8]) -> u32 {
    if p.stride != 28 || p.vertex_count <= 0 || p.vertex_count % 4 != 0 {
        return ERR_CAP; // 契约违反（与 Java 前置守卫一致，不应到达）
    }
    // 生产实测：MC 26.x 的 mesh.vertexBuffer() 容量按「每 quad 一条 28B 记录」计，而 drawState.vertexCount()
    // 是渲染展开后的 4× 顶点数 ⇒ quad 上限必须按字节数 /112（4×28）钳，按 /28 钳会越界 4 倍。
    // quads==0 ⇒ 全部交回 Java 路径。
    let quads = ((p.vertex_count / 4) as usize).min(src.len() / 112);
    if quads == 0 {
        return 0;
    }
    if quads > p.quad_cap.max(0) as usize || plan.len() < quads * VPT_SLOTS_PER_QUAD * VPT_SLOT_BYTES {
        return ERR_CAP;
    }
    let gx = |base: usize| f32::from_le_bytes([src[base], src[base + 1], src[base + 2], src[base + 3]]);
    for quad in 0..quads {
        let mut cx = 0.0f32; let mut cy = 0.0f32; let mut cz = 0.0f32;
        let mut cu = 0.0f32; let mut cv = 0.0f32;
        let mut vx = [0.0f32; 4]; let mut vy = [0.0f32; 4]; let mut vz = [0.0f32; 4];
        for v in 0..4usize {
            let base = (quad * 4 + v) * 28;
            let x = gx(base); let y = gx(base + 4); let z = gx(base + 8);
            vx[v] = x; vy[v] = y; vz[v] = z;
            cx += x; cy += y; cz += z;
            cu += gx(base + 16); cv += gx(base + 20);
        }
        cx *= 0.25f32; cy *= 0.25f32; cz *= 0.25f32; cu *= 0.25f32; cv *= 0.25f32;

        // albedo 探针槽（仅几何可判定：doAlbedo 与法线非退化；front/back 各自的界内检查也是纯几何）
        if p.do_albedo != 0 {
            if let Some((nx, ny, nz)) = cross_normalize(vx[0], vy[0], vz[0], vx[1], vy[1], vz[1], vx[2], vy[2], vz[2]) {
                let fx = floor_i(cx - nx * 0.01f32);
                let fy = floor_i(cy - ny * 0.01f32);
                let fz = floor_i(cz - nz * 0.01f32);
                let front_ok = fx >= 0 && fx < 16 && fy >= 0 && fy < 16 && fz >= 0 && fz < 16;
                if front_ok {
                    vpt_write_slot(plan, quad, 0, VPT_SLOT_ALBEDO_FRONT, fx, fy, fz);
                    let bx = floor_i(cx + nx * 0.01f32);
                    let by = floor_i(cy + ny * 0.01f32);
                    let bz = floor_i(cz + nz * 0.01f32);
                    let back_ok = bx >= 0 && bx < 16 && by >= 0 && by < 16 && bz >= 0 && bz < 16;
                    if back_ok {
                        vpt_write_slot(plan, quad, 1, VPT_SLOT_ALBEDO_BACK, bx, by, bz);
                    }
                }
            }
        }

        // cutout 植被探针槽（cutout_layer && 法线非退化；A 恒发，B 恒发，Java 按结果决定是否探）
        if p.cutout_layer != 0 {
            if let Some((fnx, fny, fnz)) = cross_normalize(vx[0], vy[0], vz[0], vx[1], vy[1], vz[1], vx[2], vy[2], vz[2]) {
                vpt_write_slot(plan, quad, 2, VPT_SLOT_FOLIAGE_A,
                    floor_i(cx - fnx * 0.02f32), floor_i(cy - fny * 0.02f32), floor_i(cz - fnz * 0.02f32));
                vpt_write_slot(plan, quad, 3, VPT_SLOT_FOLIAGE_B, floor_i(cx), floor_i(cy), floor_i(cz));
            }
        }

        // collapse 雨洼探针槽（仅 flatCount==1 的 quad；坐标可出界 → i16，region 能跨节解析）
        let span_x = quad_span(vx[0], vx[1], vx[2], vx[3]);
        let span_y = quad_span(vy[0], vy[1], vy[2], vy[3]);
        let span_z = quad_span(vz[0], vz[1], vz[2], vz[3]);
        let flat_x = span_x <= 1.0e-4f32;
        let flat_y = span_y <= 1.0e-4f32;
        let flat_z = span_z <= 1.0e-4f32;
        let flat_count = (flat_x as i32) + (flat_y as i32) + (flat_z as i32);
        if flat_count == 1 {
            if flat_y {
                vpt_write_slot(plan, quad, 4, VPT_SLOT_COLLAPSE_A,
                    floor_i(cx), floor_i(cy - 0.01f32), floor_i(cz));
            } else {
                let side_x = if flat_x { 1.0f32 } else { 0.0f32 };
                let side_z = if flat_z { 1.0f32 } else { 0.0f32 };
                vpt_write_slot(plan, quad, 4, VPT_SLOT_COLLAPSE_A,
                    floor_i(cx - side_x * 0.01f32), floor_i(cy), floor_i(cz - side_z * 0.01f32));
                vpt_write_slot(plan, quad, 5, VPT_SLOT_COLLAPSE_B,
                    floor_i(cx + side_x * 0.01f32), floor_i(cy), floor_i(cz + side_z * 0.01f32));
            }
        }
    }
    (quads * VPT_SLOTS_PER_QUAD) as u32
}

/// # Safety
/// `params` 指向完整 `VptParams`；`src` 可读 `src_len` 字节；`plan` 可写 `plan_cap_bytes` 字节。
#[no_mangle]
pub unsafe extern "C" fn off_vpt_plan(
    params: *const VptParams,
    src: *const u8, src_len: u32,
    plan: *mut u8, plan_cap_bytes: u32,
) -> u32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let p = &*params;
        let src = std::slice::from_raw_parts(src, src_len as usize);
        let plan = std::slice::from_raw_parts_mut(plan, plan_cap_bytes as usize);
        plan.fill(0); // 未发槽 = 0x00
        vpt_plan_inner(p, src, plan)
    }));
    result.unwrap_or(ERR_PANIC)
}

#[allow(clippy::too_many_arguments)]
fn vpt_build_inner(
    p: &VptParams,
    vertices: &mut [u8],
    plan: &[u8],
    bits: &[u8],
    red: &mut [i32], green: &mut [i32], blue: &mut [i32], count: &mut [i32],
) -> u32 {
    // 与 plan 同一 /112 钳制（同一缓冲、同一容量视图），保证 build 只处理 plan 已发槽的 quad
    let quads = ((p.vertex_count / 4) as usize).min(vertices.len() / 112);
    let atlas_w = p.atlas_width;
    let atlas_h = p.atlas_height;
    let mut any = false;
    let get_slot = |quad: usize, s: usize| -> (u8, i16, i16, i16) {
        let o = (quad * VPT_SLOTS_PER_QUAD + s) * VPT_SLOT_BYTES;
        (plan[o], i16::from_le_bytes([plan[o + 2], plan[o + 3]]),
         i16::from_le_bytes([plan[o + 4], plan[o + 5]]), i16::from_le_bytes([plan[o + 6], plan[o + 7]]))
    };
    for quad in 0..quads {
        let mut cx = 0.0f32; let mut cy = 0.0f32; let mut cz = 0.0f32;
        let mut cu = 0.0f32; let mut cv = 0.0f32;
        let mut vx = [0.0f32; 4]; let mut vy = [0.0f32; 4]; let mut vz = [0.0f32; 4];
        for v in 0..4usize {
            let base = (quad * 4 + v) * 28;
            let x = f32::from_le_bytes([vertices[base], vertices[base + 1], vertices[base + 2], vertices[base + 3]]);
            let y = f32::from_le_bytes([vertices[base + 4], vertices[base + 5], vertices[base + 6], vertices[base + 7]]);
            let z = f32::from_le_bytes([vertices[base + 8], vertices[base + 9], vertices[base + 10], vertices[base + 11]]);
            vx[v] = x; vy[v] = y; vz[v] = z;
            cx += x; cy += y; cz += z;
            cu += f32::from_le_bytes([vertices[base + 16], vertices[base + 17], vertices[base + 18], vertices[base + 19]]);
            cv += f32::from_le_bytes([vertices[base + 20], vertices[base + 21], vertices[base + 22], vertices[base + 23]]);
        }
        cx *= 0.25f32; cy *= 0.25f32; cz *= 0.25f32; cu *= 0.25f32; cv *= 0.25f32;

        // ---- albedo 累加（生产控制流：front solid→采 front；front air→back 摆 in-bounds+solid→采 back；否则不采）
        if p.do_albedo != 0 {
            if let Some((nx, ny, nz)) = cross_normalize(vx[0], vy[0], vz[0], vx[1], vy[1], vz[1], vx[2], vy[2], vz[2]) {
                let _ = (nx, ny, nz); // 归一化法线仅用于探针坐标（已在 plan 期算过），此处无需再算
                let (k0, sx, sy, sz) = get_slot(quad, 0);
                if k0 == VPT_SLOT_ALBEDO_FRONT {
                    let b0 = bits[quad * VPT_SLOTS_PER_QUAD];
                    if b0 != VPT_BIT_UNPROBED {
                        let mut sample: Option<(i16, i16, i16)> = None;
                        if (b0 & 1) == 0 {
                            sample = Some((sx, sy, sz)); // front solid → 采 front
                        } else {
                            let (k1, bx, by, bz) = get_slot(quad, 1);
                            if k1 == VPT_SLOT_ALBEDO_BACK {
                                let b1 = bits[quad * VPT_SLOTS_PER_QUAD + 1];
                                if b1 != VPT_BIT_UNPROBED && (b1 & 1) == 0 {
                                    sample = Some((bx, by, bz)); // front air + back solid → 采 back
                                }
                            }
                        }
                        if let Some((lx, ly, lz)) = sample {
                            let ax = ((((cu * atlas_w as f32) as f64).floor() as i32).max(0)).min(atlas_w - 1);
                            let ay = ((((cv * atlas_h as f32) as f64).floor() as i32).max(0)).min(atlas_h - 1);
                            let index = (lx as usize) + 16 * ((lz as usize) + 16 * (ly as usize));
                            if index >= red.len() {
                                // 一次性诊断：打印该 quad 的全部输入，定位坐标失控来源后移除
                                let (k0, s0x, s0y, s0z) = get_slot(quad, 0);
                                let (k1, s1x, s1y, s1z) = get_slot(quad, 1);
                                let b0 = bits[quad * VPT_SLOTS_PER_QUAD];
                                let b1 = bits[quad * VPT_SLOTS_PER_QUAD + 1];
                                eprintln!(
                                    "[VPT-DIAG] quad={}/{} idx={} lx,ly,lz={},{},{} slot0=({},{},{},{}) slot1=({},{},{},{}) b0={:#04x} b1={:#04x} verts={:?}{:?}{:?}{:?}",
                                    quad, quads, index, lx, ly, lz,
                                    k0, s0x, s0y, s0z, k1, s1x, s1y, s1z, b0, b1,
                                    [vx[0], vy[0], vz[0]], [vx[1], vy[1], vz[1]],
                                    [vx[2], vy[2], vz[2]], [vx[3], vy[3], vz[3]]);
                                return ERR_CAP; // 停止处理该 section，回落 Java 路径
                            }
                            let pixel = p.atlas_pixels as usize
                                + (((ay as usize) * atlas_w as usize + ax as usize) * 4usize);
                            let bp = pixel as *const u8;
                            let (br, bg, bb) = unsafe {
                                ((*bp & 0xFF) as i32,
                                 (*bp.add(1) & 0xFF) as i32,
                                 (*bp.add(2) & 0xFF) as i32)
                            };
                            red[index] = red[index].wrapping_add(br);
                            green[index] = green[index].wrapping_add(bg);
                            blue[index] = blue[index].wrapping_add(bb);
                            count[index] = count[index].wrapping_add(1);
                            any = true;
                        }
                    }
                }
            }
        }

        // ---- cutout 植被标记（byte 27 = 25）
        if p.cutout_layer != 0 {
            if cross_normalize(vx[0], vy[0], vz[0], vx[1], vy[1], vz[1], vx[2], vy[2], vz[2]).is_some() {
                let (k2, _, _, _) = get_slot(quad, 2);
                if k2 == VPT_SLOT_FOLIAGE_A {
                    let b2 = bits[quad * VPT_SLOTS_PER_QUAD + 2];
                    let mut foliage = false;
                    if b2 != VPT_BIT_UNPROBED && (b2 & 2) != 0 {
                        foliage = true;
                    } else {
                        let (k3, _, _, _) = get_slot(quad, 3);
                        if k3 == VPT_SLOT_FOLIAGE_B {
                            let b3 = bits[quad * VPT_SLOTS_PER_QUAD + 3];
                            if b3 != VPT_BIT_UNPROBED && (b3 & 2) != 0 {
                                foliage = true;
                            }
                        }
                    }
                    if foliage {
                        for v in 0..4usize {
                            vertices[(quad * 4 + v) * 28 + 27] = 25;
                        }
                    }
                }
            }
        }

        // ---- collapse 雨洼标记（byte 27 = 23，后写可覆盖植被 25，与生产顺序一致）
        let span_x = quad_span(vx[0], vx[1], vx[2], vx[3]);
        let span_y = quad_span(vy[0], vy[1], vy[2], vy[3]);
        let span_z = quad_span(vz[0], vz[1], vz[2], vz[3]);
        let flat_count = ((span_x <= 1.0e-4f32) as i32)
            + ((span_y <= 1.0e-4f32) as i32)
            + ((span_z <= 1.0e-4f32) as i32);
        if flat_count == 1 {
            let (k4, _, _, _) = get_slot(quad, 4);
            if k4 == VPT_SLOT_COLLAPSE_A {
                let b4 = bits[quad * VPT_SLOTS_PER_QUAD + 4];
                let mut resolved = false;
                if b4 != VPT_BIT_UNPROBED && (b4 & 4) != 0 {
                    resolved = true;
                } else {
                    let (k5, _, _, _) = get_slot(quad, 5);
                    if k5 == VPT_SLOT_COLLAPSE_B {
                        let b5 = bits[quad * VPT_SLOTS_PER_QUAD + 5];
                        if b5 != VPT_BIT_UNPROBED && (b5 & 4) != 0 {
                            resolved = true;
                        }
                    }
                }
                if resolved {
                    for v in 0..4usize {
                        vertices[(quad * 4 + v) * 28 + 27] = 23;
                    }
                }
            }
        }
    }
    if any { 1 } else { 0 }
}

/// # Safety
/// `params` 指向完整 `VptParams`；`src` 可写且可读 `src_len` 字节（就地打标）；
/// `plan`/`bits` 为 plan 导出与 Java 探针填好的配套缓冲；
/// `red/green/blue/count` 各 4096 个 i32（Java 堆数组经 ofArray 传入）。
#[no_mangle]
pub unsafe extern "C" fn off_vpt_build(
    params: *const VptParams,
    src: *mut u8, src_len: u32,
    plan: *const u8, plan_cap_bytes: u32,
    bits: *const u8, bits_cap_bytes: u32,
    red: *mut i32, green: *mut i32, blue: *mut i32, count: *mut i32, acc_len: u32,
) -> u32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let p = &*params;
        if p.stride != 28 || p.vertex_count <= 0 || p.vertex_count % 4 != 0 {
            return ERR_CAP;
        }
        // 与 inner 同一 /112 钳制（MC 26.x vertexBuffer 容量按 28B/quad 记，vertexCount 是 4× 展开数）
        let quads = ((p.vertex_count / 4) as usize)
            .min((src_len as usize) / 112)
            .min((plan_cap_bytes as usize) / (VPT_SLOTS_PER_QUAD * VPT_SLOT_BYTES));
        if (bits_cap_bytes as usize) < quads * VPT_SLOTS_PER_QUAD
            || ((acc_len as usize) < 4096) {
            return ERR_CAP;
        }
        let vertices = std::slice::from_raw_parts_mut(src, src_len as usize);
        let plan = std::slice::from_raw_parts(plan, plan_cap_bytes as usize);
        let bits = std::slice::from_raw_parts(bits, bits_cap_bytes as usize);
        let red = std::slice::from_raw_parts_mut(red, acc_len as usize);
        let green = std::slice::from_raw_parts_mut(green, acc_len as usize);
        let blue = std::slice::from_raw_parts_mut(blue, acc_len as usize);
        let count = std::slice::from_raw_parts_mut(count, acc_len as usize);
        vpt_build_inner(p, vertices, plan, bits, red, green, blue, count)
    }));
    result.unwrap_or(ERR_PANIC)
}
/// # Safety
/// `params` 指向完整 `WeatherParams`；`heights`/`precip`/`flake_h` 为 plan 对应的探针数组
/// （n=max_rain_drops：heights n×i32、precip n×u8、flake_h 2n×i32）；dst 两段可写且互不重叠。
#[no_mangle]
pub unsafe extern "C" fn off_weather_build(
    params: *const WeatherParams,
    heights: *const i32,
    precip: *const u8,
    flake_h: *const i32,
    dst_v: *mut f32,
    dst_v_cap_words: u32,
    dst_m: *mut f32,
    dst_m_cap_words: u32,
) -> u32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let p = &*params;
        let n = p.max_rain_drops as usize;
        let heights = std::slice::from_raw_parts(heights, n);
        let precip = std::slice::from_raw_parts(precip, n);
        let flake_h = std::slice::from_raw_parts(flake_h, n * 2);
        let mut dst_v = std::slice::from_raw_parts_mut(dst_v, dst_v_cap_words as usize);
        let mut dst_m = std::slice::from_raw_parts_mut(dst_m, dst_m_cap_words as usize);
        weather_build_inner(p, heights, precip, flake_h, &mut dst_v, &mut dst_m)
    }));
    result.unwrap_or(ERR_PANIC)
}

fn weather_build_inner(
    p: &WeatherParams,
    heights: &[i32],
    precip: &[u8],
    flake_h: &[i32],
    dst_v: &mut [f32],
    dst_m: &mut [f32],
) -> u32 {
    let grid = p.grid_size;
    let half = grid / 2;
    let anchor_x = p.anchor_x;
    let anchor_z = p.anchor_z;
    let mut quads: Vec<WQuad> = Vec::with_capacity(64);
    let cap = p.max_weather_quads.max(0) as usize;

    for cell in 0..p.max_rain_drops.max(0) as usize {
        let grid_x = (cell % grid as usize) as i32;
        let grid_z = (cell / grid as usize) as i32;
        let world_cell_x = anchor_x + grid_x - half;
        let world_cell_z = anchor_z + grid_z - half;
        let seed = hash32(world_cell_x.wrapping_mul(H1) ^ world_cell_z.wrapping_mul(H2));
        let cell_unit = unit32(hash32(seed ^ 0x165667b1));
        if cell_unit >= p.any_fill {
            continue;
        }
        let jitter_x = unit32(seed) - 0.5f32;
        let jitter_z = unit32(seed.wrapping_mul(1664525).wrapping_add(1013904223)) - 0.5f32;
        let world_x = (world_cell_x as f32 + jitter_x * 0.62f32) * p.cell_spacing;
        let world_z = (world_cell_z as f32 + jitter_z * 0.62f32) * p.cell_spacing;
        let x = world_x - p.player_x;
        let z = world_z - p.player_z;
        let phase = unit32(seed.wrapping_mul(1103515245).wrapping_add(12345));
        match precip[cell] {
            0 => continue, // NONE
            2 => {
                // SNOW
                if cell_unit >= p.snow_fill {
                    continue;
                }
                for f in 0..p.rt_snow_flakes_per_cell.max(0) as usize {
                    let flake_seed = hash32(seed ^ H3.wrapping_mul(f as i32 + 1));
                    let flake_phase = fract_java(phase + unit32(flake_seed ^ 0x68bc21eb));
                    let fall_speed = (p.snow_fall_min
                        + (p.snow_fall_max - p.snow_fall_min) * unit32(flake_seed ^ 0x7feb352d))
                        * p.snow_fall_speed_scale;
                    let y = -12.0f32 + fract_java(flake_phase - p.seconds * fall_speed) * 28.0f32;
                    let sway_phase = unit32(flake_seed ^ 0x51ed270b) * (std::f32::consts::PI * 2.0f32);
                    let sway_angle = p.seconds * p.snow_sway_frequency * (std::f32::consts::PI * 2.0f32)
                        + sway_phase;
                    let sway_x = j_sin(sway_angle) * p.snow_sway_blocks;
                    let sway_z = j_cos(sway_angle * 1.618f32) * p.snow_sway_blocks;
                    let flake_x = x + sway_x;
                    let flake_z = z + sway_z;
                    let column_top = flake_h[cell * 2 + f];
                    if p.eye_y + y < column_top as f32 {
                        continue;
                    }
                    let near_fade = smoothstep_java(
                        p.near_fade_inner,
                        p.near_fade_outer,
                        (flake_x * flake_x + y * y + flake_z * flake_z).sqrt(),
                    );
                    if near_fade <= 0.02f32 {
                        continue;
                    }
                    let size = (p.snow_flake_min
                        + (p.snow_flake_max - p.snow_flake_min) * unit32(flake_seed ^ H4))
                        * p.snow_size_scale;
                    let variant = hash32(flake_seed ^ H5) & 15;
                    let u0 = (variant & 3) as f32 * 0.25f32;
                    let v0 = p.snow_tile_v_base + ((variant >> 2) as f32) * 0.125f32;
                    let u1 = u0 + 0.25f32;
                    let v1 = v0 + 0.125f32;
                    // weatherFlakeRotation.set(cameraRotation).rotateZ(spin)
                    let spin = unit32(flake_seed ^ 0x27d4eb2f) * (std::f32::consts::PI * 2.0f32);
                    let s = j_sin(spin * 0.5f32);
                    let c = j_cos(spin * 0.5f32);
                    let (rx, ry, rz, rw) = (
                        p.cam_x * c + p.cam_y * s,
                        p.cam_y * c - p.cam_x * s,
                        p.cam_z * c + p.cam_w * s,
                        p.cam_w * c - p.cam_z * s,
                    );
                    append_quad(
                        &mut quads,
                        cap,
                        WQuad {
                            x: flake_x, y, z: flake_z, scale: size * near_fade,
                            u0, u1, v0, v1,
                            xr: rx, yr: ry, zr: rz, wr: rw,
                            color: p.snow_color, marker: p.rt_snow_marker,
                            wind_x: 0.0f32, wind_z: 0.0f32,
                        },
                    );
                }
            }
            _ => {
                // RAIN
                if cell_unit >= p.rain_fill {
                    continue;
                }
                let y = -12.0f32 + fract_java(phase - p.seconds * 0.65f32) * 28.0f32;
                let column_top = heights[cell];
                if p.eye_y + y < column_top as f32 {
                    continue;
                }
                let near_fade = smoothstep_java(
                    p.near_fade_inner,
                    p.near_fade_outer,
                    (x * x + y * y + z * z).sqrt(),
                );
                if near_fade <= 0.02f32 {
                    continue;
                }
                let streak_scale = p.rain_scale * near_fade;
                let variant = hash32(seed ^ 0x51ed270b) & 15;
                let u0 = (variant & 3) as f32 * 0.25f32;
                let v0 = ((variant >> 2) as f32) * 0.125f32;
                let u1 = u0 + 0.25f32;
                let v1 = v0 + 0.125f32;
                for (yr, wr) in [(0.0f32, 1.0f32), (0.70710677f32, 0.70710677f32)] {
                    append_quad(
                        &mut quads,
                        cap,
                        WQuad {
                            x, y, z, scale: streak_scale,
                            u0, u1, v0, v1,
                            xr: 0.0f32, yr, zr: 0.0f32, wr,
                            color: p.rain_color, marker: p.rt_rain_marker,
                            wind_x: p.wind_x, wind_z: p.wind_z,
                        },
                    );
                }
            }
        }
    }

    // emitQuad/emit 打包（Java endFrame 路径；weather 槽位 dx/dy/dz 恒 0）
    let vertex_words = 12usize;
    let need_v = quads.len() * 6 * vertex_words;
    if dst_v.len() < need_v || dst_m.len() < quads.len() * 6 * 3 {
        return ERR_CAP;
    }
    let mut vi = 0usize;
    let mut mi = 0usize;
    for q in &quads {
        let width = if q.marker == p.rt_rain_marker {
            p.rain_half_width / q.scale.max(1.0e-4f32)
        } else {
            1.0f32
        };
        let a = q_vertex(q.xr, q.yr, q.zr, q.wr, width, -1.0f32, q.scale, q.x, q.y, q.z);
        let b = q_vertex(q.xr, q.yr, q.zr, q.wr, width, 1.0f32, q.scale, q.x, q.y, q.z);
        let c = q_vertex(q.xr, q.yr, q.zr, q.wr, -width, 1.0f32, q.scale, q.x, q.y, q.z);
        let d = q_vertex(q.xr, q.yr, q.zr, q.wr, -width, -1.0f32, q.scale, q.x, q.y, q.z);
        let wind = |mut pt: (f32, f32, f32), ny: f32| -> (f32, f32, f32) {
            if q.wind_x != 0.0f32 || q.wind_z != 0.0f32 {
                let lift = ny * q.scale;
                pt.0 += q.wind_x * lift;
                pt.2 += q.wind_z * lift;
            }
            pt
        };
        let a = wind(a, -1.0f32);
        let b = wind(b, 1.0f32);
        let c = wind(c, 1.0f32);
        let d = wind(d, -1.0f32);
        let r = ((q.color >> 16) & 0xFF) as i32 as f32 / 255.0f32;
        let g = ((q.color >> 8) & 0xFF) as i32 as f32 / 255.0f32;
        let bl = (q.color & 0xFF) as i32 as f32 / 255.0f32;
        let al = ((q.color as u32) >> 24) as f32 / 255.0f32;
        let marker_f = q.marker as f32;
        let slot_f = -(1.0f32); // appendWeatherQuad 的 atlasSlot 恒 1
        for (pt, u, v) in [
            (a, q.u1, q.v1),
            (b, q.u1, q.v0),
            (c, q.u0, q.v0),
            (a, q.u1, q.v1),
            (c, q.u0, q.v0),
            (d, q.u0, q.v1),
        ] {
            dst_v[vi] = pt.0;
            dst_v[vi + 1] = pt.1;
            dst_v[vi + 2] = pt.2;
            dst_v[vi + 3] = u;
            dst_v[vi + 4] = v;
            dst_v[vi + 5] = r;
            dst_v[vi + 6] = g;
            dst_v[vi + 7] = bl;
            dst_v[vi + 8] = al;
            dst_v[vi + 9] = 0.0f32;
            dst_v[vi + 10] = marker_f;
            dst_v[vi + 11] = slot_f;
            dst_m[mi] = 0.0f32;
            dst_m[mi + 1] = 0.0f32;
            dst_m[mi + 2] = 0.0f32;
            vi += vertex_words;
            mi += 3;
        }
    }
    quads.len() as u32
}

// ===================================================================
// M3 批量转换内核（2026-11-19）：N 节一次 JNI，替代逐节调用
// ===================================================================

/// 批量转换（M3，规格 docs/orion-m3-dispatch-spec.md §3）：N 个 section 一次调用。
///
/// # 契约（冻结，2026-11-19）
/// - `src`：N 节的原始 DH 字节流顺序拼接；`lens[i]` = 第 i 节字节数（非 64 倍数尾部忽略，同单节语义）；
/// - `widths[i]` = 第 i 节 blockWidth；`dst` 容量 ≥ Σsrc_len（最坏全有效，写入顺序同输入序）；
/// - `out_lens[i]` = 第 i 节写入字节数（调用方按它切片 dst）；
/// - 返回已转换节数；任一节 ERR_CAP（容量不足）即整批 ERR_CAP（调用方回落 Java 逐节路径）。
///
/// # Safety
/// `src` 可读 `src_total_len` 字节；`dst` 可写 `dst_cap` 字节；`lens`/`widths`/`out_lens`
/// 各可读/写 `count` 个 u32；输入与输出内存不得重叠。
#[no_mangle]
pub unsafe extern "C" fn off_convert_sections(
    src: *const u8,
    src_total_len: u32,
    lens: *const u32,
    widths: *const u32,
    count: u32,
    dst: *mut u8,
    dst_cap: u32,
    texture_enabled: u32,
    out_lens: *mut u32,
) -> u32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if count == 0 {
            return 0u32;
        }
        let lens = std::slice::from_raw_parts(lens, count as usize);
        let widths = std::slice::from_raw_parts(widths, count as usize);
        let out_lens = std::slice::from_raw_parts_mut(out_lens, count as usize);
        let dst = std::slice::from_raw_parts_mut(dst, dst_cap as usize);
        let total = src_total_len as usize;
        let src = if total == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(src, total)
        };

        let mut src_at = 0usize;
        let mut dst_at = 0usize;
        for i in 0..count as usize {
            let len = lens[i] as usize;
            if src_at + len > total || dst_at + len > dst.len() {
                return ERR_CAP;
            }
            let written = convert_inner(
                &src[src_at..src_at + len],
                &mut dst[dst_at..dst_at + len],
                widths[i],
                texture_enabled != 0,
            ) as usize;
            out_lens[i] = written as u32;
            src_at += len;
            dst_at += written; // 输出按实际写入紧凑排列（无效 quad 被剔除，各节输出长度可变）
        }
        count
    }));
    result.unwrap_or(ERR_PANIC)
}
