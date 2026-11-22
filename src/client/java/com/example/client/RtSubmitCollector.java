package com.example.client;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.gui.Font;
import net.minecraft.client.model.Model;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.renderer.OrderedSubmitNodeCollector;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.block.MovingBlockRenderState;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.renderer.entity.state.EntityRenderState;
import net.minecraft.client.renderer.feature.ModelFeatureRenderer;
import net.minecraft.client.renderer.gizmos.DrawableGizmoPrimitives;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.state.level.QuadParticleRenderState;
import net.minecraft.client.renderer.texture.UvMapping;
import net.minecraft.client.resources.model.geometry.ItemQuads;
import net.minecraft.network.chat.Component;
import net.minecraft.util.FormattedCharSequence;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.joml.Quaternionf;

import java.util.List;

/**
 * 2026-11-04 几何捕获层重构 —— 第 1 步（骨架）。
 *
 * <p><b>为什么需要这一层</b>：现有捕获挂在 {@code ModelFeatureRenderer.prepareModel} /
 * {@code RenderTypeFeatureRenderer.getVertexBuilder}，那是提交**之后**的层，拿不到
 * {@link UvMapping}。方块实体（箱子 / 告示牌 / 床 / 旗帜 / 展示框里的物品）的模型 UV
 * 是 ModelPart 的 0..1，必须按该 sprite 在图集里的矩形重映射，否则会采样到图集左上角。</p>
 *
 * <p><b>接口事实</b>（用 javap 反查 26.3 得到）：
 * {@code SubmitNodeCollector extends OrderedSubmitNodeCollector}，后者只有一个方法
 * {@code order(int)}；真正的方法全在父接口里，且**全部是 abstract**（没有 default 兜底），
 * 所以这里必须逐个实现。核心的那个是：</p>
 *
 * <pre>{@code
 * <S> void submitModel(Model<? super S>, S, PoseStack, RenderType,
 *                      int lightCoords, int overlayCoords, int tintedColor,
 *                      UvMapping uv, int outlineColor);
 * }</pre>
 *
 * <p>参考实现（Caustica 的 {@code RtEntityCollector}）走的是"自己驱动"：
 * {@code dispatcher.extractEntity(...)} 之后调
 * {@code dispatcher.submit(state, camera, ..., collector)} 把自己的 collector 传进去。
 * 我们后续也要按这个路子挂上去。</p>
 *
 * <p><b>本步只建骨架</b>：不接入任何现有流程，不影响渲染。目的仅是跑通编译，
 * 并确认 {@link UvMapping} 能在这里拿到。</p>
 */
public final class RtSubmitCollector implements SubmitNodeCollector {

    /** 最近一次 submitModel 携带的 UV 映射，供后续步骤做重映射时对照。 */
    private UvMapping lastUvMapping;

    /** 最近一次 submitModel 携带的 tint 颜色 / 光照，后续接材质时要用。 */
    private int lastTintedColor;
    private int lastLightCoords;

    @Override
    public OrderedSubmitNodeCollector order(int order) {
        return null;
    }

    @Override
    public <S> void submitModel(
            Model<? super S> model,
            S state,
            PoseStack poseStack,
            RenderType renderType,
            int lightCoords,
            int overlayCoords,
            int tintedColor,
            UvMapping uvMapping,
            int outlineColor) {
        this.lastUvMapping = uvMapping;
        this.lastTintedColor = tintedColor;
        this.lastLightCoords = lightCoords;
    }

    @Override
    public <S> void submitCrumblingOverlay(
            Model<? super S> model,
            S state,
            PoseStack poseStack,
            RenderType renderType,
            int lightCoords,
            int overlayCoords,
            int tintedColor,
            ModelFeatureRenderer.CrumblingOverlay crumblingOverlay) {
        // 破坏进度覆盖层：RT 里不画（与 Caustica 一致）。
    }

    @Override
    public void submitShadow(
            PoseStack poseStack,
            float radius,
            List<EntityRenderState.ShadowPiece> pieces) {
        // 原版软阴影贴片：RT 有自己的阴影，不引入。
    }

    @Override
    public void submitNameTag(
            PoseStack poseStack,
            Vec3 nameTagAttachment,
            int offset,
            Component name,
            boolean seeThrough,
            int lightCoords,
            CameraRenderState camera) {
        // 名牌：屏幕空间元素，不参与 RT。
    }

    @Override
    public void submitText(
            PoseStack poseStack,
            float x,
            float y,
            FormattedCharSequence string,
            boolean dropShadow,
            Font.DisplayMode displayMode,
            int lightCoords,
            int color,
            int backgroundColor,
            int outlineColor) {
        // 文本：正交字形，RT 里由 UI 层负责。
    }

    @Override
    public void submitTextBackground(
            PoseStack poseStack,
            float x,
            float y,
            float width,
            float height,
            int color,
            Font.DisplayMode displayMode,
            int lightCoords) {
        // 同上。
    }

    @Override
    public void submitFlame(
            PoseStack poseStack,
            EntityRenderState renderState,
            Quaternionf rotation) {
        // 着火火焰层：屏幕空间 billboard，不参与 RT。
    }

    @Override
    public void submitLeash(
            PoseStack poseStack,
            EntityRenderState.LeashState leashState) {
        // 拴绳：细线，后续如需可展开成 ribbon。
    }

    @Override
    public void submitMovingBlock(
            PoseStack poseStack,
            MovingBlockRenderState state,
            int outlineColor) {
        // 活塞推动中的方块。
    }

    @Override
    public void submitBlockModel(
            PoseStack poseStack,
            RenderType renderType,
            List<BlockStateModelPart> parts,
            int[] tintLayers,
            int lightCoords,
            int overlayCoords,
            int outlineColor) {
        // 方块模型（展示框里的方块、掉落物等）。
    }

    @Override
    public void submitBreakingBlockModel(
            PoseStack poseStack,
            List<BlockStateModelPart> parts,
            int progress,
            boolean afterTerrain) {
        // 破坏进度覆盖。
    }

    @Override
    public void submitShapeOutline(
            PoseStack poseStack,
            VoxelShape shape,
            RenderType renderType,
            int color,
            float width,
            boolean afterTerrain) {
        // 选中框 / 结构轮廓。
    }

    @Override
    public void submitItem(
            PoseStack poseStack,
            net.minecraft.world.item.ItemDisplayContext displayContext,
            int lightCoords,
            int overlayCoords,
            int outlineColor,
            int[] tintLayers,
            ItemQuads quads,
            ItemStackRenderState.FoilType foilType) {
        // 物品模型（含第一人称手持物、展示框里的物品）。
    }

    @Override
    public void submitCustomGeometry(
            PoseStack poseStack,
            RenderType renderType,
            SubmitNodeCollector.CustomGeometryRenderer customGeometryRenderer) {
        // 自定义几何（末地传送门、信标光柱、守卫者光束等）。
    }

    @Override
    public void submitQuadParticleGroup(QuadParticleRenderState particles) {
        // 粒子。
    }

    @Override
    public void submitGizmoPrimitives(
            DrawableGizmoPrimitives.Group group,
            CameraRenderState camera,
            boolean onTop) {
        // 调试 gizmo。
    }

    @Override
    public void submitModelPart(
            ModelPart part,
            PoseStack poseStack,
            RenderType renderType,
            int lightCoords,
            int overlayCoords,
            UvMapping uvMapping,
            int order) {
        // 26.3 第一人称手臂走 submitModelPart（state 是 Unit），这里留空 —— 该路径
        // 目前仍由 EntityRtCapture 的既有 mixin 捕获，本类尚未接管。
    }

    /** 诊断用：最近一次模型提交的 UV 映射（null = 整张贴图，不需要重映射）。 */
    public UvMapping lastUvMapping() {
        return lastUvMapping;
    }

    public int lastTintedColor() {
        return lastTintedColor;
    }
}
