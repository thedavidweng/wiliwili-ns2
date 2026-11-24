# Keep Borealis/NanoVG and separate rendering from decoding

The port adapts the existing Borealis/NanoVG UI minimally through an SDL2 and EGL/GLES path; it does not rewrite the UI in Vulkan or replace the player framework. OunceVK is present, but a renderer rewrite would discard existing UI resources and focus behavior at the highest possible cost. Hardware decoding is evaluated independently through G3 and is never conflated with rendering.
