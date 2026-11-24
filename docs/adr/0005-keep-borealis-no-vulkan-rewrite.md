# Preserve Borealis/NanoVG, optimize OunceVK/Zink first, and keep native Vulkan evaluation gated

The port preserves Borealis/NanoVG, upstream XML layouts, resources, focus behavior, and MPVCore rather than rewriting the application UI from scratch or replacing the player framework. Its canonical initial production rendering stack is Vulkan-backed:

`Borealis / NanoVG + libmpv -> SDL2 / EGL / OpenGL ES -> OunceVK Zink -> NVK Vulkan -> Switch 2 GPU`

We optimize copies, texture uploads, synchronization, thumbnail loading, danmaku batching, and presentation frame scheduling on this stack first. Preserving Borealis and keeping rendering distinct from libounce VideoDecoder hardware decoding does not categorically prohibit evaluating a native Vulkan backend under existing Borealis/NanoVG and libmpv/libplacebo seams: native Vulkan is evaluated on an isolated post-G2 research branch against the optimized Zink baseline and adopted only if measured gains justify its maintenance cost without permanently maintaining competing production renderers or a large private libmpv fork.
