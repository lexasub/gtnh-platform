$input v_texcoord, v_color

#include <bgfx_shader.sh>

SAMPLER2D(s_texAtlas, 0);

void main()
{
    vec4 texColor = texture2D(s_texAtlas, v_texcoord);
    gl_FragColor = texColor * v_color;
}