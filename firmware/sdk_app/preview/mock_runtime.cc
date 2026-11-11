// Host preview: rasterize cheeko.h display calls into a 240x296 framebuffer.
#include "cheeko.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <cmath>
namespace cheeko {
static uint32_t fb[296][240];
static void px(int x,int y,uint32_t c){ if(x>=0&&x<240&&y>=0&&y<296) fb[y][x]=c; }
void Display::Clear(uint32_t c){ for(auto&r:fb) for(auto&p:r) p=c; }
void Display::FillRect(int x,int y,int w,int h,uint32_t c){ for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) px(i,j,c); }
void Display::Rect(int x,int y,int w,int h,uint32_t c){ for(int i=x;i<x+w;i++){px(i,y,c);px(i,y+h-1,c);} for(int j=y;j<y+h;j++){px(x,j,c);px(x+w-1,j,c);} }
void Display::FillCircle(int cx,int cy,int r,uint32_t c){ for(int j=-r;j<=r;j++) for(int i=-r;i<=r;i++) if(i*i+j*j<=r*r) px(cx+i,cy+j,c); }
void Display::Circle(int cx,int cy,int r,uint32_t c){ for(int j=-r-1;j<=r+1;j++) for(int i=-r-1;i<=r+1;i++){ double d=std::sqrt(i*i+j*j); if(d<=r&&d>=r-2) px(cx+i,cy+j,c);} }
void Display::Line(int x1,int y1,int x2,int y2,uint32_t c){ int n=std::max(std::abs(x2-x1),std::abs(y2-y1))+1; for(int k=0;k<=n;k++){ int x=x1+(x2-x1)*k/n, y=y1+(y2-y1)*k/n; px(x,y,c); px(x,y+1,c);} }
void Display::Text(int x,int y,const std::string& t){ FillRect(x,y+6,(int)t.size()*10,8,0x606870); }
void Display::CenterText(int y,const std::string& t){ Text((240-(int)t.size()*10)/2,y,t); }
void Display::Image(const std::string&){}
void Speaker::Play(const std::string&){} void Speaker::Tone(int,int){} void Speaker::SetVolume(int){}
bool Speaker::PlayPcm(const int16_t*,size_t,int){return true;} bool Speaker::IsPlaying(){return false;}
static std::map<std::string,int> ints;
void Storage::PutInt(const std::string&k,int v){ints[k]=v;} int Storage::GetInt(const std::string&k,int f){auto it=ints.find(k);return it==ints.end()?f:it->second;}
void Storage::PutString(const std::string&,const std::string&){} std::string Storage::GetString(const std::string&,const std::string&f){return f;}
bool Storage::Has(const std::string&){return false;} void Storage::Remove(const std::string&){}
void Log::Info(const std::string&m){fprintf(stderr,"%s\n",m.c_str());} void Log::Warn(const std::string&m){Info(m);} void Log::Error(const std::string&m){Info(m);}
Display& CheekoRuntime::display(){static Display d;return d;} Speaker& CheekoRuntime::speaker(){static Speaker s;return s;}
Storage& CheekoRuntime::storage(){static Storage s;return s;} Log& CheekoRuntime::log(){static Log l;return l;}
Cloud& CheekoRuntime::cloud(){static Cloud c;return c;}
void Cloud::Connect(){} void Cloud::StartVoiceSession(){} void Cloud::EndVoiceTurn(){} bool Cloud::IsLinked(){return true;}
CheekoRuntime& Cheeko(){static CheekoRuntime r;return r;}
void SavePpm(const char* path){ FILE*f=fopen(path,"wb"); fprintf(f,"P6 240 296 255\n"); for(auto&r:fb) for(auto p:r){unsigned char b[3]={(unsigned char)(p>>16),(unsigned char)(p>>8),(unsigned char)p}; fwrite(b,1,3,f);} fclose(f); }
}
extern "C" cheeko::CheekoApp* CreateCheekoApp();
int main(int argc,char**argv){
  using namespace cheeko;
  int colour = argc>1 ? atoi(argv[1]) : 0;
  Cheeko().storage().PutInt("duck_color", colour);
  CheekoApp* app=CreateCheekoApp();
  app->OnStart(); app->OnTick(100); SavePpm("idle.ppm");
  TouchEvent t; t.x=120; t.y=200; t.pressed=true; app->OnTick(1000); app->OnTouch(t);
  app->OnTick(1100); SavePpm("talk.ppm");
  app->OnTick(3500); SavePpm("blink.ppm");
}
