$input a_position, a_normal, a_texcoord0, a_color0
$output v_color, v_texcoord, v_normal

#include <bgfx_shader.sh>

void main() {
    gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
    v_color = a_color0;
    v_texcoord = a_texcoord0;
    v_normal = a_normal;
}
