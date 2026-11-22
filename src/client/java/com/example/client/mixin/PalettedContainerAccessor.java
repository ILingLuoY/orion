package com.example.client.mixin;

import net.minecraft.world.level.chunk.PalettedContainer;

import java.lang.reflect.Field;

/**
 * 2026-11-04 拿 {@code PalettedContainer} 的内部 {@code Data}（反射实现，**不是 Mixin**）。
 *
 * <p>动机：VLI 体素填充现在逐格调 {@code palette.get(x,y,z)}，而
 * {@code PalettedContainer.get(index)} 内部是
 * {@code palette.valueFor(storage.get(index))}。拿到 {@code Data} 后可以按 0..4095 的
 * 线性下标一次展开整段，每个调色板表项的状态分类只算一次。</p>
 *
 * <p><b>为什么不用 {@code @Accessor}</b>：真实字段类型是私有嵌套 record
 * {@code PalettedContainer$Data}，Java 源码里写不出这个类型；而 Mixin 的
 * {@code @Accessor} 是**按返回类型精确匹配字段描述符**的（返回 {@code Object} 或返回
 * 注入接口都会报 {@code No candidates were found matching data:L...;}），
 * 直接导致启动期 {@code InvalidAccessorException} —— 表现就是"一进世界就崩"。
 * 反射不受类型可见性限制，且 Minecraft 类不在命名模块里，
 * {@code setAccessible(true)} 可用。</p>
 *
 * <p>拿到的对象可以强转成 {@link PalettedContainerStorageAccessor} 取 {@code storage}。</p>
 */
public final class PalettedContainerAccessor {

    private static final Field DATA_FIELD = resolveDataField();

    private static Field resolveDataField() {
        try {
            final Field field = PalettedContainer.class.getDeclaredField("data");
            field.setAccessible(true);
            return field;
        } catch (Throwable throwable) {
            System.err.println("[RTX-SECTION] PalettedContainer.data 反射失败: " + throwable);
            return null;
        }
    }

    private PalettedContainerAccessor() {
    }

    /** 取容器的 {@code Data}；失败返回 null（调用方自行回退到逐格 get）。 */
    public static PalettedContainerStorageAccessor data(Object container) {
        if (DATA_FIELD == null || container == null) {
            return null;
        }
        try {
            return (PalettedContainerStorageAccessor) DATA_FIELD.get(container);
        } catch (Throwable throwable) {
            return null;
        }
    }
}
