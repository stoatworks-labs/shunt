#pragma once

/**
    The GLSL, as plain strings.

    ## Two passes, and no framebuffer anywhere

    1. **Background.** One attributeless quad. For the source that is the
       background colour; for the effect it is the incoming clip, scaled by a
       gain that lets Reveal and Colourise fade the untouched clip back in as
       the effect is mixed out.
    2. **Shapes.** One instanced quad per shape in the train, sized by the
       shape's own bound and placed by the `Xform` uniform array that
       `Queue.cpp` filled. With a drop shadow on there are two instances per
       shape — its shadow, then itself — so each shadow lands on the older
       shapes and never on its own. `Cell` is each shape's rectangle of the
       Image, sampled on texture unit 1.

    Every mode — including the effect's Reveal and Hide, which look like they
    need a mask buffer — is reachable with those two passes and a blend
    function. Reaching for an FBO is the obvious move and walks straight into
    two SDK bugs; see AGENTS.md.

    ## Why an instanced quad rather than one fullscreen pass

    A fullscreen pass would evaluate every distance function at every pixel and
    then decide it was nowhere near any of them. Sixty-four small quads
    rasterise only the pixels that can possibly be covered, and the shapes in
    this plugin are deliberately small — a pixel-map mark is a few thousandths
    of the frame across.

    ## The array size is written out as 64

    Because the shader is a plain string literal and there is no substitution
    step. `Shunt.cpp` carries a `static_assert` that `kMaxShapes` still equals
    64, so raising the C++ constant without raising the GLSL one is a build
    error rather than a uniform-array overrun.

    The distance functions, the outline, the feather and the shading are carried
    over from the same repo's orrery, which is where they were written — see
    ATTRIBUTIONS.md for the derivations they in turn come from.
*/
namespace shunt
{
extern const char* const kBackgroundVertexShader;
extern const char* const kBackgroundFragmentShader;

extern const char* const kShapeVertexShader;
extern const char* const kShapeFragmentShader;

/// Spliced in after the `#version` line for the effect build. The source build
/// compiles the same strings without it.
extern const char* const kEffectDefine;

} // namespace shunt
