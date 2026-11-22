const uint kVptSparseSlotCount = kVptSparseTileSize * kVptSparseTileSize;

uint vptSparseBitReverse6(uint value)
{
    uint reversed = 0u;
    reversed |= ((value >> 0u) & 1u) << 5u;
    reversed |= ((value >> 1u) & 1u) << 4u;
    reversed |= ((value >> 2u) & 1u) << 3u;
    reversed |= ((value >> 3u) & 1u) << 2u;
    reversed |= ((value >> 4u) & 1u) << 1u;
    reversed |= ((value >> 5u) & 1u) << 0u;
    return reversed;
}

bool vptSparseLayeredActive(uvec2 pixelCoord, uint frameIdx, float samplingRate)
{
    const uint slotCount =
        uint(clamp(samplingRate, 0.0, 1.0) * float(kVptSparseSlotCount) + 0.5);
    if(slotCount == 0u)
    {
        return false;
    }
    if(slotCount >= kVptSparseSlotCount)
    {
        return true;
    }

    const uint slot =
        (pixelCoord.y & (kVptSparseTileSize - 1u)) * kVptSparseTileSize +
        (pixelCoord.x & (kVptSparseTileSize - 1u));
    const uint shuffled = vptSparseBitReverse6(slot);
    const uint offset = ((frameIdx & (kVptSparseSlotCount - 1u)) * slotCount) &
        (kVptSparseSlotCount - 1u);

    return ((shuffled + kVptSparseSlotCount - offset) &
        (kVptSparseSlotCount - 1u)) < slotCount;
}

// 直算版（掩码 pass 不可用时的回退，也是 -DSPARSE_USE_MASK=0 的实现）。
bool vptSparsePixelIsActiveDirect(uvec2 pixelCoord)
{
    float samplingRate = clamp(cam.sparseParams.y, 0.0, 100.0) * 0.01;
    if(samplingRate >= 1.0 || samplingRate <= 0.0)
    {
        return true;
    }

#if SPARSE_LAYERED_PATTERN
    return vptSparseLayeredActive(pixelCoord, cam.frameCount, samplingRate);
#elif SPARSE_RATE_NOISE_WHITE
    return vptWhiteNoise(pixelCoord, cam.frameCount, 0u) <= samplingRate;
#else
    return vptSparseBlueNoiseThreshold(pixelCoord, cam.frameCount) <= samplingRate;
#endif
}

bool vptSparsePixelIsActive(uvec2 pixelCoord)
{
    float samplingRate = clamp(cam.sparseParams.y, 0.0, 100.0) * 0.01;
    if(samplingRate >= 1.0 || samplingRate <= 0.0)
    {
        return true;
    }

#if SPARSE_USE_MASK
    // Remix sparse_rendering.slangh:29-37
    const uvec2 blockCoordinate = pixelCoord / uvec2(4u, 2u);
    const uvec2 blockLocal = pixelCoord % uvec2(4u, 2u);
    const uint bitIndex = blockLocal.y * 4u + blockLocal.x;
    const uint maskValue =
        imageLoad(sparseActiveMaskImage, ivec2(blockCoordinate)).r;

    return ((maskValue >> bitIndex) & 1u) != 0u;
#else
    return vptSparsePixelIsActiveDirect(pixelCoord);
#endif
}