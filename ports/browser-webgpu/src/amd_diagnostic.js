// Test-only direct numerical kernels. The production browser and native fast
// paths do not import this module. Graph/model data and public boundary names
// stay unchanged so captures can be compared independently across graphics APIs.
import { windowPhase } from './geometry.js';

export async function exactDiagnostic({window = true, runtime = true} = {}) {
  const read = async name => {
    const response=await fetch(new URL(`../shaders/${name}`,import.meta.url));
    if(!response.ok)throw new Error(`cannot read diagnostic shader ${name}`);
    return response.text();
  };
  const [numericCode,source]=await Promise.all([read('numerics.wgsl'),read('amd_exact_gemm.wgsl')]);
  let gemmCode=source;
  if(runtime) {
    // Large K-specialized scalar loops can spend minutes in the graphics
    // driver's optimizer. Diagnostic arithmetic does not need shape folding.
    // Retain the declared overrides for the existing pipeline interface, while
    // reading dispatch data from its identical, already-recorded uniform.
    const fields={MATMUL_ROWS:'words[0].x',MATMUL_K:'words[0].y',MATMUL_N:'words[0].z',
      LAYOUT_WEIGHT_BYTE_OFFSET:'words[0].w',LAYOUT_BIAS_BYTE_OFFSET:'words[1].x',MATMUL_FLAGS:'words[1].y',
      LAYOUT_WEIGHT_MATRIX_CHANNELS:'words[1].z',LAYOUT_WEIGHT_COLUMN_OFFSET:'words[1].w',
      LAYOUT_OUTPUT_MATRIX_CHANNELS:'words[2].x',LAYOUT_OUTPUT_COLUMN_OFFSET:'words[2].y',
      LAYOUT_INPUT_MATRIX_CHANNELS:'words[2].z'};
    const start=source.indexOf('struct ExactParams');
    let body=source.slice(start);
    for(const [name,field] of Object.entries(fields))body=body.replace(new RegExp(`\\b${name}\\b`,'g'),`exact_params.${field}`);
    gemmCode=source.slice(0,start)+body;
  }
  return {
    extraShaders:window?[{path:'shaders/amd_exact_window.wgsl',entryPoints:['amd_exact_window']}]:[],
    before(network) {
      const modules=new Map();
      network.matmul.module=function(variant) {
        const key=variant.output;
        if(!modules.has(key))modules.set(key,this.device.createShaderModule({label:`diagnostic scalar F13 ${key}`,
          code:numericCode+'\n'+gemmCode.replace('__HALF_OUTPUT__',String(key==='half')).replace('__DUAL_OUTPUT__',String(key==='dual'))}));
        return modules.get(key);
      };
      if(window)network.graph.windowAttention=function({qkv,attended,prior,scales,width,height,heads,phase,label}) {
        const [shiftX,shiftY]=windowPhase(phase);
        const tasks=Math.ceil((width+shiftX)/8)*Math.ceil((height+shiftY)/8)*16;
        const params=new Uint32Array([width*height,heads,width,height,heads*32,shiftX,shiftY,1]);
        this.recorder.pass('amd_exact_window',{1:qkv.buffer,2:scales,3:prior,6:attended.buffer},params,
          [heads,Math.min(tasks,65535),Math.ceil(tasks/65535)],`${label} diagnostic direct attention`);
      };
    },
  };
}
