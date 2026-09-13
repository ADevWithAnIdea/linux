// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
/* Focused Mesa feature workloads; reuse the portable MIT-licensed GLES setup.
 * Build with -I/path/to/drm-shim-suite/src and the existing EGL/GLES libraries.
 * No shader binaries, GPU registers, or firmware structures are embedded. */
#define main portable_suite_main
#include "gl.c"
#undef main

static void sampled(void) {
    struct target t=target(17,9,GL_RGBA8,1);
    const uint8_t texels[2][4]={{64,32,16,128},{32,64,96,127}};
    GLuint textures[2], samplers[2];
    glGenTextures(2,textures);glGenSamplers(2,samplers);
    for(unsigned i=0;i<2;i++) {
        glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,textures[i]);
        glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,1,1);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,texels[i]);
        glSamplerParameteri(samplers[i],GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glSamplerParameteri(samplers[i],GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glBindSampler(i,samplers[i]);
    }
    GLuint p=program(
        "#version 310 es\nprecision highp float; uniform highp sampler2D s0;out vec4 v;"
        "void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
        "gl_Position=vec4(p*2.-1.,0.,1.);v=textureLod(s0,vec2(0.5),0.);}",
        "#version 310 es\nprecision highp float;uniform highp sampler2D s1;in vec4 v;"
        "layout(location=0)out vec4 o;void main(){o=v+textureLod(s1,vec2(0.5),0.);}",NULL);
    glUseProgram(p);glUniform1i(glGetUniformLocation(p,"s0"),0);glUniform1i(glGetUniformLocation(p,"s1"),1);
    const uint8_t rgba[4]={96,96,112,255};uint8_t *expected=solid_expected(t.w,t.h,rgba);
    for(unsigned i=0;i<2;i++){glDrawArrays(GL_TRIANGLES,0,3);pixels(t,expected,1);}
    puts("MESA_FEATURE_PASS vertex_fragment_sampling repeats=2");
    if(getenv("M4_SKIP_COMPUTE_SAMPLING")) {
        glDeleteProgram(p);glDeleteSamplers(2,samplers);glDeleteTextures(2,textures);free(expected);delete_target(t);return;
    }
    GLuint cp=program(NULL,NULL,
        "#version 310 es\nprecision highp float;precision highp int;layout(local_size_x=1)in;"
        "uniform highp sampler2D s;layout(std430,binding=0)buffer O{uvec4 value[];};"
        "void main(){value[gl_GlobalInvocationID.x]=uvec4(round(textureLod(s,vec2(.5),0.)*255.));}");
    uint32_t guard[24];for(unsigned i=0;i<24;i++)guard[i]=0xa5a5a5a5;
    GLuint out=buffer(guard,24);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,out);
    glUseProgram(cp);glUniform1i(glGetUniformLocation(cp,"s"),1);
    for(unsigned pass=0;pass<2;pass++) {
        glDispatchCompute(4,1,1);glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,out);
        uint32_t *data=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,sizeof(guard),GL_MAP_READ_BIT);
        require(data!=NULL,"sampled compute readback");
        for(unsigned i=0;i<24;i++)require(data[i]==(i<16?texels[1][i%4]:0xa5a5a5a5),"sampled compute value and guard");
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    gl_ok("sampled shaders");puts("MESA_FEATURE_PASS compute_sampling repeats=2 guards=8");
    glDeleteBuffers(1,&out);glDeleteProgram(cp);glDeleteProgram(p);
    glDeleteSamplers(2,samplers);glDeleteTextures(2,textures);free(expected);delete_target(t);
}

static void depth_draw(int bias) {
    /* Keep the color target below the compression threshold while the
     * larger ZLS attachment is compressible. GLES permits unequal attachment
     * dimensions; rendering is bounded by their minimum dimensions. */
    struct target t=target(bias?33:17,bias?17:9,GL_RGBA8,1);
    GLuint depth;glGenTextures(1,&depth);glBindTexture(GL_TEXTURE_2D,depth);
    glTexStorage2D(GL_TEXTURE_2D,1,bias?GL_DEPTH_COMPONENT16:GL_DEPTH32F_STENCIL8,33,17);
    glFramebufferTexture2D(GL_FRAMEBUFFER,bias?GL_DEPTH_ATTACHMENT:GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,depth,0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"depth framebuffer");
    GLuint p=program(vertex_source,fragment_source,NULL);glEnable(GL_DEPTH_TEST);
    if(!bias){glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,0x35,0xff);glStencilOp(GL_REPLACE,GL_REPLACE,GL_REPLACE);}
    const uint8_t green[4]={0,255,0,255};uint8_t *expected=solid_expected(t.w,t.h,green);
    for(unsigned pass=0;pass<2;pass++) {
        glDepthFunc(GL_ALWAYS);
        if(bias){glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(0,100);}
        color(p,1,0,0,1,bias?0:.5);glDrawArrays(GL_TRIANGLES,0,3);glFinish();
        glDisable(GL_POLYGON_OFFSET_FILL);glDepthFunc(GL_LESS);
        if(!bias){glStencilFunc(GL_EQUAL,0x35,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);}
        color(p,0,1,0,1,0);glDrawArrays(GL_TRIANGLES,0,3);
        color(p,0,0,1,1,.8);glDrawArrays(GL_TRIANGLES,0,3);
        pixels(t,expected,0);
    }
    gl_ok("depth draw without clear");
    printf("MESA_FEATURE_PASS %s repeats=2\n",bias?"integer_depth_bias":"compressed_depth_stencil");
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDeleteTextures(1,&depth);glDeleteProgram(p);free(expected);delete_target(t);
}

static void layered(void) {
    require(extension("GL_EXT_geometry_shader")||extension("GL_OES_geometry_shader"),"layered framebuffer API advertised");
    void (*attach)(GLenum,GLenum,GLuint,GLint)=(void*)eglGetProcAddress("glFramebufferTextureEXT");
    require(attach!=NULL,"layered framebuffer entry point");
    GLuint fbo,tex,depth;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D_ARRAY,tex);glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,GL_RGBA8,17,9,2);
    glGenTextures(1,&depth);glBindTexture(GL_TEXTURE_2D_ARRAY,depth);glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,GL_DEPTH24_STENCIL8,17,9,2);
    attach(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tex,0);attach(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,depth,0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"layered framebuffer complete");glViewport(0,0,17,9);
    const uint8_t rgba[4]={255,0,255,255};uint8_t *expected=solid_expected(17,9,rgba);
    for(unsigned pass=0;pass<2;pass++) {
        attach(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tex,0);attach(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,depth,0);
        glClearColor(1,0,1,1);glClearDepthf(.75);glClearStencil(0x27);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glFinish();
        for(unsigned layer=0;layer<2;layer++) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tex,0,layer);
            glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,depth,0,layer);
            pixels((struct target){.fbo=fbo,.w=17,.h=9},expected,0);
        }
    }
    gl_ok("layered clear");puts("MESA_FEATURE_PASS layered_color_depth_stencil repeats=2 layers=2");
    free(expected);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);glDeleteTextures(1,&depth);
}
static void partial_accumulate(void) {
    /* R32F is supported by the existing Apple9 tile compiler. The portable
     * oracle requires every pixel in two targets to equal 200000 exactly. */
    pressure_case(200000,2,33,7);
    puts("MESA_FEATURE_PASS partial_accumulation triangles=200000 targets=2");
}

static void single_page(void) {
    void (*arm)(void)=(void*)dlsym(RTLD_DEFAULT,"m4_single_page_arm");
    int (*ready)(void)=(void*)dlsym(RTLD_DEFAULT,"m4_single_page_ready");
    require(arm && ready,"single page public UAPI probe loaded");
    GLuint cp=program(NULL,NULL,
        "#version 310 es\nprecision highp int;layout(local_size_x=1)in;"
        "layout(std430,binding=0)readonly buffer I{uint a[];};"
        "layout(std430,binding=1)buffer O{uint b[];};"
        "void main(){uint i=gl_GlobalInvocationID.x;b[i]=a[i*4096u+13u];}");
    uint32_t input[0x3000],output[8];
    for(unsigned i=0;i<0x3000;i++)input[i]=i+0x100;
    for(unsigned i=0;i<8;i++)output[i]=0xa5a5a5a5;
    arm();GLuint src=buffer(input,0x3000);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,src);
    GLuint dst=buffer(output,8);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,dst);glUseProgram(cp);
    for(unsigned pass=0;pass<2;pass++) {
        glDispatchCompute(3,1,1);glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        require(ready(),"single-page buffer allocation observed");
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,dst);
        uint32_t *data=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,sizeof(output),GL_MAP_READ_BIT);
        require(data!=NULL,"single-page readback");
        for(unsigned i=0;i<8;i++)require(data[i]==(i<3?0x10d:0xa5a5a5a5),"three GPU aliases and output guards");
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    gl_ok("single page alias execution");puts("MESA_FEATURE_PASS single_page_gpu_alias pages=3 repeats=2 guards=5");
    glDeleteProgram(cp);glDeleteBuffers(1,&src);glDeleteBuffers(1,&dst);
}

static void layered_uapi(void) {
    GLuint fbo,tex,depth;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D_ARRAY,tex);glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,GL_RGBA8,17,9,2);
    uint8_t black[17*9*2*4];memset(black,0,sizeof(black));
    for(unsigned i=3;i<sizeof(black);i+=4)black[i]=255;
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,0,17,9,2,GL_RGBA,GL_UNSIGNED_BYTE,black);
    glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tex,0,0);
    glGenTextures(1,&depth);glBindTexture(GL_TEXTURE_2D_ARRAY,depth);glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,GL_DEPTH_COMPONENT16,17,9,2);
    glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,depth,0,0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"two-layer backing framebuffer");glViewport(0,0,17,9);
    GLuint p=program(vertex_source,fragment_source,NULL);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);
    color(p,1,0,1,1,0);glDrawArrays(GL_TRIANGLES,0,3);glFinish();
    /* The interposer declares two layers for the layer-zero VDM stream;
     * layer one has no geometry. Later readback submits retain Mesa's values. */
    unsetenv("M4_UAPI_LAYERS");glDisable(GL_DEPTH_TEST);
    const uint8_t magenta[4]={255,0,255,255};uint8_t *expected=solid_expected(17,9,magenta);
    pixels((struct target){.fbo=fbo,.w=17,.h=9},expected,0);
    glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tex,0,1);
    glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,depth,0,1);
    pixels((struct target){.fbo=fbo,.w=17,.h=9},black,0);
    gl_ok("two-layer UAPI with empty second layer");puts("MESA_FEATURE_PASS layered_uapi layers=2 layer1_guard=153_pixels");
    free(expected);glDeleteProgram(p);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);glDeleteTextures(1,&depth);
}

int main(int argc,char **argv) {
    require(argc==2,"one feature name required");setup();
    if(!strcmp(argv[1],"sampling"))sampled();
    else if(!strcmp(argv[1],"bias"))depth_draw(1);
    else if(!strcmp(argv[1],"compressed"))depth_draw(0);
    else if(!strcmp(argv[1],"layered"))layered();
    else if(!strcmp(argv[1],"partial"))partial_accumulate();
    else if(!strcmp(argv[1],"single-page"))single_page();
    else if(!strcmp(argv[1],"layered-uapi"))layered_uapi();
    else fail("unknown feature");
    glFinish();gl_ok("feature completion");
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
    printf("MESA_FEATURES_PASS checks=%llu\n",(unsigned long long)checks);return 0;
}
