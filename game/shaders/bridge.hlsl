// GPU-only texture/buffer conversion. The host owns these descriptors and restores game state after recording.
cbuffer Params : register(b0) {
  uint width, height, colorX, colorY;
  uint motionWidth, motionHeight, motionX, motionY;
  float motionScaleX, motionScaleY, preExposure, exposureScale;
  float jitterDeltaX, jitterDeltaY;
  uint temporal, exposureAvailable;
};
#ifdef PACK_INPUT
Texture2D<float4> colorTexture : register(t0);
Texture2D<float4> motionTexture : register(t1);
Texture2D<float4> exposureTexture : register(t2);
RWStructuredBuffer<float4> packed : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  uint p = id.y * width + id.x;
  packed[p*2] = colorTexture.Load(int3(id.xy + uint2(colorX,colorY),0));
  float2 motion = 0;
  if (temporal) {
    uint2 m = min(((id.xy*2+1)*uint2(motionWidth,motionHeight))/(uint2(width,height)*2),
                  uint2(motionWidth-1,motionHeight-1));
    motion = motionTexture.Load(int3(m+uint2(motionX,motionY),0)).xy * float2(motionScaleX,motionScaleY)
             / float2(motionWidth,motionHeight) + float2(jitterDeltaX,jitterDeltaY)/float2(width,height);
  }
  float2 previous = (float2(id.xy)+.5)/float2(width,height) + motion;
  bool valid = temporal && all(isfinite(motion)) && all(previous >= 0) && all(previous <= 1);
  packed[p*2+1] = float4(motion,valid ? 1 : 0,0);
  if (p == 0) {
    float e = exposureAvailable ? exposureTexture.Load(int3(0,0,0)).r : 1;
    e *= exposureScale/max(preExposure,1e-8);
    packed[width*height*2] = float4(isfinite(e) && e>0 ? e : 1,0,0,0);
  }
}
#else
StructuredBuffer<float4> result : register(t0);
Texture2D<float4> originalColor : register(t1);
RWTexture2D<float4> outputTexture : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x < width && id.y < height)
    outputTexture[id.xy] = result[width*height].x == 1
      ? result[id.y*width+id.x] : originalColor.Load(int3(id.xy+uint2(colorX,colorY),0));
}
#endif
