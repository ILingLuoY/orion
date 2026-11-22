package com.example.vulkan;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/**
 * orion_farfield.dll（Rust cdylib）的 FFM 桥（Java 25, java.lang.foreign）。
 *
 * <h2>加载顺序</h2>
 * <ol>
 *   <li>系统属性 {@code orion.rustDll}＝绝对路径（bench/dev 用）；</li>
 *   <li>打包资源 {@code /natives/windows/orion_farfield.dll}（提取到临时目录后加载，
 *       与 NativeVulkan 的 rtx_backend 同型）。</li>
 * </ol>
 *
 * <h2>版本门</h2>
 * {@code off_abi_version} 必须 == {@link #EXPECTED_ABI}，否则视为不可用并打日志。
 *
 * <h2>失败安全</h2>
 * 所有方法失败（加载失败/句柄异常/错误码）返回 -1 或 {@code available()==false}，
 * 调用方回落 Java 路径；本类绝不抛出到渲染循环。
 *
 * <p>JVM 需 {@code --enable-native-access=ALL-UNNAMED}（JDK 25 默认 WARN，不拦）。</p>
 */
public final class RustBridge {
    public static final int EXPECTED_ABI = 3;
    private static final String RESOURCE_PATH = "/natives/windows/orion_farfield.dll";
    private static final int WEATHER_PLAN_CELL_WORDS = 7;

    private static volatile boolean initTried;
    private static volatile boolean available;
    private static SymbolLookup lib;
    private static MethodHandle hWeatherPlan;
    private static MethodHandle hWeatherBuild;
    private static MethodHandle hConvert;
    private static MethodHandle hConvBatch;
    private static MethodHandle hVptPlan;
    private static MethodHandle hVptBuild;

    /** VPT 累加器的本地中转（FFM 下行不收堆段；ThreadLocal ⇒ 多线程 builder 安全）。 */
    private static final ThreadLocal<ByteBuffer[]> VPT_ACC = ThreadLocal.withInitial(() -> {
        final ByteBuffer[] acc = new ByteBuffer[4];
        for (int i = 0; i < 4; i++) {
            acc[i] = ByteBuffer.allocateDirect(4096 * 4).order(ByteOrder.nativeOrder());
        }
        return acc;
    });

    private RustBridge() {
    }

    private static synchronized void init() {
        if (initTried) {
            return;
        }
        initTried = true;
        try {
            final Path dll = locateDll();
            lib = SymbolLookup.libraryLookup(dll, Arena.global());
            final Linker linker = Linker.nativeLinker();
            final MethodHandle ver = linker.downcallHandle(
                    lib.find("off_abi_version").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT));
            final int abi = (int) ver.invokeExact();
            if (abi != EXPECTED_ABI) {
                throw new IllegalStateException("off_abi_version " + abi + " != " + EXPECTED_ABI);
            }
            hWeatherPlan = linker.downcallHandle(
                    lib.find("off_weather_plan").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            hWeatherBuild = linker.downcallHandle(
                    lib.find("off_weather_build").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            hConvert = linker.downcallHandle(
                    lib.find("off_convert_dh_section").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
            hConvBatch = linker.downcallHandle(
                    lib.find("off_convert_sections").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS));
            hVptPlan = linker.downcallHandle(
                    lib.find("off_vpt_plan").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            hVptBuild = linker.downcallHandle(
                    lib.find("off_vpt_build").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_INT));
            available = true;
            System.out.println("[RUST-BRIDGE] orion_farfield loaded, abi=" + abi + " (" + dll
                    + ", " + Files.size(dll) + " bytes)");
        } catch (Throwable failure) {
            available = false;
            lib = null;
            System.out.println("[RUST-BRIDGE] unavailable: " + failure);
        }
    }

    private static Path locateDll() throws Exception {
        final String override = System.getProperty("orion.rustDll");
        if (override != null && !override.isBlank()) {
            return Path.of(override);
        }
        final Path target = Path.of(System.getProperty("java.io.tmpdir"),
                "orion_farfield_v" + EXPECTED_ABI + ".dll");
        try (InputStream in = RustBridge.class.getResourceAsStream(RESOURCE_PATH)) {
            if (in == null) {
                throw new IllegalStateException("resource missing: " + RESOURCE_PATH);
            }
            final long resBytes = in.transferTo(Files.newOutputStream(target,
                    java.nio.file.StandardOpenOption.CREATE,
                    java.nio.file.StandardOpenOption.TRUNCATE_EXISTING,
                    java.nio.file.StandardOpenOption.WRITE));
            System.out.println("[RUST-BRIDGE] extracted " + resBytes + " bytes -> " + target
                    + " (resource " + RESOURCE_PATH + ")");
        }
        return target;
    }

    /** 是否可用（首次调用完成加载与版本门）。 */
    public static boolean available() {
        init();
        return available;
    }

    /**
     * 天气 plan：每格 {@value #WEATHER_PLAN_CELL_WORDS}×i32 探针坐标写进 {@code plan}
     * （从 position 0 写；不改 Java 侧 position）。
     *
     * @return 计划格数；-1 = 失败
     */
    public static int weatherPlan(ByteBuffer params, ByteBuffer plan) {
        if (!available()) {
            return -1;
        }
        try {
            return (int) hWeatherPlan.invokeExact(
                    MemorySegment.ofBuffer(params),
                    MemorySegment.ofBuffer(plan),
                    plan.remaining() / (WEATHER_PLAN_CELL_WORDS * 4));
        } catch (Throwable failure) {
            return -1;
        }
    }

    /**
     * 天气构建：Rust 往两个直接缓冲的 segment 视图里写顶点/运动（不改 Java position）。
     *
     * @return quad 数；-1 = 失败（含容量不足/panic 错误码）
     */
    public static int weatherBuild(ByteBuffer params, ByteBuffer heights, ByteBuffer precip,
                                   ByteBuffer flakeH, ByteBuffer dstV, ByteBuffer dstM) {
        if (!available()) {
            return -1;
        }
        try {
            final int quads = (int) hWeatherBuild.invokeExact(
                    MemorySegment.ofBuffer(params),
                    MemorySegment.ofBuffer(heights),
                    MemorySegment.ofBuffer(precip),
                    MemorySegment.ofBuffer(flakeH),
                    MemorySegment.ofBuffer(dstV), dstV.remaining() / 4,
                    MemorySegment.ofBuffer(dstM), dstM.remaining() / 4);
            return (quads == 0xFFFFFFFF || quads == 0xFFFFFFFE) ? -1 : quads;
        } catch (Throwable failure) {
            return -1;
        }
    }

    /** DH 远场逐节转换（M3 预留，句柄就绪，生产接线挂起）。 */
    public static int convertDhSection(ByteBuffer src, ByteBuffer dst,
                                       int blockWidth, boolean textureEnabled) {
        if (!available()) {
            return -1;
        }
        try {
            final int written = (int) hConvert.invokeExact(
                    MemorySegment.ofBuffer(src), src.remaining(),
                    MemorySegment.ofBuffer(dst), dst.remaining(),
                    blockWidth, textureEnabled ? 1 : 0);
            return (written == 0xFFFFFFFF || written == 0xFFFFFFFE) ? -1 : written;
        } catch (Throwable failure) {
            return -1;
        }
    }

    private static final ThreadLocal<ByteBuffer> CONV_SRC = ThreadLocal.withInitial(
            () -> ByteBuffer.allocateDirect(4 * 1024 * 1024).order(ByteOrder.nativeOrder()));
    private static final ThreadLocal<ByteBuffer> CONV_LENS = ThreadLocal.withInitial(
            () -> ByteBuffer.allocateDirect(4096 * 4).order(ByteOrder.nativeOrder()));
    private static final ThreadLocal<ByteBuffer> CONV_WIDTHS = ThreadLocal.withInitial(
            () -> ByteBuffer.allocateDirect(4096 * 4).order(ByteOrder.nativeOrder()));
    private static final ThreadLocal<ByteBuffer> CONV_DST = ThreadLocal.withInitial(
            () -> ByteBuffer.allocateDirect(4 * 1024 * 1024).order(ByteOrder.nativeOrder()));
    private static final ThreadLocal<ByteBuffer> CONV_OUTLENS = ThreadLocal.withInitial(
            () -> ByteBuffer.allocateDirect(4096 * 4).order(ByteOrder.nativeOrder()));

    /**
     * M3 批量转换：N 节一次 JNI（替代逐节调用）；输入节原始字节按序拼接，输出按节切片。
     * 任一失败返回 null（调用方回落逐节 Java convert）。
     */
    public static byte[][] convertSectionsBatch(byte[][] srcs, int[] widths, boolean textureEnabled) {
        if (!available() || srcs.length == 0 || srcs.length > 4096) {
            return null;
        }
        try (Arena arena = Arena.ofConfined()) {
            int total = 0;
            for (byte[] s : srcs) total += s.length;
            final MemorySegment srcSeg = arena.allocate(total);
            int at = 0;
            for (byte[] s : srcs) {
                MemorySegment.copy(s, 0, srcSeg, ValueLayout.JAVA_BYTE, at, s.length);
                at += s.length;
            }
            final MemorySegment lensSeg = arena.allocate(srcs.length * 4L);
            final MemorySegment widthsSeg = arena.allocate(srcs.length * 4L);
            final MemorySegment outLensSeg = arena.allocate(srcs.length * 4L);
            for (int i = 0; i < srcs.length; i++) {
                lensSeg.set(ValueLayout.JAVA_INT, i * 4L, srcs[i].length);
                widthsSeg.set(ValueLayout.JAVA_INT, i * 4L, widths[i]);
            }
            final MemorySegment dstSeg = arena.allocate(total);
            final int n = (int) hConvBatch.invokeExact(
                    srcSeg, total,
                    lensSeg, widthsSeg, srcs.length,
                    dstSeg, total,
                    textureEnabled ? 1 : 0,
                    outLensSeg);
            if (n != srcs.length) {
                return null;
            }
            final byte[][] out = new byte[srcs.length][];
            int off = 0;
            for (int i = 0; i < srcs.length; i++) {
                final int len = outLensSeg.get(ValueLayout.JAVA_INT, i * 4L);
                if (len < 0 || off + len > total) {
                    return null;
                }
                final byte[] piece = new byte[len];
                for (int k = 0; k < len; k++) {
                    piece[k] = dstSeg.get(ValueLayout.JAVA_BYTE, off + k);
                }
                out[i] = piece;
                off += len;
            }
            return out;
        } catch (Throwable failure) {
            return null;
        }
    }

    /**
     * VPT plan：src 视图取【整个缓冲】（duplicate().clear()）——生产 Java 用绝对索引读顶点，
     * segment 必须从 index 0 起才能对齐。
     *
     * @return 已写槽位数（quads×6）；-1 = 失败
     */
    public static int vptPlan(ByteBuffer params, ByteBuffer vertices, ByteBuffer plan) {
        if (!available()) {
            return -1;
        }
        try {
            final ByteBuffer v = vertices.duplicate().clear();
            return (int) hVptPlan.invokeExact(
                    MemorySegment.ofBuffer(params),
                    MemorySegment.ofBuffer(v), v.remaining(),
                    MemorySegment.ofBuffer(plan), plan.remaining());
        } catch (Throwable failure) {
            return -1;
        }
    }

    /**
     * VPT build：顶点缓冲就地打标（同上全缓冲视图）；累加器是 Java 堆数组，FFM 下行不收堆段
     * ⇒ 经 ThreadLocal 本地缓冲拷入拷出（每节 2×64KB memcpy，可忽略；同时避免多线程 builder 下静态缓冲竞争）。
     *
     * @return vptAny 标志（0/1）；-1 = 失败
     */
    public static int vptBuild(ByteBuffer params, ByteBuffer vertices, ByteBuffer plan, ByteBuffer bits,
                               int[] red, int[] green, int[] blue, int[] count) {
        if (!available()) {
            return -1;
        }
        try {
            final ByteBuffer v = vertices.duplicate().clear();
            final ByteBuffer[] acc = VPT_ACC.get();
            final int[][] src = {red, green, blue, count};
            for (int i = 0; i < 4; i++) {
                acc[i].clear();
                acc[i].asIntBuffer().put(src[i]);
            }
            final int ret = (int) hVptBuild.invokeExact(
                    MemorySegment.ofBuffer(params),
                    MemorySegment.ofBuffer(v), v.remaining(),
                    MemorySegment.ofBuffer(plan), plan.remaining(),
                    MemorySegment.ofBuffer(bits), bits.remaining(),
                    MemorySegment.ofBuffer(acc[0]),
                    MemorySegment.ofBuffer(acc[1]),
                    MemorySegment.ofBuffer(acc[2]),
                    MemorySegment.ofBuffer(acc[3]),
                    4096);
            if (ret == 0xFFFFFFFF || ret == 0xFFFFFFFE) {
                return -1;
            }
            for (int i = 0; i < 4; i++) {
                acc[i].asIntBuffer().get(src[i]);
            }
            return ret;
        } catch (Throwable failure) {
            return -1;
        }
    }
}
