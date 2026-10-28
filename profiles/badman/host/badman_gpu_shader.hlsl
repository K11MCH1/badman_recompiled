// Independently written integer pixel pipeline shared by D3D11 and Vulkan.
// CPU setup supplies exact fixed-point UVs, validated footprints and tile bins.
StructuredBuffer<uint> input_words : register(t0);
RWStructuredBuffer<uint> state_words : register(u0);
uint byte_at(uint base,uint address){return (input_words[base+address/4]>>((address&3)*8))&255;}
int floor16(int x){return x/16-((x<0 && x%16!=0)?1:0);}
int wrap_index(int x,uint size,bool clamp_mode){
    if(clamp_mode)return clamp(x,0,int(size)-1);
    int n=x%int(size);return n<0?n+int(size):n;
}
uint expand5(uint x){return (x<<3)|(x>>2);}
uint expand6(uint x){return (x<<2)|(x>>4);}
uint texel(uint t,int x,int y){
    uint width=input_words[t],height=input_words[t+1],pitch=input_words[t+2];
    uint format=input_words[t+3],flags=input_words[t+4],length=input_words[t+5];
    x=wrap_index(x,width,(flags&2)!=0);y=wrap_index(y,height,(flags&4)!=0);
    uint bpp=format==3?4:(format>=4?1:2);
    uint bx=format==4?uint(x)/2:uint(x)*bpp;
    uint offset=uint(y)*pitch+bx;
    if(flags&1)offset=(uint(y)/8)*(pitch*8)+(bx/16)*128+(uint(y)%8)*16+(bx%16);
    uint base=t+6;
    uint a=byte_at(base,offset);
    if(format>=4){uint index=format==4?((a>>((uint(x)&1)*4))&15):a;return input_words[base+length+index];}
    uint b=byte_at(base,offset+1);
    uint word=a|(b<<8);
    if(format==3)return word|(byte_at(base,offset+2)<<16)|(byte_at(base,offset+3)<<24);
    if(format==0)return 0xFF000000|expand5(word&31)|(expand6((word>>5)&63)<<8)|(expand5((word>>11)&31)<<16);
    if(format==1)return ((word&0x8000)?0xFF000000:0)|expand5(word&31)|(expand5((word>>5)&31)<<8)|(expand5((word>>10)&31)<<16);
    return ((word&15)*17)|(((word>>4)&15)*17<<8)|(((word>>8)&15)*17<<16)|(((word>>12)&15)*17<<24);
}
uint interpolate(uint a,uint b,uint fraction){
    uint value=0;
    [unroll]for(uint shift=0;shift<32;shift+=8)value|=(( ((a>>shift)&255)*(16-fraction)+((b>>shift)&255)*fraction)>>4)<<shift;
    return value;
}
uint sample_texture(uint t,int u,int v,bool minifying){
    uint flags=input_words[t+4];
    if(!(flags&(minifying?8:16)))return texel(t,floor16(u),floor16(v));
    int left=floor16(u-8),top=floor16(v-8);
    uint fx=uint(u-8-left*16),fy=uint(v-8-top*16);
    return interpolate(interpolate(texel(t,left,top),texel(t,left+1,top),fx),
        interpolate(texel(t,left,top+1),texel(t,left+1,top+1),fx),fy);
}
uint modulate(uint tex,uint color,bool alpha){
    uint result=0;
    [unroll]for(uint shift=0;shift<24;shift+=8)
        result|=((((tex>>shift)&255)*(((color>>shift)&255)+1))>>8)<<shift;
    return result|((alpha?(((color>>24)+1)*(tex>>24)>>8):(color>>24))<<24);
}
int factor(uint code,int other,uint sa,uint da,uint fixed_value){
    if(code==0)return other;if(code==1)return 255-other;
    if(code==2)return int(sa);if(code==3)return 255-int(sa);
    if(code==4)return int(da);if(code==5)return 255-int(da);return int(fixed_value);
}
uint blend(uint src,uint dst,uint mode,uint fixed_a,uint fixed_b){
    uint output=dst&0xFF000000,equation=(mode>>8)&7;
    [unroll]for(uint shift=0;shift<24;shift+=8){
        int s=int((src>>shift)&255),d=int((dst>>shift)&255),value;
        if(equation==3)value=min(s,d);else if(equation==4)value=max(s,d);else if(equation==5)value=abs(s-d);
        else{
            int a=((2*s+1)*(2*factor(mode&15,d,src>>24,dst>>24,(fixed_a>>shift)&255)+1))>>10;
            int b=((2*d+1)*(2*factor((mode>>4)&15,s,src>>24,dst>>24,(fixed_b>>shift)&255)+1))>>10;
            value=equation==0?a+b:(equation==1?a-b:b-a);
        }
        output|=uint(clamp(value,0,255))<<shift;
    }
    return output;
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint stride=input_words[1],height=input_words[2],pixel_index=id.x;
    if(pixel_index>=stride*height)return;
    uint x=pixel_index%stride,y=pixel_index/stride;
    uint bin=input_words[3]+((y/16)*64+x/16)*2;
    uint indices=input_words[bin],count=input_words[bin+1];
    uint address=input_words[0]+pixel_index;
    if(address>=524288)return;
    uint destination=state_words[address];
    for(uint i=0;i<count;++i){
        uint p=input_words[4]+input_words[indices+i]*20;
        uint x0=input_words[p],y0=input_words[p+1];
        if(x<x0||y<y0||x>=input_words[p+2]||y>=input_words[p+3])continue;
        uint color=input_words[p+4],preserve=input_words[p+5],flags=input_words[p+9];
        if(flags&2){
            int u=int(input_words[input_words[p+11]+((flags&16)?y-y0:x-x0)]);
            int v=int(input_words[input_words[p+12]+((flags&16)?x-x0:y-y0)]);
            color=modulate(sample_texture(input_words[p+10],u,v,(flags&32)!=0),color,(flags&4)!=0);
        }
        if(flags&1)color=blend(color,destination,input_words[p+6],input_words[p+7],input_words[p+8]);
        if(flags&8){
            uint nibble=(input_words[p+13+(y&3)]>>((x&3)*4))&15;
            int delta=int(nibble)-(nibble>=8?16:0);uint adjusted=color&0xFF000000;
            [unroll]for(uint shift=0;shift<24;shift+=8)adjusted|=uint(clamp(int((color>>shift)&255)+delta,0,255))<<shift;
            color=adjusted;
        }
        destination=(destination&preserve)|(color&~preserve);
    }
    state_words[address]=destination;
}
