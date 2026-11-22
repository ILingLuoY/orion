package com.example.client.mixin;

import net.minecraft.util.BitStorage;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;


/**
 * 2026-11-04 给 {@code PalettedContainer$Data}（私有 record）注入一个取
 * {@code storage} 的接口。
 *
 * <p>record 的组件在字节码里就是 private final 字段，{@code @Accessor} 可以直接读。
 * 注入接口之后，{@link PalettedContainerAccessor#orion$data()} 返回的 Object
 * 就能强转成本接口。
 */
@Mixin(targets = "net.minecraft.world.level.chunk.PalettedContainer$Data")
public interface PalettedContainerStorageAccessor {

    @Accessor("storage")
    BitStorage orion$storage();
}
