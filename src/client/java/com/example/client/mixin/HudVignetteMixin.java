package com.example.client.mixin;

import com.example.vulkan.NativeVulkan;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.Hud;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-25 Batch P: Cancel vanilla elliptical vignette drawn in HUD pass.
 * Vanilla vignette runs after the RT hudless snapshot and darkens the entire
 * outer ring of the screen, which caused fg_ui_mask.comp to mark the outer
 * ring as uiAlpha=1 and blocked DLSS Frame Generation outside the center ellipse.
 */
@Mixin(Hud.class)
public abstract class HudVignetteMixin {

    @Inject(method = "extractVignette", at = @At("HEAD"), cancellable = true)
    private void rtx$skipVanillaVignette(
            GuiGraphicsExtractor graphics,
            Entity entity,
            CallbackInfo ci) {
        if (NativeVulkan.isLoaded()) {
            ci.cancel();
        }
    }
}
