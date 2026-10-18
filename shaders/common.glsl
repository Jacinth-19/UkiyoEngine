// ukiyo: shared backend shims and push constants. Included by every shader.
// Descriptor sets (fixed numbering, must match src/rhi/*):
//   set 0 = Frame  : UBO 0 (FrameBlock), tex 1 (cascaded shadow atlas)
//   set 0 = Post   : tex 1, tex 2                     (post-processing passes)
//   set 1 = Material: tex 4 base, tex 5 shade-multiply, tex 6 emissive, UBO 8
//   set 2 = Skin   : UBO 9 (joint matrices)
#ifdef BACKEND_VK
#define UBO(s, b) layout(std140, set = s, binding = b) uniform
#define TEX(s, b) layout(set = s, binding = b)
#define PUSH layout(push_constant) uniform
#define VERTEX_ID gl_VertexIndex
#define INSTANCE_ID gl_InstanceIndex
#else
#define UBO(s, b) layout(std140, binding = b) uniform
#define TEX(s, b) layout(binding = b)
#define PUSH layout(std140, binding = 1) uniform
#define VERTEX_ID gl_VertexID
#define INSTANCE_ID gl_InstanceID
#endif

// Per-draw / per-pass constants (<= 128 bytes). p0/p1 meaning is per shader.
PUSH PushBlock {
    mat4 model;
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 p3;
} pc;
