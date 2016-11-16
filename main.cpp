#include <iostream>
#include <functional>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#define ReadPixel(buf,w,x,y) (buf)[(uint32_t)(w)*(uint32_t)(y)+(uint32_t)(x)]

#define OUT

using namespace std;
using namespace glm;

SDL_Renderer* ren;

struct Vertex
{
  vec4 position;
  vec4 color;
  vec2 uv;
  vec4 normal;
};

Vertex VertexLerp(const Vertex& a,const Vertex& b,float t)
{
  Vertex r;
  r.position=lerp(a.position,b.position,t);
  r.normal=lerp(a.normal,b.normal,t);
  r.color=lerp(a.color,b.color,t);
  r.uv=lerp(a.uv,b.uv,t);
  return r;
}

class Device
{
public:
  using VertexShader=function<Vertex(const Vertex&)>;
  using FragmentShader=function<vec4(const Vertex&)>;

  Device(int32_t w,int32_t h):m_w(w),m_h(h)
  {
    m_depth_buffer=new float[m_w*m_h];
    m_render_buffer=new uint32_t[m_w*m_h];
    Clear();
  }

  ~Device()
  {
    delete[] m_render_buffer;
    delete[] m_depth_buffer;
  }

  uint32_t* GetRenderTarget()
  {return m_render_buffer;}

  void SetShader(const VertexShader& vertex_shader,const FragmentShader& fragment_shader)
  {
    m_vertex_shader=vertex_shader;
    m_fragment_shader=fragment_shader;
  }

  static vec4 TexturePixel(void* buf,uint32_t w,uint32_t h,vec2 uv)
  {
    const float inv=1.0f/255.0f;
    vec4 color;
    uint32_t color_uint=ReadPixel((uint32_t*)buf,w,(w-1)*uv.x,(h-1)*uv.y);
    color.a=(color_uint&0xff)*inv;
    color.b=((color_uint&0xff00)>>8)*inv;
    color.g=((color_uint&0xff0000)>>16)*inv;
    color.r=((color_uint&0xff000000)>>24)*inv;
    return color;
  }

  void Draw(vector<Vertex>& vert,uint32_t offset,uint32_t size)
  {
    for(int i=offset;i<size;i+=3)
      DrawTriangle(vert[i],vert[i+1],vert[i+2]);
  }

  void Draw(vector<Vertex>& vert,vector<uint32_t> index,uint32_t offset,uint32_t size)
  {
    for(int i=offset;i<size;i+=3)
      DrawTriangle(vert[index[i]],vert[index[i+1]],vert[index[i+2]]);
  }

  void Clear()
  {
    memset(m_render_buffer,0,(m_w*m_h)<<2);
    for(int i=0;i<m_w*m_h;++i)m_depth_buffer[i]=1.0f;
  }

private:

  struct Scanline
  {
    Scanline(int32_t _x,int32_t _y,int32_t _w,Vertex& _vx1,Vertex& _vx2,float _step):x(_x),y(_y),w(_w),
    vx1(_vx1),vx2(_vx2),step(_step){}
    int32_t x,y,w;//屏幕空间
    Vertex vx1,vx2;
    float step;//标准裁剪正方体空间
  };

  void DrawTriangle(Vertex& v1,Vertex& v2,Vertex& v3)
  {
    Vertex vert[4];
    if(!m_vertex_shader)throw runtime_error("NO VERTEX SHADER!");
    if(!m_fragment_shader)throw runtime_error("NO FRAGMENT SHADER!");
    vert[0]=m_vertex_shader(v1);
    vert[1]=m_vertex_shader(v2);
    vert[2]=m_vertex_shader(v3);

    for(Vertex& v:vert)if(CheckCVV(v))return;

    for(Vertex& v:vert)//归一化(标准裁剪正方体)
    {
      float rhw=1.0f/v.position.w;
      v.position.x*=rhw;
      v.position.y*=rhw;
      v.position.z*=rhw;
    }

    //Scanline
    int n=SplitTriangle(vert[0],vert[1],vert[2],vert[3]);
    ivec4 v_screen[4];
    for(Vertex& v:vert)
    {
      v_screen[&v-vert]=ToScreenSpace(v.position);
    }

    if(n==1)
    {
      DrawTriangle2(v_screen[0],v_screen[1],v_screen[2],vert[0],vert[1],vert[2]);
    }else if(n==2)
    {//高->低
      DrawTriangle2(v_screen[0],v_screen[1],v_screen[3],vert[0],vert[1],vert[3]);
      DrawTriangle2(v_screen[3],v_screen[1],v_screen[2],vert[3],vert[1],vert[2]);
    }
  }

  void DrawTriangle2(ivec4& iv1,ivec4& iv2,ivec4& iv3,Vertex& v1,Vertex& v2,Vertex& v3)
  {
    for(uint32_t y=iv3.y;y<=iv1.y;++y)
    {
      Scanline scanline=GenScanline(iv1,iv2,iv3,v1,v2,v3,y);
      float t=0.0f;
      for(uint32_t x=scanline.x;x<=scanline.x+scanline.w;++x)
      {
        Vertex cv(VertexLerp(scanline.vx1,scanline.vx2,t));
        t+=scanline.step;
        if(ReadPixel(m_depth_buffer,m_w,x,y)>cv.position.z)
        {
          vec4 fcolor(m_fragment_shader(cv));
          uint32_t color=0;
          color|=(uint32_t)(fcolor.a*255.0f);
          color|=((uint32_t)(fcolor.b*255.0f))<<8;
          color|=((uint32_t)(fcolor.g*255.0f))<<16;
          color|=((uint32_t)(fcolor.r*255.0f))<<24;
          ReadPixel(m_render_buffer,m_w,x,y)=color;
          ReadPixel(m_depth_buffer,m_w,x,y)=cv.position.z;
        }
      }
    }
  }

  int32_t CheckCVV(Vertex& v)
  {
    int32_t r=0;
    float w=v.position.w;
    r|=v.position.x<-w?1:0;
    r|=v.position.x> w?2:0;
    r|=v.position.y<-w?4:0;
    r|=v.position.y> w?8:0;
    r|=v.position.z<-w?16:0;
    r|=v.position.z> w?32:0;
    return r;
  }

  int SplitTriangle(Vertex& v1,Vertex& v2,Vertex& v3,OUT Vertex& v4)
  {
    if(v1.position.y<v2.position.y)swap(v1,v2);
    if(v1.position.y<v3.position.y)swap(v1,v3);
    if(v2.position.y<v3.position.y)swap(v2,v3);
    if(vec2(v1.position)==vec2(v2.position)||
       vec2(v1.position)==vec2(v3.position)||
       vec2(v2.position)==vec2(v3.position))return 0;
    if(v2.position.y==v3.position.y||
       v2.position.y==v1.position.y||
       v3.position.y==v1.position.y)return 1;

    float u=(v1.position.y-v2.position.y)/(v1.position.y-v3.position.y);
    v4=VertexLerp(v1,v3,u);
    return 2;
  }

  ivec4 ToScreenSpace(const vec4& v)
  {
    ivec4 r;
    r.x=v.x*0.5*m_w+(0.5*m_w);
    r.y=v.y*0.5*m_h+(0.5*m_h);
    r.z=v.z*0.5+0.5;
    return r;
  }

  Scanline GenScanline(ivec4& iv1,ivec4& iv2,ivec4& iv3,Vertex& v1,Vertex& v2,Vertex& v3,int32_t y)
  {
    float u=0.0f;
    int32_t x1=0;;
    int32_t x2=0;
    Vertex vx1;
    Vertex vx2;
    if(iv1.y==iv2.y)
    {
      u=((float)(y-iv3.y))/(iv1.y-iv3.y);
      x1=iv3.x+(iv1.x-iv3.x)*u;
      x2=iv3.x+(iv2.x-iv3.x)*u;
      vx1=VertexLerp(v3,v1,u);
      vx2=VertexLerp(v3,v2,u);
    }
    else
    {
      u=((float)(iv1.y-y))/(iv1.y-iv3.y);
      x1=iv1.x+(iv3.x-iv1.x)*u;
      x2=iv1.x+(iv2.x-iv1.x)*u;
      vx1=VertexLerp(v1,v3,u);
      vx2=VertexLerp(v1,v2,u);
    }
    int32_t w=x2-x1;
    float step=1.0f/w;
    if(x1>x2)
    {
      swap(x1,x2);
      swap(vx1,vx2);
      w=-w;
      step=-step;
    }
    return Scanline(x1,y,w,vx1,vx2,step);
  }

private:
  VertexShader m_vertex_shader;
  FragmentShader m_fragment_shader;
  int32_t m_w,m_h;
  uint32_t* m_render_buffer;
  float* m_depth_buffer;
};

#undef main

#define W 800
#define H 600

vector<Vertex> vertex;
vector<uint32_t> indices(36);
void init_vertex()
{
  vertex.push_back(Vertex{vec4(-1.0f, -1.0f, -1.0f,1.0f),vec4(0.0f, 0.0f, 0.0f,1.0f),vec2(0.0f,1.0f)});
  vertex.push_back(Vertex{vec4(-1.0f, 1.0f, -1.0f,1.0f),vec4(0.0f, 1.0f, 0.0f,1.0f),vec2(0.0f,0.0f)});
  vertex.push_back(Vertex{vec4(1.0f, 1.0f, -1.0f,1.0f),vec4(1.0f, 1.0f, 0.0f,1.0f),vec2(1.0f,0.0f)});
  vertex.push_back(Vertex{vec4(1.0f, -1.0f, -1.0f,1.0f),vec4(1.0f, 0.0f, 0.0f,1.0f),vec2(1.0f,1.0f)});
  vertex.push_back(Vertex{vec4(-1.0f, -1.0f, 1.0f,1.0f),vec4(0.0f, 0.0f, 1.0f,1.0f),vec2(0,0)});
  vertex.push_back(Vertex{vec4(-1.0f, 1.0f, 1.0f,1.0f),vec4(0.0f, 1.0f, 1.0f,1.0f),vec2(0,0)});
  vertex.push_back(Vertex{vec4(1.0f, 1.0f, 1.0f,1.0f),vec4(1.0f, 1.0f, 1.0f,1.0f),vec2(0,0)});
  vertex.push_back(Vertex{vec4(1.0f, -1.0f, 1.0f,1.0f),vec4(1.0f,0.0f, 1.0f,1.0f),vec2(0,0)});

  // front side
  indices[0] = 0; indices[1] = 1; indices[2] = 2;
  indices[3] = 0; indices[4] = 2; indices[5] = 3;

  // back side
  indices[6] = 4; indices[7] = 6; indices[8] = 5;
  indices[9] = 4; indices[10] = 7; indices[11] = 6;

  // left side
  indices[12] = 4; indices[13] = 5; indices[14] = 1;
  indices[15] = 4; indices[16] = 1; indices[17] = 0;

  // right side
  indices[18] = 3; indices[19] = 2; indices[20] = 6;
  indices[21] = 3; indices[22] = 6; indices[23] = 7;

  // top
  indices[24] = 1; indices[25] = 5; indices[26] = 6;
  indices[27] = 1; indices[28] = 6; indices[29] = 2;

  // bottom
  indices[30] = 4; indices[31] = 0; indices[32] = 3;
  indices[33] = 4; indices[34] = 3; indices[35] = 7;
}

int main(int argc, char *argv[])
{
  init_vertex();
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window* win=SDL_CreateWindow("Soft3D",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,800,600,SDL_WINDOW_SHOWN);
  ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  SDL_Texture* tex=SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_STREAMING,W,H);

  Device device(W,H);

  mat4 p=perspective(45.0f,(float)W/(float)H,0.1f,1000.0f);
  mat4 v=lookAt(vec3(0.0f,2.0f,10.0f),vec3(0.0f,0.0f,0.0f),vec3(1.0f,1.0f,0.0f));
  mat4 m;
  //glm::translate(m,vec3(0,0,0));

  SDL_Surface* tmp=IMG_Load(u8"F:\\OneDrive\\pixiv\\pixiv3076817.jpg");
  SDL_Surface* test_tex=SDL_ConvertSurfaceFormat(tmp,SDL_PIXELFORMAT_RGBA8888,0);
  SDL_FreeSurface(tmp);

  device.SetShader(
  [&p,&v,&m](const Vertex& vert)
  {
    Vertex rv=vert;
    rv.position=p*v*m*vert.position;
    return rv;
  },
  [&test_tex](const Vertex& vc)
  {
    return Device::TexturePixel(test_tex->pixels,test_tex->w,test_tex->h,vc.uv);
    //return vc.color;
  }
  );

  bool quit=false;
  SDL_Event ev;
  while(!quit)
  {
    while(SDL_PollEvent(&ev))
    {
      if(ev.type==SDL_QUIT)quit=true;
    }
    SDL_RenderClear(ren);
    device.Clear();
    device.Draw(vertex,indices,0,indices.size());
    SDL_UpdateTexture(tex,nullptr,device.GetRenderTarget(),W<<2);
    m=glm::rotate(m,0.1f/glm::pi<float>(),vec3(0.0f,1.0f,0.0f));
    SDL_RenderCopyEx(ren,tex,nullptr,nullptr,0.0,nullptr,SDL_FLIP_VERTICAL);
    SDL_RenderPresent(ren);
  }

  SDL_FreeSurface(test_tex);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
