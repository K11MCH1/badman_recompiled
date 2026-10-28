"""Embed independently written HLSL and its Vulkan SPIR-V for SDK-free local builds.

Developer-only regeneration requires Microsoft's separately obtained DXC.
The generated header contains no game material. Players compile this header with
the runtime; neither a Vulkan SDK nor a shader compiler is required at setup.
"""
import argparse
from pathlib import Path
import struct
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    host=Path(__file__).resolve().parents[1]/'host'
    source=host/'badman_gpu_shader.hlsl'
    output=args.output or host/'badman_gpu_shader.hpp'
    import tempfile
    with tempfile.TemporaryDirectory(prefix='badman-independent-shader-') as work:
        spirv=Path(work)/'raster.spv'
        subprocess.run([str(args.dxc),'-spirv','-T','cs_6_0','-E','main',
            '-fspv-target-env=vulkan1.0','-fvk-t-shift','0','0',
            '-fvk-u-shift','1','0','-O3','-Fo',str(spirv),str(source)],check=True)
        binary=spirv.read_bytes()
    words=struct.unpack('<'+'I'*(len(binary)//4),binary)
    assert words[0]==0x07230203
    lines=['#pragma once','#include <array>','#include <cstdint>','#include <string_view>',
        '// Generated from the independent badman_gpu_shader.hlsl; no game code.',
        'namespace badman {',
        'inline constexpr std::string_view gpu_shader_hlsl=R"BADMAN_SHADER('+source.read_text(encoding='utf-8')+')BADMAN_SHADER";',
        f'inline constexpr std::array<std::uint32_t,{len(words)}> gpu_shader_spirv{{']
    lines.extend('    '+','.join(f'0x{w:08x}u' for w in words[i:i+8])+',' for i in range(0,len(words),8))
    lines+=['};','}']
    output.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(f'Embedded HLSL and {len(binary)} SPIR-V bytes in {output}')

if __name__=='__main__':main()
