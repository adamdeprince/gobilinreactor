// GLES presentation of kitty's shaped glyph atlas and screen cells.
#include "engine.h"
#include <GLES3/gl3.h>
#include <android/log.h>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace {
struct Sprite { unsigned x,y,z; std::vector<uint8_t> bytes; bool dirty=true; };
std::unordered_map<unsigned,Sprite> sprites;
GLuint program=0, texture=0, buffer=0, vao=0;
constexpr unsigned atlas_size=1024, layers=4;
GLuint shader(GLenum type,const char* source) {
    GLuint s=glCreateShader(type); glShaderSource(s,1,&source,nullptr); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) { char message[2048];glGetShaderInfoLog(s,sizeof(message),nullptr,message);__android_log_print(ANDROID_LOG_ERROR,"goblin-kitty","shader: %s",message);glDeleteShader(s);return 0; }
    return s;
}
bool setup() {
    const char* vertex=R"GL(#version 300 es
precision highp float;
layout(location=0) in vec2 cell;
layout(location=1) in vec3 sprite;
layout(location=2) in vec3 fg;
layout(location=3) in vec3 bg;
layout(location=4) in vec3 effects;
uniform vec2 viewport;
uniform vec2 cell_size;
out vec3 tex;
out vec3 foreground;
out vec3 background;
flat out vec3 flags;
out vec2 local_pos;
const vec2 corners[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
void main() {
    vec2 p=corners[gl_VertexID];
    vec2 xy=(cell+p)*cell_size;
    gl_Position=vec4(2.0*xy.x/viewport.x-1.0,1.0-2.0*xy.y/viewport.y,0,1);
    tex=vec3((sprite.xy+p)*cell_size/1024.0,sprite.z);
    foreground=fg; background=bg; flags=effects; local_pos=p;
})GL";
    const char* fragment=R"GL(#version 300 es
precision highp float;
precision highp sampler2DArray;
uniform sampler2DArray atlas;
in vec3 tex;
in vec3 foreground;
in vec3 background;
flat in vec3 flags;
in vec2 local_pos;
out vec4 color;
void main() {
    vec4 glyph=texture(atlas,tex);
    vec3 ink=flags.x>0.5 ? glyph.rgb : foreground;
    float alpha=glyph.a;
    int attrs=int(flags.z);
    if (((attrs&7)!=0 && local_pos.y>0.88 && local_pos.y<0.95) ||
        ((attrs&64)!=0 && local_pos.y>0.48 && local_pos.y<0.54)) { alpha=1.0; ink=foreground; }
    if (flags.y>0.5) alpha*=0.6;
    color=vec4(mix(background,ink,alpha),1.0);
})GL";
    GLuint vs=shader(GL_VERTEX_SHADER,vertex),fs=shader(GL_FRAGMENT_SHADER,fragment);
    if (!vs||!fs) return false;
    program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);glDeleteShader(vs);glDeleteShader(fs);
    GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok){glDeleteProgram(program);program=0;return false;}
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D_ARRAY,texture);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,GL_RGBA8,atlas_size,atlas_size,layers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenBuffers(1,&buffer);glGenVertexArrays(1,&vao);
    for(auto& entry:sprites)entry.second.dirty=true;
    return true;
}
std::array<float,3> rgb(uint32_t c) {return {float((c>>16)&255)/255,float((c>>8)&255)/255,float(c&255)/255};}
}
extern "C" void GoblinRenderReset() {program=texture=buffer=vao=0;}
extern "C" void GoblinRenderClearSprites() {sprites.clear();}
extern "C" void GoblinRenderSprite(unsigned x,unsigned y,unsigned z,const void* data,size_t size) {
    if(x>=atlas_size||y>=atlas_size||z>=layers||size>65536)return;
    const unsigned key=(z<<20)|(y<<10)|x;
    auto& s=sprites[key];s.x=x;s.y=y;s.z=z;s.dirty=true;
    // kitty emits uint32 RGBA words for desktop GL_UNSIGNED_INT_8_8_8_8.
    // GLES upload uses bytes, so explicitly serialize R,G,B,A on little endian.
    auto pixels=static_cast<const uint32_t*>(data);s.bytes.resize(size);
    for(size_t i=0;i<size/4;i++) {
        uint32_t p=pixels[i];s.bytes[4*i]=p>>24;s.bytes[4*i+1]=p>>16;s.bytes[4*i+2]=p>>8;s.bytes[4*i+3]=p;
    }
}
extern "C" void GoblinRenderFrame(const GoblinCell* cells,unsigned rows,unsigned columns,
        unsigned cw,unsigned ch,unsigned ax,unsigned ay,int cursor_x,int cursor_y,int width,int height) {
    if(!program&&!setup())return;
    glViewport(0,0,width,height);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D_ARRAY,texture);
    for(auto& entry:sprites) {
        auto& s=entry.second;
        if(s.dirty&&s.bytes.size()==cw*ch*4&&(s.x+1)*cw<=atlas_size&&(s.y+1)*ch<=atlas_size) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,s.x*cw,s.y*ch,s.z,cw,ch,1,GL_RGBA,GL_UNSIGNED_BYTE,s.bytes.data());s.dirty=false;
        }
    }
    constexpr unsigned stride=14;
    std::vector<float> data;data.reserve(rows*columns*stride);
    for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<columns;x++) {
        const auto& c=cells[y*columns+x];uint32_t sp=c.sprite&0x7fffffff;
        unsigned z=sp/(ax*ay),sy=(sp%(ax*ay))/ax,sx=sp%ax;
        auto fg=rgb(c.foreground),bg=rgb(c.background);
        if(int(x)==cursor_x&&int(y)==cursor_y)std::swap(fg,bg);
        data.insert(data.end(),{float(x),float(y),float(sx),float(sy),float(z),fg[0],fg[1],fg[2],bg[0],bg[1],bg[2],float(c.sprite>>31),float((c.attributes>>7)&1),float(c.attributes)});
    }
    glUseProgram(program);glUniform2f(glGetUniformLocation(program,"viewport"),width,height);glUniform2f(glGetUniformLocation(program,"cell_size"),cw,ch);
    glUniform1i(glGetUniformLocation(program,"atlas"),0);
    glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,buffer);glBufferData(GL_ARRAY_BUFFER,data.size()*sizeof(float),data.data(),GL_STREAM_DRAW);
    const unsigned sizes[]={2,3,3,3,3};unsigned offset=0;
    for(unsigned i=0;i<5;i++){glEnableVertexAttribArray(i);glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,stride*sizeof(float),reinterpret_cast<void*>(offset*sizeof(float)));glVertexAttribDivisor(i,1);offset+=sizes[i];}
    glDrawArraysInstanced(GL_TRIANGLES,0,6,rows*columns);
}
