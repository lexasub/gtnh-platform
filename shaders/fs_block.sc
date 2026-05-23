$input v_color, v_texcoord, v_normal

#include <bgfx_shader.sh>

SAMPLER2D(s_texAtlas, 0);

void main() {
    gl_FragColor = texture2D(s_texAtlas, v_texcoord) * v_color;
}
