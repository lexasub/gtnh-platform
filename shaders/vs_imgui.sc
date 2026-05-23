$input a_position, a_texcoord0, a_color0
$output v_texcoord, v_color

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_viewProj, vec4(a_position.xy, 0.0, 1.0));
    v_texcoord = a_texcoord0;
    v_color = a_color0;
}