// Same half publications, display proxy and five-tap history reconstruction as the upstream demo.
float roundF16(float v) {
  uint b=floatBitsToUint(v), s=b&0x80000000u, m=b&0x7fffffffu;
  if(m>=0x7f800000u)return v;
  if(m>=0x477ff000u)return uintBitsToFloat(s|0x7f800000u);
  if(m<0x38800000u){float q=roundEven(uintBitsToFloat(m)*16777216.0);return uintBitsToFloat(s|floatBitsToUint(q*0.000000059604644775390625));}
  return uintBitsToFloat(s|((m+0xfffu+((m>>13u)&1u))&~0x1fffu));
}
float truncateHalf(float v) {
  uint b=floatBitsToUint(v),s=(b>>16u)&0x8000u,e=(b>>23u)&255u,m=b&0x7fffffu,h;
  int x=int(e)-112;
  if(e==255u)h=s|(m!=0u?0x7e00u:0x7c00u);
  else if(x>=31)h=s|0x7c00u;
  else if(x<=0)h=x < -10?s:s|((m|0x800000u)>>uint(14-x));
  else h=s|(uint(x)<<10u)|(m>>13u);
  return unpackHalf2x16(h).x;
}
float srgbEncode(float v){v=clamp(v,0,1);return v<=.0031308?12.92*v:1.055*pow(v,1.0/2.4)-.055;}
float srgbDecode(float v){v=clamp(v,0,1);return v<=.04045?v/12.92:pow((v+.055)/1.055,2.4);}
float finiteNonnegative(float v){return isnan(v)||isinf(v)?0:max(v,0);}
float proxyComponent(float v,float paper){v=finiteNonnegative(v)/max(paper,.05);if(v>.75)v=.75+.25*(1-exp(-5.770780*(v-.75)));return roundF16(srgbEncode(v));}
layout(push_constant) uniform FrameParams {
  uint fullWidth,fullHeight,width,height;
  uint seed,historyValid,autoMask,style;
  float tone,structure,skin,paperWhite;
  float intensity,blendScale,historyStrength,enabled;
  float colorStrength,maxRatio;
} params;
layout(std430,binding=0) readonly buffer Source {vec4 source[];};
layout(std430,binding=1) readonly buffer History {vec4 previousHistory[];};
vec3 proxyAt(uvec2 id) {
  float e=source[params.width*params.height*2u].x;
  vec3 c=source[(id.y*params.width+id.x)*2u].rgb*e;
  return vec3(proxyComponent(c.r,params.paperWhite),proxyComponent(c.g,params.paperWhite),proxyComponent(c.b,params.paperWhite));
}
vec3 historyAt(ivec2 p){p=clamp(p,ivec2(0),ivec2(params.width,params.height)-1);return previousHistory[p.y*int(params.width)+p.x].rgb;}
vec3 bilinearHistory(vec2 p){p=clamp(p,vec2(.5),vec2(params.width,params.height)-.5)-.5;ivec2 b=ivec2(floor(p));vec2 f=fract(p);return mix(mix(historyAt(b),historyAt(b+ivec2(1,0)),f.x),mix(historyAt(b+ivec2(0,1)),historyAt(b+ivec2(1,1)),f.x),f.y);}
bool hasHistory(uvec2 p){return params.historyValid!=0u&&source[(p.y*params.width+p.x)*2u+1u].z>.5;}
vec3 historySample(uvec2 p){
  vec2 position=vec2(p)+.5+source[(p.y*params.width+p.x)*2u+1u].xy*vec2(params.width,params.height);
  vec2 base=floor(position-.5)+.5,f=clamp(position-base,0,1),sq=f*f,cube=f*sq;
  vec2 w0=fma(f+cube,vec2(-.5),sq),w1=(cube*1.5-sq*2.5)+1,w3=(cube-sq)*.5,w2=((1-w0)-w1)-w3,mid=w1+w2;
  vec2 low=base-1,center=base+w2/mid,high=base+2;
  float a=w0.x*mid.y,b=w0.y*mid.x,c=mid.x*mid.y,d=w3.y*mid.x,e=w3.x*mid.y;
  vec3 v=fma(bilinearHistory(vec2(low.x,center.y)),vec3(a),bilinearHistory(vec2(center.x,low.y))*b);
  v=fma(bilinearHistory(center),vec3(c),v);v=fma(bilinearHistory(vec2(center.x,high.y)),vec3(d),v);
  return fma(bilinearHistory(vec2(high.x,center.y)),vec3(e),v)*(1.0/(e+(d+(c+(a+b)))));
}
