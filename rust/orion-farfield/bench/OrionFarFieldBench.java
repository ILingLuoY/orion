import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.Random;

import org.joml.Quaternionf;
import org.joml.Vector3f;

/**
 * Orion Rust pilot harness: M0 FFI check + DH parity (M2) + weather plan-check/parity/A-B.
 *
 * Weather twin mirrors ParticleRtCapture.buildRtWeather/appendWeatherSnow/emitQuad/emit
 * (2026-11-17 source) with real JOML for rotation.
 *
 * Run (from rust/orion-farfield):
 *   D:\RTX\bin\java -Xmx2g --enable-native-access=ALL-UNNAMED -cp <joml-1.10.9.jar>; bench\OrionFarFieldBench.java dist\orion_farfield.dll
 *
 * Exit codes: 0 ok, 1 ABI mismatch, 2 parity FAIL, 3 rust runtime error.
 */
public final class OrionFarFieldBench {

    // ---- weather constants (mirror ParticleRtCapture) ----
    static final int GRID = 42;                 // WEATHER_GRID_SIZE = round(16*sqrt(7))
    static final int MAX_DROPS = GRID * GRID;   // 1764
    static final int MAX_WEATHER_QUADS = MAX_DROPS * 2;
    static final int RT_RAIN_MARKER = -2;
    static final int RT_SNOW_MARKER = -4;
    static final float NEAR_IN = 0.50F, NEAR_OUT = 1.80F;
    static final float SNOW_V_BASE = 0.5F;
    static final int FLAKES_PER_CELL = 2;
    static final float SNOW_FALL_MIN = 0.085F, SNOW_FALL_MAX = 0.145F;
    static final float SNOW_SWAY_FREQ = 0.22F;
    static final float SNOW_MIN = 0.06F, SNOW_MAX = 0.19F;
    static final int PLAN_WORDS = 7;

    // ================= DH convert twin (M2, bit-exact reference) =================

    static byte[] convertJava(byte[] src, int blockWidth, boolean textureEnabled) {
        final int limit = blockWidth + 1;
        int validQuads = 0;
        for (int q = 0; q + 64 <= src.length; q += 64) {
            boolean valid = true;
            for (int v = 0; v < 4; v++) {
                final int s = q + v * 16;
                if (readU16(src, s) > limit || readU16(src, s + 4) > limit) { valid = false; break; }
            }
            if (valid) validQuads++;
        }
        if (validQuads == 0) return new byte[0];
        final byte[] out = new byte[validQuads * 64];
        int at = 0;
        for (int q = 0; q + 64 <= src.length; q += 64) {
            boolean valid = true;
            for (int v = 0; v < 4; v++) {
                final int s = q + v * 16;
                if (readU16(src, s) > limit || readU16(src, s + 4) > limit) { valid = false; break; }
            }
            if (!valid) continue;
            for (int v = 0; v < 4; v++) {
                final int s = q + v * 16;
                final int d = at + v * 16;
                writeU16(out, d, readU16(src, s));
                writeU16(out, d + 2, readU16(src, s + 2));
                writeU16(out, d + 4, readU16(src, s + 4));
                final int tileId = textureEnabled ? readU16(src, s + 14) : 0;
                writeU16(out, d + 6, tileId);
                writeU16(out, d + 8, src[s + 13] & 0x0F);
                out[d + 10] = src[s + 8];
                out[d + 11] = src[s + 9];
                out[d + 12] = src[s + 10];
                out[d + 13] = src[s + 6];
                out[d + 14] = src[s + 12];
                out[d + 15] = 0;
            }
            at += 64;
        }
        return out;
    }

    static int readU16(byte[] src, int index) {
        return (src[index] & 0xFF) | ((src[index + 1] & 0xFF) << 8);
    }

    static void writeU16(byte[] dst, int index, int value) {
        dst[index] = (byte) (value & 0xFF);
        dst[index + 1] = (byte) ((value >> 8) & 0xFF);
    }

    static byte[] genDhStream(int quads, int blockWidth, long seed) {
        Random rnd = new Random(seed);
        byte[] src = new byte[quads * 64];
        for (int q = 0; q < quads; q++) {
            for (int v = 0; v < 4; v++) {
                int s = q * 64 + v * 16;
                int x = rnd.nextInt(blockWidth + 4);
                int y = rnd.nextInt(1024);
                int z = rnd.nextInt(blockWidth + 4);
                putU16(src, s, x);
                putU16(src, s + 2, y);
                putU16(src, s + 4, z);
                for (int b = 6; b <= 13; b++) src[s + b] = (byte) rnd.nextInt(256);
                putU16(src, s + 14, rnd.nextInt(65536));
            }
        }
        return src;
    }

    static void putU16(byte[] b, int i, int v) {
        b[i] = (byte) v;
        b[i + 1] = (byte) (v >> 8);
    }

    // ================= weather twin =================

    /** typed params — no index games */
    static final class WP {
        int anchorX, anchorZ;
        float playerX, playerZ, eyeY, seconds, cellSpacing;
        float anyFill, rainFill, snowFill;
        float rainScale, rainHalfWidth, windX, windZ;
        int rainColor, snowColor;
        float camX, camY, camZ, camW;
        float fallScale, swayBlocks, sizeScale;
    }

    static int hashW(int value) {
        value ^= value >>> 16;
        value *= 0x7feb352d;
        value ^= value >>> 15;
        value *= 0x846ca68b;
        return value ^ (value >>> 16);
    }

    static float unitW(int value) {
        return (value & 0x00FFFFFF) * (1.0F / 16777216.0F);
    }

    static float fractW(float value) {
        return value - (float) Math.floor(value);
    }

    static float smoothW(float edge0, float edge1, float value) {
        float t = Math.max(0.0F, Math.min(1.0F, (value - edge0) / (edge1 - edge0)));
        return t * t * (3.0F - 2.0F * t);
    }

    static int argbFromFloat(float a, float r, float g, float b) {
        return ((int) (a * 255.0F) << 24) | ((int) (r * 255.0F) << 16)
                | ((int) (g * 255.0F) << 8) | (int) (b * 255.0F);
    }

    // quad record: x,y,z,scale,u0,u1,v0,v1,xr,yr,zr,wr,windX,windZ + color + marker
    static final int QF = 14;
    static float[] quadF = new float[MAX_WEATHER_QUADS * QF];
    static int[] quadColor = new int[MAX_WEATHER_QUADS];
    static int[] quadMarker = new int[MAX_WEATHER_QUADS];
    static int twinQuadCount;

    static final Quaternionf twinRot = new Quaternionf();
    static final Vector3f twinA = new Vector3f();
    static final Vector3f twinB = new Vector3f();
    static final Vector3f twinC = new Vector3f();
    static final Vector3f twinD = new Vector3f();

    static void twinAppend(float x, float y, float z, float scale,
                           float u0, float u1, float v0, float v1,
                           float xr, float yr, float zr, float wr,
                           int color, int marker, float windX, float windZ) {
        if (twinQuadCount >= MAX_WEATHER_QUADS) return;
        final int o = twinQuadCount * QF;
        quadF[o] = x; quadF[o + 1] = y; quadF[o + 2] = z; quadF[o + 3] = scale;
        quadF[o + 4] = u0; quadF[o + 5] = u1; quadF[o + 6] = v0; quadF[o + 7] = v1;
        quadF[o + 8] = xr; quadF[o + 9] = yr; quadF[o + 10] = zr; quadF[o + 11] = wr;
        quadF[o + 12] = windX; quadF[o + 13] = windZ;
        quadColor[twinQuadCount] = color;
        quadMarker[twinQuadCount] = marker;
        twinQuadCount++;
    }

    /** Production vertex(): set(nx,ny,0).rotate(rot).mul(scale).add(x,y,z) — real JOML. */
    static Vector3f twinVertex(Vector3f out, Quaternionf rot, int qi, float nx, float ny) {
        final int o = qi * QF;
        return out.set(nx, ny, 0.0F).rotate(rot)
                .mul(quadF[o + 3]).add(quadF[o], quadF[o + 1], quadF[o + 2]);
    }

    static void twinEmitQuad(ByteBuffer out, ByteBuffer mout, int qi, float rainHalfWidth) {
        final int o = qi * QF;
        twinRot.set(quadF[o + 8], quadF[o + 9], quadF[o + 10], quadF[o + 11]);
        final boolean isRain = quadMarker[qi] == RT_RAIN_MARKER;
        final float width = isRain
                ? rainHalfWidth / Math.max(quadF[o + 3], 1.0e-4F)
                : 1.0F;
        twinVertex(twinA, twinRot, qi, width, -1.0F);
        twinVertex(twinB, twinRot, qi, width, 1.0F);
        twinVertex(twinC, twinRot, qi, -width, 1.0F);
        twinVertex(twinD, twinRot, qi, -width, -1.0F);
        if (isRain) {
            windW(twinA, -1.0F, qi);
            windW(twinB, 1.0F, qi);
            windW(twinC, 1.0F, qi);
            windW(twinD, -1.0F, qi);
        }
        final int color = quadColor[qi];
        final float r = ((color >> 16) & 0xFF) / 255.0F;
        final float g = ((color >> 8) & 0xFF) / 255.0F;
        final float b = (color & 0xFF) / 255.0F;
        final float a = ((color >>> 24) & 0xFF) / 255.0F;
        final float markerF = quadMarker[qi];
        final float slotF = -1.0F; // weather atlasSlot = 1
        emitW(out, mout, twinA, quadF[o + 5], quadF[o + 7], r, g, b, a, markerF);
        emitW(out, mout, twinB, quadF[o + 5], quadF[o + 6], r, g, b, a, markerF);
        emitW(out, mout, twinC, quadF[o + 4], quadF[o + 6], r, g, b, a, markerF);
        emitW(out, mout, twinA, quadF[o + 5], quadF[o + 7], r, g, b, a, markerF);
        emitW(out, mout, twinC, quadF[o + 4], quadF[o + 6], r, g, b, a, markerF);
        emitW(out, mout, twinD, quadF[o + 4], quadF[o + 7], r, g, b, a, markerF);
    }

    static void windW(Vector3f p, float ny, int qi) {
        final int o = qi * QF;
        if (quadF[o + 12] == 0.0F && quadF[o + 13] == 0.0F) return;
        final float lift = ny * quadF[o + 3];
        p.x += quadF[o + 12] * lift;
        p.z += quadF[o + 13] * lift;
    }

    static void emitW(ByteBuffer out, ByteBuffer mout, Vector3f p, float u, float v,
                      float r, float g, float b, float a, float markerF) {
        out.putFloat(p.x).putFloat(p.y).putFloat(p.z).putFloat(u).putFloat(v);
        out.putFloat(r).putFloat(g).putFloat(b).putFloat(a);
        out.putFloat(0.0F).putFloat(markerF).putFloat(-1.0F);
        mout.putFloat(0.0F).putFloat(0.0F).putFloat(0.0F);
    }

    /**
     * Full twin of buildRtWeather (production semantics, JOML rotation).
     * Reads synthetic probe arrays; also writes expected plan coordinates.
     *
     * @return quad count
     */
    static int weatherTwin(WP w, int[] heights, int[] precip, int[] flakeH,
                           int[][] planOut, ByteBuffer vbuf, ByteBuffer mbuf) {
        twinQuadCount = 0;
        for (int cell = 0; cell < MAX_DROPS; cell++) {
            final int gridX = cell % GRID;
            final int gridZ = cell / GRID;
            final int wcX = w.anchorX + gridX - GRID / 2;
            final int wcZ = w.anchorZ + gridZ - GRID / 2;
            final int seed = hashW(wcX * 0x9e3779b1 ^ wcZ * 0x85ebca77);
            final float cellUnit = unitW(hashW(seed ^ 0x165667b1));
            final float jitterX = unitW(seed) - 0.5F;
            final float jitterZ = unitW(seed * 1664525 + 1013904223) - 0.5F;
            final float worldX = (wcX + jitterX * 0.62F) * w.cellSpacing;
            final float worldZ = (wcZ + jitterZ * 0.62F) * w.cellSpacing;
            planOut[cell][0] = (int) Math.floor(worldX);
            planOut[cell][1] = (int) Math.floor(worldZ);
            final float phase = unitW(seed * 1103515245 + 12345);
            if (cellUnit < w.snowFill) {
                for (int f = 0; f < FLAKES_PER_CELL; f++) {
                    final int flakeSeed = hashW(seed ^ (0x9E3779B9 * (f + 1)));
                    final float swayPhase = unitW(flakeSeed ^ 0x51ed270b) * (float) (Math.PI * 2.0);
                    final float swayAngle = w.seconds * SNOW_SWAY_FREQ * (float) (Math.PI * 2.0) + swayPhase;
                    final float swayX = (float) Math.sin(swayAngle) * w.swayBlocks;
                    final float swayZ = (float) Math.cos(swayAngle * 1.618F) * w.swayBlocks;
                    planOut[cell][2 + f * 2] = (int) Math.floor(worldX + swayX);
                    planOut[cell][3 + f * 2] = (int) Math.floor(worldZ + swayZ);
                }
            } else {
                planOut[cell][2] = 0; planOut[cell][3] = 0;
                planOut[cell][4] = 0; planOut[cell][5] = 0;
            }
            planOut[cell][6] = cellUnit < w.snowFill ? 1 : 0;
            if (cellUnit >= w.anyFill) continue;
            final float x = worldX - w.playerX;
            final float z = worldZ - w.playerZ;
            final int pc = precip[cell];
            if (pc == 0) continue;
            if (pc == 2) {
                if (cellUnit >= w.snowFill) continue;
                for (int f = 0; f < FLAKES_PER_CELL; f++) {
                    final int flakeSeed = hashW(seed ^ (0x9E3779B9 * (f + 1)));
                    final float flakePhase = fractW(phase + unitW(flakeSeed ^ 0x68bc21eb));
                    final float fallSpeed = (SNOW_FALL_MIN + (SNOW_FALL_MAX - SNOW_FALL_MIN)
                            * unitW(flakeSeed ^ 0x7feb352d)) * w.fallScale;
                    final float y = -12.0F + fractW(flakePhase - w.seconds * fallSpeed) * 28.0F;
                    final float swayPhase = unitW(flakeSeed ^ 0x51ed270b) * (float) (Math.PI * 2.0);
                    final float swayAngle = w.seconds * SNOW_SWAY_FREQ * (float) (Math.PI * 2.0) + swayPhase;
                    final float swayX = (float) Math.sin(swayAngle) * w.swayBlocks;
                    final float swayZ = (float) Math.cos(swayAngle * 1.618F) * w.swayBlocks;
                    final float flakeX = x + swayX;
                    final float flakeZ = z + swayZ;
                    if (w.eyeY + y < flakeH[cell * 2 + f]) continue;
                    final float nearFade = smoothW(NEAR_IN, NEAR_OUT,
                            (float) Math.sqrt(flakeX * flakeX + y * y + flakeZ * flakeZ));
                    if (nearFade <= 0.02F) continue;
                    final float size = (SNOW_MIN + (SNOW_MAX - SNOW_MIN)
                            * unitW(flakeSeed ^ 0x85ebca6b)) * w.sizeScale;
                    final int variant = hashW(flakeSeed ^ 0xc2b2ae35) & 15;
                    final float u0 = (variant & 3) * 0.25F;
                    final float v0 = SNOW_V_BASE + (variant >>> 2) * 0.125F;
                    final Quaternionf fr = new Quaternionf(w.camX, w.camY, w.camZ, w.camW)
                            .rotateZ(unitW(flakeSeed ^ 0x27d4eb2f) * (float) (Math.PI * 2.0));
                    twinAppend(flakeX, y, flakeZ, size * nearFade, u0, u0 + 0.25F, v0, v0 + 0.125F,
                            fr.x, fr.y, fr.z, fr.w, w.snowColor, RT_SNOW_MARKER, 0.0F, 0.0F);
                }
                continue;
            }
            if (cellUnit >= w.rainFill) continue;
            final float y = -12.0F + fractW(phase - w.seconds * 0.65F) * 28.0F;
            if (w.eyeY + y < heights[cell]) continue;
            final float nearFade = smoothW(NEAR_IN, NEAR_OUT,
                    (float) Math.sqrt(x * x + y * y + z * z));
            if (nearFade <= 0.02F) continue;
            final float streakScale = w.rainScale * nearFade;
            final int variant = hashW(seed ^ 0x51ed270b) & 15;
            final float u0 = (variant & 3) * 0.25F;
            final float v0 = (variant >>> 2) * 0.125F;
            twinAppend(x, y, z, streakScale, u0, u0 + 0.25F, v0, v0 + 0.125F,
                    0.0F, 0.0F, 0.0F, 1.0F, w.rainColor, RT_RAIN_MARKER, w.windX, w.windZ);
            twinAppend(x, y, z, streakScale, u0, u0 + 0.25F, v0, v0 + 0.125F,
                    0.0F, 0.70710677F, 0.0F, 0.70710677F, w.rainColor, RT_RAIN_MARKER, w.windX, w.windZ);
        }
        for (int qi = 0; qi < twinQuadCount; qi++) twinEmitQuad(vbuf, mbuf, qi, w.rainHalfWidth);
        return twinQuadCount;
    }

    // ================= params buffer (field order frozen = Rust WeatherParams) =================

    static void writeParams(ByteBuffer p, WP w) {
        p.clear();
        p.putFloat(w.playerX);
        p.putFloat(w.playerZ);
        p.putFloat(w.eyeY);
        p.putFloat(w.seconds);
        p.putFloat(w.cellSpacing);
        p.putInt(w.anchorX);
        p.putInt(w.anchorZ);
        p.putInt(GRID);
        p.putInt(MAX_DROPS);
        p.putInt(MAX_WEATHER_QUADS);
        p.putFloat(w.anyFill);
        p.putFloat(w.rainFill);
        p.putFloat(w.snowFill);
        p.putFloat(w.rainScale);
        p.putFloat(w.rainHalfWidth);
        p.putFloat(w.windX);
        p.putFloat(w.windZ);
        p.putInt(w.rainColor);
        p.putInt(w.snowColor);
        p.putFloat(w.camX).putFloat(w.camY).putFloat(w.camZ).putFloat(w.camW);
        p.putFloat(w.fallScale);
        p.putFloat(w.swayBlocks);
        p.putFloat(w.sizeScale);
        p.putFloat(NEAR_IN);
        p.putFloat(NEAR_OUT);
        p.putFloat(SNOW_V_BASE);
        p.putFloat(SNOW_FALL_MIN);
        p.putFloat(SNOW_FALL_MAX);
        p.putFloat(SNOW_SWAY_FREQ);
        p.putFloat(SNOW_MIN);
        p.putFloat(SNOW_MAX);
        p.putInt(RT_RAIN_MARKER);
        p.putInt(RT_SNOW_MARKER);
        p.putInt(FLAKES_PER_CELL);
        p.flip();
    }

    // ================= main =================

    public static void main(String[] args) throws Throwable {
        String dll = args.length > 0 ? args[0] : "dist/orion_farfield.dll";
        try (Arena arena = Arena.ofConfined()) {
            SymbolLookup lib = SymbolLookup.libraryLookup(Path.of(dll), arena);
            Linker linker = Linker.nativeLinker();

            MethodHandle ver = linker.downcallHandle(lib.find("off_abi_version").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT));
            int abi = (int) ver.invokeExact();
            System.out.println("[M0] off_abi_version = " + abi + " (expect 3)");
            if (abi != 3) System.exit(1);

            MethodHandle sum = linker.downcallHandle(lib.find("off_sum_u32").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_DOUBLE,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            int n = 1 << 20;
            MemorySegment sumSeg = arena.allocate((long) n * 4);
            for (int i = 0; i < n; i++) sumSeg.set(ValueLayout.JAVA_INT, (long) i * 4, i);
            double got = (double) sum.invokeExact(sumSeg, n);
            System.out.println("[M0] sum ok = " + (got == ((double) (n - 1) * n / 2)));

            MethodHandle conv = linker.downcallHandle(lib.find("off_convert_dh_section").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));

            // ---------- DH parity (M2, must stay bit-exact) ----------
            int[][] configs = {{1, 1}, {1, 0}, {2, 1}, {16, 1}, {16, 0}, {64, 1}, {512, 0}, {512, 1}};
            int[] quadCounts = {0, 1, 7, 331, 4096};
            long seed = 20261117L;
            int fails = 0;
            int cases = 0;
            for (int[] cfg : configs) {
                for (int quads : quadCounts) {
                    seed++;
                    int blockWidth = cfg[0];
                    boolean tex = cfg[1] == 1;
                    byte[] src = genDhStream(quads, blockWidth, seed);
                    byte[] j = convertJava(src, blockWidth, tex);
                    int dstCap = Math.max(64, src.length);
                    MemorySegment srcSeg = arena.allocate(Math.max(1, src.length));
                    MemorySegment.copy(src, 0, srcSeg, ValueLayout.JAVA_BYTE, 0, src.length);
                    MemorySegment dstSeg = arena.allocate(dstCap);
                    int written = (int) conv.invokeExact(srcSeg, src.length, dstSeg, dstCap, blockWidth, tex ? 1 : 0);
                    cases++;
                    boolean ok = written == j.length;
                    if (ok && written > 0) {
                        for (int i = 0; i < written && ok; i++) {
                            if (dstSeg.get(ValueLayout.JAVA_BYTE, i) != j[i]) {
                                ok = false;
                                System.out.println("  [DH DIFF] first mismatch @ " + i);
                            }
                        }
                    }
                    if (!ok) fails++;
                }
            }
            System.out.println("[M2] DH parity cases=" + cases + " fails=" + fails
                    + (fails == 0 ? "  => PASS" : "  => FAIL"));
            if (fails > 0) System.exit(2);

            // ---------- M3 batch kernel: off_convert_sections ----------
            MethodHandle convBatch = linker.downcallHandle(lib.find("off_convert_sections").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,   // src, total_len
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, // lens, widths, count
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,   // dst, dst_cap
                            ValueLayout.JAVA_INT,                        // texture_enabled
                            ValueLayout.ADDRESS));                       // out_lens

            final int batchSections = 64;
            int[] bWidths = new int[batchSections];
            int[] bLens = new int[batchSections];
            byte[][] bSrc = new byte[batchSections][];
            int totalLen = 0;
            Random brnd = new Random(99_000);
            for (int i = 0; i < batchSections; i++) {
                bWidths[i] = 16 + (i % 3) * 8;
                final int q = 64 + brnd.nextInt(192);
                bSrc[i] = genDhStream(q, bWidths[i], 31337L + i);
                bLens[i] = bSrc[i].length;
                totalLen += bSrc[i].length;
            }
            ByteBuffer bSrcBuf = ByteBuffer.allocateDirect(totalLen).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer bLensBuf = ByteBuffer.allocateDirect(batchSections * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer bWidthsBuf = ByteBuffer.allocateDirect(batchSections * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer bOutLensBuf = ByteBuffer.allocateDirect(batchSections * 4).order(ByteOrder.LITTLE_ENDIAN);
            for (int i = 0; i < batchSections; i++) {
                bSrcBuf.put(bSrc[i]);
                bLensBuf.putInt(bLens[i]);
                bWidthsBuf.putInt(bWidths[i]);
            }
            bSrcBuf.flip();
            bLensBuf.flip();
            bWidthsBuf.flip();
            ByteBuffer bDst = ByteBuffer.allocateDirect(totalLen).order(ByteOrder.LITTLE_ENDIAN);

            int converted = (int) convBatch.invokeExact(
                    MemorySegment.ofBuffer(bSrcBuf), totalLen,
                    MemorySegment.ofBuffer(bLensBuf), MemorySegment.ofBuffer(bWidthsBuf), batchSections,
                    MemorySegment.ofBuffer(bDst), totalLen,
                    1, MemorySegment.ofBuffer(bOutLensBuf));
            boolean batchOk = converted == batchSections;
            if (batchOk) {
                int at = 0;
                for (int i = 0; i < batchSections && batchOk; i++) {
                    final byte[] expect = convertJava(bSrc[i], bWidths[i], true);
                    final int outLen = bOutLensBuf.getInt(i * 4);
                    if (outLen != expect.length) {
                        batchOk = false;
                        System.out.println("  [BATCH DIFF] len @" + i + " rust=" + outLen + " java=" + expect.length);
                        continue;
                    }
                    for (int k = 0; k < expect.length && batchOk; k++) {
                        if (bDst.get(at + k) != expect[k]) {
                            batchOk = false;
                            System.out.println("  [BATCH DIFF] byte @" + i + "/" + k);
                        }
                    }
                    at += expect.length;
                }
            }
            System.out.println("[M3] batch parity sections=" + batchSections + " => "
                    + (batchOk ? "PASS" : "FAIL"));
            if (!batchOk) System.exit(2);

            // ---------- M3 AB: java per-section loop vs one batch call ----------
            final int abSections = 256;
            int[] abWidths = new int[abSections];
            int[] abLens = new int[abSections];
            byte[][] abSrc = new byte[abSections][];
            int abTotal = 0;
            for (int i = 0; i < abSections; i++) {
                abWidths[i] = 64;
                abSrc[i] = genDhStream(8192, 64, 88_000L + i);
                abLens[i] = abSrc[i].length;
                abTotal += abLens[i];
            }
            ByteBuffer abSrcBuf = ByteBuffer.allocateDirect(abTotal).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer abLensBuf = ByteBuffer.allocateDirect(abSections * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer abWidthsBuf = ByteBuffer.allocateDirect(abSections * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer abOutLensBuf = ByteBuffer.allocateDirect(abSections * 4).order(ByteOrder.LITTLE_ENDIAN);
            for (int i = 0; i < abSections; i++) {
                abSrcBuf.put(abSrc[i]);
                abLensBuf.putInt(abLens[i]);
                abWidthsBuf.putInt(abWidths[i]);
            }
            abSrcBuf.flip();
            abLensBuf.flip();
            abWidthsBuf.flip();
            ByteBuffer abDst = ByteBuffer.allocateDirect(abTotal).order(ByteOrder.LITTLE_ENDIAN);
            long bestAbJ = Long.MAX_VALUE, bestAbR = Long.MAX_VALUE;
            for (int r = 0; r < 12; r++) {
                long t0 = System.nanoTime();
                for (int i = 0; i < abSections; i++) convertJava(abSrc[i], abWidths[i], true);
                long t1 = System.nanoTime();
                int cnv = (int) convBatch.invokeExact(
                        MemorySegment.ofBuffer(abSrcBuf), abTotal,
                        MemorySegment.ofBuffer(abLensBuf), MemorySegment.ofBuffer(abWidthsBuf), abSections,
                        MemorySegment.ofBuffer(abDst), abTotal,
                        1, MemorySegment.ofBuffer(abOutLensBuf));
                long t2 = System.nanoTime();
                if (cnv != abSections) { System.out.println("[M3 AB] rust error"); System.exit(3); }
                if (r >= 4) {
                    bestAbJ = Math.min(bestAbJ, t1 - t0);
                    bestAbR = Math.min(bestAbR, t2 - t1);
                }
            }
            System.out.println("[M3 AB] workload = " + abSections + " sections x 8192 quads ("
                    + (abTotal / 1024) + " KiB src)");
            System.out.printf("[M3 AB] java per-section best = %.2f ms%n", bestAbJ / 1e6);
            System.out.printf("[M3 AB] rust one-batch best  = %.2f ms%n", bestAbR / 1e6);
            System.out.printf("[M3 AB] speedup = %.2fx%n", (double) bestAbJ / bestAbR);

            // ---------- weather handles ----------
            MethodHandle plan = linker.downcallHandle(lib.find("off_weather_plan").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            MethodHandle build = linker.downcallHandle(lib.find("off_weather_build").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

            ByteBuffer params = ByteBuffer.allocateDirect(37 * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer planBuf = ByteBuffer.allocateDirect(MAX_DROPS * PLAN_WORDS * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer hBuf = ByteBuffer.allocateDirect(MAX_DROPS * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer pBuf = ByteBuffer.allocateDirect(MAX_DROPS).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer fBuf = ByteBuffer.allocateDirect(MAX_DROPS * 2 * 4).order(ByteOrder.LITTLE_ENDIAN);
            int vCapBytes = MAX_WEATHER_QUADS * 6 * 12 * 4;
            ByteBuffer rustV = ByteBuffer.allocateDirect(vCapBytes).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer rustM = ByteBuffer.allocateDirect(MAX_WEATHER_QUADS * 6 * 3 * 4).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer twinV = ByteBuffer.allocateDirect(vCapBytes).order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer twinM = ByteBuffer.allocateDirect(MAX_WEATHER_QUADS * 6 * 3 * 4).order(ByteOrder.LITTLE_ENDIAN);
            int[][] twinPlan = new int[MAX_DROPS][PLAN_WORDS];

            // synthetic probes (shared by twin and rust build)
            Random rnd = new Random(20261117L);
            int[] heights = new int[MAX_DROPS];
            int[] precip = new int[MAX_DROPS];
            int[] flakeH = new int[MAX_DROPS * 2];
            for (int c = 0; c < MAX_DROPS; c++) {
                heights[c] = 60 + rnd.nextInt(141);
                int pc = rnd.nextInt(100);
                precip[c] = pc < 20 ? 0 : (pc < 70 ? 1 : 2);
                flakeH[c * 2] = 60 + rnd.nextInt(141);
                flakeH[c * 2 + 1] = 60 + rnd.nextInt(141);
            }

            // ---------- weather parity across 4 scenarios ----------
            int wFails = 0;
            int wCases = 0;
            float[][] scenarioFills = {{1, 0}, {0, 1}, {1, 1}, {0, 0}};
            for (int sc = 0; sc < scenarioFills.length; sc++) {
                WP w = new WP();
                w.anchorX = rnd.nextInt(20001) - 10000;
                w.anchorZ = rnd.nextInt(20001) - 10000;
                w.playerX = w.anchorX * 7.3F + 11.7F;
                w.playerZ = w.anchorZ * 5.1F - 3.3F;
                w.eyeY = 70.5F;
                w.seconds = 123.4F + sc;
                w.cellSpacing = 1.35F * 16 / 42;
                w.anyFill = Math.max(scenarioFills[sc][0], scenarioFills[sc][1]);
                w.rainFill = scenarioFills[sc][0];
                w.snowFill = scenarioFills[sc][1];
                w.rainScale = 0.6F + rnd.nextFloat();
                w.rainHalfWidth = 0.039F;
                w.windX = 0.32F * 1.1F;
                w.windZ = 0.11F * 1.1F;
                w.rainColor = argbFromFloat(0.8F, 0.89F, 0.93F, 0.96F);
                w.snowColor = argbFromFloat(0.9F, 0.93F, 0.95F, 0.98F);
                w.camX = rnd.nextFloat() - 0.5F;
                w.camY = rnd.nextFloat() - 0.5F;
                w.camZ = rnd.nextFloat() - 0.5F;
                w.camW = 1.0F;
                float len = (float) Math.sqrt(w.camX * w.camX + w.camY * w.camY + w.camZ * w.camZ + w.camW * w.camW);
                w.camX /= len; w.camY /= len; w.camZ /= len; w.camW /= len;
                w.fallScale = 1.0F + rnd.nextFloat() * 0.5F;
                w.swayBlocks = 0.30F;
                w.sizeScale = 1.0F + rnd.nextFloat() * 0.4F;

                // twin
                twinV.clear();
                twinM.clear();
                int jq = weatherTwin(w, heights, precip, flakeH, twinPlan, twinV, twinM);

                // rust plan + verify coordinates
                writeParams(params, w);
                planBuf.clear(); // ofBuffer 视图基于 [position,limit)，上一场景读完后必须归零
                int planned = (int) plan.invokeExact(
                        MemorySegment.ofBuffer(params), MemorySegment.ofBuffer(planBuf), MAX_DROPS);
                planBuf.position(0);
                int planDiffs = 0;
                if (planned == MAX_DROPS) {
                    for (int c = 0; c < MAX_DROPS; c++) {
                        for (int k = 0; k < PLAN_WORDS; k++) {
                            if (planBuf.getInt() != twinPlan[c][k]) planDiffs++;
                        }
                    }
                } else {
                    planDiffs = Integer.MAX_VALUE;
                }

                // rust build
                hBuf.clear();
                for (int c = 0; c < MAX_DROPS; c++) hBuf.putInt(heights[c]);
                hBuf.flip();
                pBuf.clear();
                for (int c = 0; c < MAX_DROPS; c++) pBuf.put((byte) precip[c]);
                pBuf.flip();
                fBuf.clear();
                for (int c = 0; c < MAX_DROPS * 2; c++) fBuf.putInt(flakeH[c]);
                fBuf.flip();
                rustV.clear();
                rustM.clear();
                int rq = (int) build.invokeExact(
                        MemorySegment.ofBuffer(params), MemorySegment.ofBuffer(hBuf),
                        MemorySegment.ofBuffer(pBuf), MemorySegment.ofBuffer(fBuf),
                        MemorySegment.ofBuffer(rustV), rustV.remaining() / 4,
                        MemorySegment.ofBuffer(rustM), rustM.remaining() / 4);
                wCases++;

                boolean ok = rq == jq && planDiffs == 0;
                int maxPosE6 = 0;
                if (ok && rq > 0) {
                    // compare vertices: pos (f0..2) tolerance 2e-4, other 9 fields bit-exact
                    float[] rv = new float[rq * 6 * 12];
                    rustV.position(0);
                    for (int i = 0; i < rv.length; i++) rv[i] = rustV.getFloat();
                    twinV.position(0);
                    for (int vtx = 0; vtx < rq * 6 && ok; vtx++) {
                        for (int f = 0; f < 12; f++) {
                            float rvv = rv[vtx * 12 + f];
                            float jvv = twinV.getFloat();
                            if (f <= 2) {
                                float d = Math.abs(rvv - jvv);
                                maxPosE6 = Math.max(maxPosE6, (int) Math.ceil(d * 1.0e6F));
                                if (d > 2.0e-4F) {
                                    ok = false;
                                    System.out.println("  [WX DIFF] pos v" + vtx + " f" + f
                                            + " rust=" + rvv + " java=" + jvv);
                                }
                            } else if (Float.floatToRawIntBits(rvv) != Float.floatToRawIntBits(jvv)) {
                                ok = false;
                                System.out.println("  [WX DIFF] field v" + vtx + " f" + f
                                        + " rust=" + rvv + " java=" + jvv);
                            }
                        }
                    }
                    // motion bit-exact
                    for (int i = 0; i < rq * 6 * 3 && ok; i++) {
                        float rm = rustM.getFloat(i * 4);
                        float jm = twinM.getFloat(i * 4);
                        if (Float.floatToRawIntBits(rm) != Float.floatToRawIntBits(jm)) {
                            ok = false;
                            System.out.println("  [WX DIFF] motion @ " + i);
                        }
                    }
                }
                if (!ok) wFails++;
                System.out.println("[WX] scenario " + sc + " (rain=" + w.rainFill + " snow=" + w.snowFill
                        + ") quads rust/java=" + rq + "/" + jq
                        + " planDiffs=" + (planDiffs == Integer.MAX_VALUE ? -1 : planDiffs)
                        + " maxPosDiffE-6=" + maxPosE6
                        + (ok ? "  => PASS" : "  => FAIL"));
            }
            System.out.println("[WX] weather cases=" + wCases + " fails=" + wFails
                    + (wFails == 0 ? "  => PASS" : "  => FAIL"));
            if (wFails > 0) System.exit(2);

            // ---------- VPT parity: twin (production logic) vs rust plan+build ----------
            MethodHandle vptPlan = linker.downcallHandle(lib.find("off_vpt_plan").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            MethodHandle vptBuild = linker.downcallHandle(lib.find("off_vpt_build").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.JAVA_INT));

            final int VPT_QUADS = 4096;
            int vptFails = 0;
            int vptCases = 0;
            long bestVJ = Long.MAX_VALUE, bestVR = Long.MAX_VALUE;
            for (int sc = 0; sc < 3; sc++) {
                Random vrnd = new Random(777_000L + sc);
                ByteBuffer vsrc = ByteBuffer.allocateDirect(VPT_QUADS * 4 * 28).order(ByteOrder.LITTLE_ENDIAN);
                for (int q = 0; q < VPT_QUADS; q++) {
                    float bx = vrnd.nextInt(24) - 4.0F;   // 含出界坐标（<0 或 >16）
                    float by = vrnd.nextInt(24) - 4.0F;
                    float bz = vrnd.nextInt(24) - 4.0F;
                    float w = 0.5F + vrnd.nextFloat() * 2.0F;
                    int mode = q % 8;                     // 0=平X 1=平Y 2=平Z 3=退化 4..7=随机面
                    for (int v = 0; v < 4; v++) {
                        float dx = switch (mode) { case 0, 1 -> vrnd.nextFloat() * w; case 3 -> 0.0F;
                            default -> (v == 1 || v == 2) ? w : 0.0F; };
                        float dy = switch (mode) { case 0 -> vrnd.nextFloat() * w; case 2, 3 -> 0.0F;
                            default -> (v == 2 || v == 3) ? w : 0.0F; };
                        float dz = switch (mode) { case 1 -> vrnd.nextFloat() * w; case 2, 3 -> 0.0F;
                            default -> (v == 3) ? w * 0.7F : 0.0F; };
                        vsrc.putFloat(bx + dx).putFloat(by + dy).putFloat(bz + dz);
                        vsrc.putFloat(vrnd.nextFloat() * 3.0F);   // +12
                        vsrc.putFloat(vrnd.nextFloat() * 3.0F);   // +16 u
                        vsrc.putFloat(vrnd.nextFloat() * 3.0F);   // +20 v
                        for (int b = 0; b < 4; b++) vsrc.put((byte) vrnd.nextInt(256)); // +24..27（27 是标记字节）
                    }
                }
                vsrc.flip();
                final boolean cutout = sc != 1;
                final boolean doAlbedo = sc != 2;

                ByteBuffer vpar = ByteBuffer.allocateDirect(48).order(ByteOrder.LITTLE_ENDIAN);
                vpar.putInt(0).putInt(0).putInt(0);               // min_x/y/z（孪生/rust 同值）
                vpar.putInt(VPT_QUADS * 4).putInt(28);
                vpar.putInt(256).putInt(256);                     // atlas 256x256
                vpar.putInt(cutout ? 1 : 0).putInt(doAlbedo ? 1 : 0);
                vpar.putInt(VPT_QUADS);
                vpar.putLong(0L);                                  // 指针占位，下方重写
                vpar.flip();

                // 真实 atlas 内存（twin 与 rust 共读同一指针）
                MemorySegment atlasSeg = arena.allocate(256L * 256L * 4L);
                Random arnd = new Random(555L);
                for (int i = 0; i < 256 * 256; i++) {
                    atlasSeg.set(ValueLayout.JAVA_BYTE, (long) i * 4, (byte) arnd.nextInt(256));
                    atlasSeg.set(ValueLayout.JAVA_BYTE, (long) i * 4 + 1, (byte) arnd.nextInt(256));
                    atlasSeg.set(ValueLayout.JAVA_BYTE, (long) i * 4 + 2, (byte) arnd.nextInt(256));
                }
                vpar.position(40);
                vpar.putLong(atlasSeg.address());
                vpar.position(0);

                ByteBuffer vptPlanRust = ByteBuffer.allocateDirect(VPT_QUADS * 48).order(ByteOrder.LITTLE_ENDIAN);
                vptPlanRust.clear();
                int slots = (int) vptPlan.invokeExact(
                        MemorySegment.ofBuffer(vpar), MemorySegment.ofBuffer(vsrc), vsrc.remaining(),
                        MemorySegment.ofBuffer(vptPlanRust), vptPlanRust.remaining());
                if (slots != VPT_QUADS * 6) {
                    System.out.println("  [VPT DIFF] slots=" + slots);
                    vptFails++;
                    vptCases++;
                    continue;
                }

                // twin：生产逻辑 + 同一谓词源；同时写 plan 槽与探针位（与生产探针循环同构）
                ByteBuffer twinVerts = ByteBuffer.allocateDirect(VPT_QUADS * 4 * 28).order(ByteOrder.LITTLE_ENDIAN);
                twinVerts.put(vsrc.duplicate().position(0));
                twinVerts.flip();
                ByteBuffer vptPlanTwin = ByteBuffer.allocateDirect(VPT_QUADS * 48).order(ByteOrder.LITTLE_ENDIAN);
                ByteBuffer vptBitsTwin = ByteBuffer.allocateDirect(VPT_QUADS * 6).order(ByteOrder.LITTLE_ENDIAN);
                int[] jR = new int[4096]; int[] jG = new int[4096];
                int[] jB = new int[4096]; int[] jC = new int[4096];
                boolean twinAny = vptTwin(vpar, twinVerts, vptPlanTwin, vptBitsTwin, atlasSeg,
                        jR, jG, jB, jC, cutout, doAlbedo);
                vpar.position(0); // twin 读到末尾，build 的 segment 视图必须从 0 重取

                // plan 字节级对比（含 kind 与坐标）
                vptPlanTwin.position(0);
                vptPlanRust.position(0);
                int planDiffs = 0;
                for (int i = 0; i < VPT_QUADS * 48; i++) {
                    if (vptPlanTwin.get() != vptPlanRust.get()) planDiffs++;
                }

                // rust build（bits 来自 twin —— 与生产 Java 探针循环同构）
                ByteBuffer rustVerts = ByteBuffer.allocateDirect(VPT_QUADS * 4 * 28).order(ByteOrder.LITTLE_ENDIAN);
                rustVerts.put(vsrc.duplicate().position(0));
                rustVerts.flip();
                vptPlanRust.position(0);
                vptBitsTwin.position(0);
                ByteBuffer[] vAcc = new ByteBuffer[4];
                for (int i = 0; i < 4; i++) {
                    vAcc[i] = ByteBuffer.allocateDirect(4096 * 4).order(ByteOrder.LITTLE_ENDIAN);
                }
                int[] rR = new int[4096]; int[] rG = new int[4096]; int[] rB = new int[4096]; int[] rC = new int[4096];
                int rAny = (int) vptBuild.invokeExact(
                        MemorySegment.ofBuffer(vpar), MemorySegment.ofBuffer(rustVerts), rustVerts.remaining(),
                        MemorySegment.ofBuffer(vptPlanRust), vptPlanRust.remaining(),
                        MemorySegment.ofBuffer(vptBitsTwin), vptBitsTwin.remaining(),
                        MemorySegment.ofBuffer(vAcc[0]),
                        MemorySegment.ofBuffer(vAcc[1]),
                        MemorySegment.ofBuffer(vAcc[2]),
                        MemorySegment.ofBuffer(vAcc[3]),
                        4096);
                vAcc[0].asIntBuffer().get(rR);
                vAcc[1].asIntBuffer().get(rG);
                vAcc[2].asIntBuffer().get(rB);
                vAcc[3].asIntBuffer().get(rC);
                vptCases++;
                int jcSum = 0, rcSum = 0;
                for (int i = 0; i < 4096; i++) { jcSum += jC[i]; rcSum += rC[i]; }
                System.out.println("  [VPT] sampleCount twin=" + jcSum + " rust=" + rcSum);
                boolean ok = rAny >= 0 && ((rAny != 0) == twinAny) && planDiffs == 0;
                if (ok) {
                    for (int i = 0; i < 4096 && ok; i++) {
                        ok = rR[i] == jR[i] && rG[i] == jG[i] && rB[i] == jB[i] && rC[i] == jC[i];
                        if (!ok) System.out.println("  [VPT DIFF] acc @ " + i
                                + " jR=" + jR[i] + " rR=" + rR[i] + " jC=" + jC[i] + " rC=" + rC[i]);
                    }
                }
                if (ok) {
                    rustVerts.position(0);
                    twinVerts.position(0);
                    for (int i = 0; i < VPT_QUADS * 4 * 28 && ok; i++) {
                        if (rustVerts.get(i) != twinVerts.get(i)) {
                            ok = false;
                            System.out.println("  [VPT DIFF] vertex byte @ " + i);
                        }
                    }
                }
                if (!ok) vptFails++;
                System.out.println("[VPT] scenario " + sc + " (cutout=" + cutout + " albedo=" + doAlbedo
                        + ") any=" + rAny + "/" + (twinAny ? 1 : 0)
                        + " planDiffs=" + planDiffs
                        + (ok ? "  => PASS" : "  => FAIL"));
            }
            System.out.println("[VPT] vpt cases=" + vptCases + " fails=" + vptFails
                    + (vptFails == 0 ? "  => PASS" : "  => FAIL"));
            if (vptFails > 0) System.exit(2);

            // ---------- VPT A/B：Java 全流程（含探针/槽写出）vs Rust plan+build（探针留在 Java 侧 = 生产形态）----------
            {
                Random vrnd = new Random(777_000L);
                ByteBuffer vsrc = ByteBuffer.allocateDirect(VPT_QUADS * 4 * 28).order(ByteOrder.LITTLE_ENDIAN);
                for (int q = 0; q < VPT_QUADS; q++) {
                    float bx = vrnd.nextInt(24) - 4.0F, by = vrnd.nextInt(24) - 4.0F, bz = vrnd.nextInt(24) - 4.0F;
                    float w = 0.5F + vrnd.nextFloat() * 2.0F;
                    int mode = q % 8;
                    for (int v = 0; v < 4; v++) {
                        float dx = switch (mode) { case 0, 1 -> vrnd.nextFloat() * w; case 3 -> 0.0F;
                            default -> (v == 1 || v == 2) ? w : 0.0F; };
                        float dy = switch (mode) { case 0 -> vrnd.nextFloat() * w; case 2, 3 -> 0.0F;
                            default -> (v == 2 || v == 3) ? w : 0.0F; };
                        float dz = switch (mode) { case 1 -> vrnd.nextFloat() * w; case 2, 3 -> 0.0F;
                            default -> (v == 3) ? w * 0.7F : 0.0F; };
                        vsrc.putFloat(bx + dx).putFloat(by + dy).putFloat(bz + dz);
                        vsrc.putFloat(vrnd.nextFloat() * 3.0F);
                        vsrc.putFloat(vrnd.nextFloat() * 3.0F);
                        vsrc.putFloat(vrnd.nextFloat() * 3.0F);
                        for (int b = 0; b < 4; b++) vsrc.put((byte) vrnd.nextInt(256));
                    }
                }
                vsrc.flip();
                MemorySegment atlasSeg2 = arena.allocate(256L * 256L * 4L);
                ByteBuffer vpar2 = ByteBuffer.allocateDirect(48).order(ByteOrder.LITTLE_ENDIAN);
                vpar2.putInt(0).putInt(0).putInt(0).putInt(VPT_QUADS * 4).putInt(28);
                vpar2.putInt(256).putInt(256).putInt(1).putInt(1).putInt(VPT_QUADS);
                vpar2.putLong(atlasSeg2.address());
                vpar2.flip();
                int[] jR = new int[4096]; int[] jG = new int[4096]; int[] jB = new int[4096]; int[] jC = new int[4096];
                ByteBuffer twinVerts2 = ByteBuffer.allocateDirect(VPT_QUADS * 4 * 28).order(ByteOrder.LITTLE_ENDIAN);
                ByteBuffer vptPlanTwin2 = ByteBuffer.allocateDirect(VPT_QUADS * 48).order(ByteOrder.LITTLE_ENDIAN);
                ByteBuffer vptBitsTwin2 = ByteBuffer.allocateDirect(VPT_QUADS * 6).order(ByteOrder.LITTLE_ENDIAN);
                ByteBuffer rustVerts2 = ByteBuffer.allocateDirect(VPT_QUADS * 4 * 28).order(ByteOrder.LITTLE_ENDIAN);
                ByteBuffer vptPlanRust2 = ByteBuffer.allocateDirect(VPT_QUADS * 48).order(ByteOrder.LITTLE_ENDIAN);
                ByteBuffer[] vAcc2 = new ByteBuffer[4];
                for (int i = 0; i < 4; i++) vAcc2[i] = ByteBuffer.allocateDirect(4096 * 4).order(ByteOrder.LITTLE_ENDIAN);
                bestVJ = Long.MAX_VALUE;
                bestVR = Long.MAX_VALUE;
                for (int r = 0; r < 24; r++) {
                    long t0 = System.nanoTime();
                    vptTwin(vpar2, twinVerts2, vptPlanTwin2, vptBitsTwin2, atlasSeg2, jR, jG, jB, jC, true, true);
                    vpar2.position(0);
                    long t1 = System.nanoTime();
                    vptPlanRust2.clear();
                    int slots = (int) vptPlan.invokeExact(
                            MemorySegment.ofBuffer(vpar2), MemorySegment.ofBuffer(vsrc), vsrc.remaining(),
                            MemorySegment.ofBuffer(vptPlanRust2), vptPlanRust2.remaining());
                    long t2 = System.nanoTime();
                    vptPlanRust2.position(0);
                    vptBitsTwin2.position(0);
                    int rq = (int) vptBuild.invokeExact(
                            MemorySegment.ofBuffer(vpar2), MemorySegment.ofBuffer(rustVerts2), rustVerts2.remaining(),
                            MemorySegment.ofBuffer(vptPlanRust2), vptPlanRust2.remaining(),
                            MemorySegment.ofBuffer(vptBitsTwin2), vptBitsTwin2.remaining(),
                            MemorySegment.ofBuffer(vAcc2[0]),
                            MemorySegment.ofBuffer(vAcc2[1]),
                            MemorySegment.ofBuffer(vAcc2[2]),
                            MemorySegment.ofBuffer(vAcc2[3]),
                            4096);
                    long t3 = System.nanoTime();
                    if (slots != VPT_QUADS * 6 || rq < 0) { System.out.println("[VPT AB] rust error"); System.exit(3); }
                    if (r >= 8) {
                        bestVJ = Math.min(bestVJ, t1 - t0);
                        bestVR = Math.min(bestVR, t3 - t1);
                    }
                }
                System.out.println("[VPT AB] workload = " + VPT_QUADS + " quads (java 含探针/槽写出；rust=plan+build，探针留 Java = 生产形态)");
                System.out.printf("[VPT AB] java full = %.2f ms%n", bestVJ / 1e6);
                System.out.printf("[VPT AB] rust plan+build = %.2f ms%n", bestVR / 1e6);
                System.out.printf("[VPT AB] math-offload speedup = %.2fx%n", (double) bestVJ / bestVR);
            }

            // ---------- weather A/B: java twin vs rust (plan+build), full rain ----------
            WP w = new WP();
            w.anchorX = 777;
            w.anchorZ = -333;
            w.playerX = 777 * 7.3F + 11.7F;
            w.playerZ = -333 * 5.1F - 3.3F;
            w.eyeY = 70.5F;
            w.cellSpacing = 1.35F * 16 / 42;
            w.anyFill = 1.0F;
            w.rainFill = 1.0F;
            w.snowFill = 0.0F;
            w.rainScale = 1.1F;
            w.rainHalfWidth = 0.039F;
            w.windX = 0.32F;
            w.windZ = 0.11F;
            w.rainColor = argbFromFloat(0.8F, 0.89F, 0.93F, 0.96F);
            w.snowColor = argbFromFloat(0.9F, 0.93F, 0.95F, 0.98F);
            w.camX = 0.1F; w.camY = 0.2F; w.camZ = -0.05F; w.camW = 1.0F;
            float len = (float) Math.sqrt(0.01F + 0.04F + 0.0025F + 1.0F);
            w.camX /= len; w.camY /= len; w.camZ /= len; w.camW /= len;
            w.fallScale = 1.0F;
            w.swayBlocks = 0.30F;
            w.sizeScale = 1.0F;
            hBuf.clear();
            for (int c = 0; c < MAX_DROPS; c++) hBuf.putInt(heights[c]);
            hBuf.flip();
            pBuf.clear();
            for (int c = 0; c < MAX_DROPS; c++) pBuf.put((byte) precip[c]);
            pBuf.flip();
            fBuf.clear();
            for (int c = 0; c < MAX_DROPS * 2; c++) fBuf.putInt(flakeH[c]);
            fBuf.flip();
            int[][] abPlan = new int[MAX_DROPS][PLAN_WORDS];

            long bestJ = Long.MAX_VALUE, bestR = Long.MAX_VALUE, bestRp = Long.MAX_VALUE;
            for (int r = 0; r < 30; r++) {
                w.seconds = 1000.0F + r * 0.05F;
                writeParams(params, w);
                planBuf.clear();
                long t0 = System.nanoTime();
                twinV.clear();
                twinM.clear();
                weatherTwin(w, heights, precip, flakeH, abPlan, twinV, twinM);
                long t1 = System.nanoTime();
                int planned = (int) plan.invokeExact(
                        MemorySegment.ofBuffer(params), MemorySegment.ofBuffer(planBuf), MAX_DROPS);
                long t2 = System.nanoTime();
                rustV.clear();
                rustM.clear();
                int rq = (int) build.invokeExact(
                        MemorySegment.ofBuffer(params), MemorySegment.ofBuffer(hBuf),
                        MemorySegment.ofBuffer(pBuf), MemorySegment.ofBuffer(fBuf),
                        MemorySegment.ofBuffer(rustV), rustV.remaining() / 4,
                        MemorySegment.ofBuffer(rustM), rustM.remaining() / 4);
                long t3 = System.nanoTime();
                if (rq < 0 || planned != MAX_DROPS) {
                    System.out.println("[AB] rust error rq=" + rq + " planned=" + planned);
                    System.exit(3);
                }
                if (r >= 8) {
                    bestJ = Math.min(bestJ, t1 - t0);
                    bestR = Math.min(bestR, t3 - t1);
                    bestRp = Math.min(bestRp, t3 - t2);
                }
            }
            System.out.println("[AB] workload = " + MAX_DROPS + " cells, rainFill=1 (full rain)");
            System.out.printf("[AB] java twin best      = %.2f ms%n", bestJ / 1e6);
            System.out.printf("[AB] rust plan+build best = %.2f ms%n", bestR / 1e6);
            System.out.printf("[AB] rust build only best = %.2f ms%n", bestRp / 1e6);
            System.out.printf("[AB] speedup (plan+build) = %.2fx%n", (double) bestJ / bestR);
        }
    }

    // ================= VPT twin（生产镜像：plan 槽 + 谓词探针 + acc 累加 + 就地打标）=================

    static float quadSpan(float a, float b, float c, float d) {
        final float min = Math.min(Math.min(a, b), Math.min(c, d));
        final float max = Math.max(Math.max(a, b), Math.max(c, d));
        return max - min;
    }

    static int vptHash(int x, int y, int z, int salt) {
        int h = x * 73856093 ^ y * 19349663 ^ z * 83492791 ^ salt;
        h ^= h >>> 16;
        h *= 0x7feb352d;
        h ^= h >>> 15;
        h *= 0x846ca68b;
        return h ^ (h >>> 16);
    }

    static boolean vptAir(int x, int y, int z, int salt) {
        return (vptHash(x, y, z, salt) & 3) == 0;
    }

    static boolean vptLeaf(int x, int y, int z, int salt) {
        return (vptHash(x, y, z, salt + 11) & 7) == 0;
    }

    static boolean vptOcc(int x, int y, int z, int salt) {
        return (vptHash(x, y, z, salt + 23) & 3) != 0;
    }

    static void putSlot(ByteBuffer planOut, int quad, int s, int kind, int x, int y, int z) {
        final int o = (quad * 6 + s) * 8;
        planOut.put(o, (byte) kind);
        planOut.put(o + 1, (byte) 0);
        planOut.putShort(o + 2, (short) x);
        planOut.putShort(o + 4, (short) y);
        planOut.putShort(o + 6, (short) z);
    }

    /**
     * 生产 accumulateVptAlbedoAndTagRainPuddles 的完整孪生：同函数内完成 plan 槽写出、
     * 谓词探针（生产控制流）、albedo 累加与就地 byte27 打标。返回 vptAny。
     */
    static boolean vptTwin(ByteBuffer vpar, ByteBuffer verts, ByteBuffer planOut, ByteBuffer bitsOut,
                           MemorySegment atlas, int[] red, int[] green, int[] blue, int[] count,
                           boolean cutout, boolean doAlbedo) {
        vpar.position(0);
        vpar.getInt(); vpar.getInt(); vpar.getInt();               // min（谓词是合成源，不用世界坐标）
        final int vertexCount = vpar.getInt();
        vpar.getInt();                                              // stride == 28
        final int atlasW = vpar.getInt(), atlasH = vpar.getInt();
        final int quads = vertexCount / 4;
        final int salt = 1013;
        planOut.clear();
        bitsOut.clear();
        boolean any = false;
        for (int quad = 0; quad < quads; quad++) {
            float cx = 0, cy = 0, cz = 0, cu = 0, cv = 0;
            float[] vx = new float[4], vy = new float[4], vz = new float[4];
            for (int v = 0; v < 4; v++) {
                final int base = (quad * 4 + v) * 28;
                final float x = verts.getFloat(base), y = verts.getFloat(base + 4), z = verts.getFloat(base + 8);
                vx[v] = x; vy[v] = y; vz[v] = z;
                cx += x; cy += y; cz += z;
                cu += verts.getFloat(base + 16); cv += verts.getFloat(base + 20);
            }
            cx *= 0.25F; cy *= 0.25F; cz *= 0.25F; cu *= 0.25F; cv *= 0.25F;

            if (doAlbedo) {
                float e1x = vx[1] - vx[0], e1y = vy[1] - vy[0], e1z = vz[1] - vz[0];
                float e2x = vx[2] - vx[0], e2y = vy[2] - vy[0], e2z = vz[2] - vz[0];
                float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
                float nl = (float) Math.sqrt(nx * nx + ny * ny + nz * nz);
                if (nl > 1.0e-6F) {
                    nx /= nl; ny /= nl; nz /= nl;
                    final int fx = (int) Math.floor(cx - nx * 0.01F);
                    final int fy = (int) Math.floor(cy - ny * 0.01F);
                    final int fz = (int) Math.floor(cz - nz * 0.01F);
                    final boolean fOk = fx >= 0 && fx < 16 && fy >= 0 && fy < 16 && fz >= 0 && fz < 16;
                    if (fOk) {
                        putSlot(planOut, quad, 0, 1, fx, fy, fz);
                        final boolean air = vptAir(fx, fy, fz, salt);
                        bitsOut.put(quad * 6, (byte) (0x80 | (air ? 1 : 0)));
                        boolean sample = false;
                        int sx = fx, sy = fy, sz = fz;
                        final int bx = (int) Math.floor(cx + nx * 0.01F);
                        final int by = (int) Math.floor(cy + ny * 0.01F);
                        final int bz = (int) Math.floor(cz + nz * 0.01F);
                        final boolean bOk = bx >= 0 && bx < 16 && by >= 0 && by < 16 && bz >= 0 && bz < 16;
                        if (air) {
                            if (bOk) {
                                putSlot(planOut, quad, 1, 2, bx, by, bz);
                                final boolean backAir = vptAir(bx, by, bz, salt);
                                bitsOut.put(quad * 6 + 1, (byte) (0x80 | (backAir ? 1 : 0)));
                                if (!backAir) {
                                    sample = true;
                                    sx = bx; sy = by; sz = bz;
                                }
                            } else {
                                bitsOut.put(quad * 6 + 1, (byte) 0xFF);
                            }
                        } else {
                            if (bOk) putSlot(planOut, quad, 1, 2, bx, by, bz);
                            bitsOut.put(quad * 6 + 1, (byte) 0xFF);
                            sample = true; // front solid ⇒ probeInBounds 保持 true ⇒ 采 front（生产语义，此前孪生漏标）
                        }
                        if (sample) {
                            final int ax = Math.max(0, Math.min(atlasW - 1, (int) Math.floor(cu * atlasW)));
                            final int ay = Math.max(0, Math.min(atlasH - 1, (int) Math.floor(cv * atlasH)));
                            final int index = sx + 16 * (sz + 16 * sy);
                            final long off = ((long) ay * atlasW + ax) * 4L;
                            red[index] += atlas.get(ValueLayout.JAVA_BYTE, off) & 0xFF;
                            green[index] += atlas.get(ValueLayout.JAVA_BYTE, off + 1) & 0xFF;
                            blue[index] += atlas.get(ValueLayout.JAVA_BYTE, off + 2) & 0xFF;
                            count[index]++;
                            any = true;
                        }
                    }
                }
            }

            if (cutout) {
                float fnx = (vy[1] - vy[0]) * (vz[2] - vz[0]) - (vz[1] - vz[0]) * (vy[2] - vy[0]);
                float fny = (vz[1] - vz[0]) * (vx[2] - vx[0]) - (vx[1] - vx[0]) * (vz[2] - vz[0]);
                float fnz = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
                float fnl = (float) Math.sqrt(fnx * fnx + fny * fny + fnz * fnz);
                if (fnl > 1.0e-6F) {
                    fnx /= fnl; fny /= fnl; fnz /= fnl;
                    final int ax = (int) Math.floor(cx - fnx * 0.02F);
                    final int ay = (int) Math.floor(cy - fny * 0.02F);
                    final int az = (int) Math.floor(cz - fnz * 0.02F);
                    putSlot(planOut, quad, 2, 3, ax, ay, az);
                    final boolean leafA = vptLeaf(ax, ay, az, salt);
                    bitsOut.put(quad * 6 + 2, (byte) (0x80 | (leafA ? 2 : 0)));
                    boolean foliage = leafA;
                    final int bx = (int) Math.floor(cx);
                    final int by = (int) Math.floor(cy);
                    final int bz = (int) Math.floor(cz);
                    putSlot(planOut, quad, 3, 4, bx, by, bz);
                    if (!leafA) {
                        final boolean leafB = vptLeaf(bx, by, bz, salt);
                        bitsOut.put(quad * 6 + 3, (byte) (0x80 | (leafB ? 2 : 0)));
                        foliage = leafB;
                    } else {
                        bitsOut.put(quad * 6 + 3, (byte) 0xFF);
                    }
                    if (foliage) {
                        for (int v = 0; v < 4; v++) verts.put((quad * 4 + v) * 28 + 27, (byte) 25);
                    }
                }
            }

            final float spanX = quadSpan(vx[0], vx[1], vx[2], vx[3]);
            final float spanY = quadSpan(vy[0], vy[1], vy[2], vy[3]);
            final float spanZ = quadSpan(vz[0], vz[1], vz[2], vz[3]);
            final boolean fx2 = spanX <= 1.0e-4F, fy2 = spanY <= 1.0e-4F, fz2 = spanZ <= 1.0e-4F;
            if ((fx2 ? 1 : 0) + (fy2 ? 1 : 0) + (fz2 ? 1 : 0) == 1) {
                int px, py, pz, qx = 0, qy = 0, qz = 0;
                boolean hasB;
                if (fy2) {
                    px = (int) Math.floor(cx);
                    py = (int) Math.floor(cy - 0.01F);
                    pz = (int) Math.floor(cz);
                    hasB = false;
                } else {
                    final float sX = fx2 ? 1.0F : 0.0F;
                    final float sZ = fz2 ? 1.0F : 0.0F;
                    px = (int) Math.floor(cx - sX * 0.01F);
                    py = (int) Math.floor(cy);
                    pz = (int) Math.floor(cz - sZ * 0.01F);
                    qx = (int) Math.floor(cx + sX * 0.01F);
                    qy = (int) Math.floor(cy);
                    qz = (int) Math.floor(cz + sZ * 0.01F);
                    hasB = true;
                }
                putSlot(planOut, quad, 4, 5, px, py, pz);
                final boolean occA = vptOcc(px, py, pz, salt);
                bitsOut.put(quad * 6 + 4, (byte) (0x80 | (occA ? 4 : 0)));
                boolean resolved = occA;
                if (hasB) {
                    putSlot(planOut, quad, 5, 6, qx, qy, qz);
                    if (!occA) {
                        final boolean occB = vptOcc(qx, qy, qz, salt);
                        bitsOut.put(quad * 6 + 5, (byte) (0x80 | (occB ? 4 : 0)));
                        resolved = occB;
                    } else {
                        bitsOut.put(quad * 6 + 5, (byte) 0xFF);
                    }
                } else {
                    bitsOut.put(quad * 6 + 5, (byte) 0x00);
                }
                if (resolved) {
                    for (int v = 0; v < 4; v++) verts.put((quad * 4 + v) * 28 + 27, (byte) 23);
                }
            }
        }
        return any;
    }
}
