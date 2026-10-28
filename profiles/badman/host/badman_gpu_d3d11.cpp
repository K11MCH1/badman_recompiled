#include "badman_gpu_device.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace badman {
namespace {
using Microsoft::WRL::ComPtr;
using psprecomp::Error;
std::string hex_result(HRESULT result) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
        << static_cast<std::uint32_t>(result);
    return out.str();
}
void checked(HRESULT result, std::string_view operation) {
    if (FAILED(result)) throw Error("D3D11 compute: " + std::string(operation) + " failed (" + hex_result(result) + ")");
}
std::string adapter_name(const DXGI_ADAPTER_DESC1 &desc) {
    const auto length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,desc.Description,-1,nullptr,0,nullptr,nullptr);
    if (length<=0) throw Error("D3D11 compute: adapter description is not valid Unicode");
    std::string name(static_cast<std::size_t>(length),'\0');
    if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,desc.Description,-1,name.data(),length,nullptr,nullptr)!=length)
        throw Error("D3D11 compute: cannot convert adapter description");
    name.pop_back();return name;
}
bool physical_vendor(UINT vendor) {
    return vendor==0x8086u || vendor==0x10DEu || vendor==0x1002u || vendor==0x1022u;
}
struct Adapter { ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 desc{}; };
struct Buffers {
    ComPtr<ID3D11Buffer> input, state, staging;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::size_t input_words{}, state_words{};
    std::size_t input_capacity{}, state_capacity{};
};
class D3D11Compute final : public GpuDevice {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11ComputeShader> shader_;
    Buffers buffers_;
    std::string description_;
    std::mutex mutex_;
    std::vector<std::uint32_t> readback_; // Reused; holds only the latest completed transaction.
    void device_ready() const { checked(device_->GetDeviceRemovedReason(),"device status"); }
    void prepare_buffers(std::size_t input_words,std::size_t state_words) {
        if (buffers_.input_words==input_words && buffers_.state_words==state_words) return;
        // Allocation capacity and shader-visible element counts are separate.
        // Retain unaffected resources, and commit a complete usable set only
        // after every allocation/view creation succeeds.
        auto next=buffers_;
        const auto capacity=[](std::size_t old,std::size_t wanted) {
            auto value=std::max<std::size_t>(old,1);
            while(value<wanted)value*=2; // Validated counts <=2^27; no overflow.
            return value;
        };
        const bool grow_input=input_words>next.input_capacity;
        const bool grow_state=state_words>next.state_capacity;
        D3D11_BUFFER_DESC desc{};
        desc.Usage=D3D11_USAGE_DEFAULT;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride=sizeof(std::uint32_t);
        if(grow_input) {
            next.input_capacity=capacity(next.input_capacity,input_words);
            desc.ByteWidth=static_cast<UINT>(next.input_capacity*sizeof(std::uint32_t));
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            checked(device_->CreateBuffer(&desc,nullptr,next.input.ReleaseAndGetAddressOf()),"grow input buffer");
        }
        if(grow_state) {
            next.state_capacity=capacity(next.state_capacity,state_words);
            desc.ByteWidth=static_cast<UINT>(next.state_capacity*sizeof(std::uint32_t));
            desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
            checked(device_->CreateBuffer(&desc,nullptr,next.state.ReleaseAndGetAddressOf()),"grow state buffer");
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            desc.MiscFlags=0;desc.StructureByteStride=0;
            checked(device_->CreateBuffer(&desc,nullptr,next.staging.ReleaseAndGetAddressOf()),"grow readback buffer");
        }
        if(grow_input||next.input_words!=input_words) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_UNKNOWN;srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
            srv.Buffer.NumElements=static_cast<UINT>(input_words);
            checked(device_->CreateShaderResourceView(next.input.Get(),&srv,next.srv.ReleaseAndGetAddressOf()),"create logical input view");
        }
        if(grow_state||next.state_words!=state_words) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_UNKNOWN;uav.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements=static_cast<UINT>(state_words);
            checked(device_->CreateUnorderedAccessView(next.state.Get(),&uav,next.uav.ReleaseAndGetAddressOf()),"create logical state view");
        }
        next.input_words=input_words;next.state_words=state_words;
        buffers_=std::move(next);
    }
public:
    explicit D3D11Compute(std::string_view hlsl) {
        if (hlsl.empty() || hlsl.size()>4u*1024u*1024u || hlsl.find('\0')!=std::string_view::npos)
            throw Error("D3D11 compute: shader source must contain 1..4194304 non-NUL bytes");
        ComPtr<ID3DBlob> code,errors;
        const auto compiled=D3DCompile(hlsl.data(),hlsl.size(),"BadmanCompute",nullptr,nullptr,"main","cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_IEEE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,
            code.GetAddressOf(),errors.GetAddressOf());
        if (FAILED(compiled)) {
            std::string diagnostic;
            if(errors)diagnostic.assign(static_cast<const char*>(errors->GetBufferPointer()),
                std::min<std::size_t>(errors->GetBufferSize(),16384));
            while(!diagnostic.empty()&&diagnostic.back()=='\0')diagnostic.pop_back();
            throw Error("D3D11 compute: cs_5_0 compilation failed ("+hex_result(compiled)+"): "+diagnostic);
        }
        ComPtr<ID3D11ShaderReflection> reflection;
        checked(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(reflection.GetAddressOf())),"reflect shader");
        UINT x{},y{},z{};reflection->GetThreadGroupSize(&x,&y,&z);
        if(x!=64||y!=1||z!=1)throw Error("D3D11 compute: shader main must use numthreads(64,1,1)");
        D3D11_SHADER_DESC shader_desc{};checked(reflection->GetDesc(&shader_desc),"reflect shader description");
        bool output=false;
        for(UINT i=0;i<shader_desc.BoundResources;++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding{};checked(reflection->GetResourceBindingDesc(i,&binding),"reflect buffer binding");
            const bool input=binding.Type==D3D_SIT_STRUCTURED&&binding.BindPoint==0&&binding.BindCount==1;
            const bool state=binding.Type==D3D_SIT_UAV_RWSTRUCTURED&&binding.BindPoint==0&&binding.BindCount==1;
            if((!input&&!state)||binding.Dimension!=D3D_SRV_DIMENSION_BUFFER)
                throw Error("D3D11 compute: shader bindings must be StructuredBuffer t0 and RWStructuredBuffer u0 only");
            output|=state;
        }
        if(!output)throw Error("D3D11 compute: shader must write RWStructuredBuffer u0");
        const auto requirements=reflection->GetRequiresFlags();
        ComPtr<IDXGIFactory1> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),"enumerate hardware adapters");
        std::vector<Adapter> adapters;
        for(UINT i=0;;++i) {
            Adapter adapter;const auto result=factory->EnumAdapters1(i,adapter.adapter.GetAddressOf());
            if(result==DXGI_ERROR_NOT_FOUND)break;
            checked(result,"enumerate adapter");checked(adapter.adapter->GetDesc1(&adapter.desc),"query adapter");
            if(!(adapter.desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))adapters.push_back(std::move(adapter));
        }
        std::stable_sort(adapters.begin(),adapters.end(),[](const Adapter &a,const Adapter &b){
            if(physical_vendor(a.desc.VendorId)!=physical_vendor(b.desc.VendorId))return physical_vendor(a.desc.VendorId);
            return a.desc.DedicatedVideoMemory>b.desc.DedicatedVideoMemory;
        });
        std::string failures;
        for(const auto &adapter:adapters) {
            const auto name=adapter_name(adapter.desc);
            constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL actual{};
            auto result=D3D11CreateDevice(adapter.adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,2,D3D11_SDK_VERSION,
                device_.ReleaseAndGetAddressOf(),&actual,context_.ReleaseAndGetAddressOf());
            if(result==E_INVALIDARG)result=D3D11CreateDevice(adapter.adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,
                levels+1,1,D3D11_SDK_VERSION,device_.ReleaseAndGetAddressOf(),&actual,context_.ReleaseAndGetAddressOf());
            if(FAILED(result)){failures+=name+" create="+hex_result(result)+"; ";continue;}
            D3D11_FEATURE_DATA_DOUBLES doubles{};
            const bool has_doubles=SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_DOUBLES,&doubles,sizeof(doubles)))&&doubles.DoublePrecisionFloatShaderOps;
            D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
            const bool extended=SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))&&options.ExtendedDoublesShaderInstructions;
            if((requirements&D3D_SHADER_REQUIRES_DOUBLES)&&!has_doubles){failures+=name+" lacks double shader operations; ";continue;}
            if((requirements&D3D_SHADER_REQUIRES_11_1_DOUBLE_EXTENSIONS)&&!extended){failures+=name+" lacks extended double shader operations; ";continue;}
            result=device_->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader_.ReleaseAndGetAddressOf());
            if(FAILED(result)){failures+=name+" shader="+hex_result(result)+"; ";continue;}
            description_="Direct3D 11 compute: "+name+" (feature "+(actual==D3D_FEATURE_LEVEL_11_1?"11.1":"11.0")+
                ", doubles="+(has_doubles?"yes":"no")+", extended-doubles="+(extended?"yes":"no")+")";
            return;
        }
        throw Error("D3D11 compute: no compatible hardware adapter; "+failures);
    }
    std::string description() const override {return description_;}
    void dispatch(std::span<const std::uint32_t> input,std::span<std::uint32_t> state,std::uint32_t groups) override {
        // Validate before allocation, GPU work or observing caller contents.
        constexpr std::size_t max_words=1u<<D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
        if(input.empty()||state.empty()||input.size()>max_words||state.size()>max_words)
            throw Error("D3D11 compute: input/state word counts must be in 1..134217728");
        if(!groups||groups>D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
            throw Error("D3D11 compute: group count must be in 1..65535");
        std::lock_guard lock(mutex_);
        device_ready();prepare_buffers(input.size(),state.size());
        auto &result=readback_;
        result.resize(state.size());
        // A null box would upload the allocation capacity and read past a
        // smaller caller span. Buffer boxes are byte ranges with exclusive end.
        const D3D11_BOX input_box{0,0,0,static_cast<UINT>(input.size()*sizeof(std::uint32_t)),1,1};
        const D3D11_BOX state_box{0,0,0,static_cast<UINT>(state.size()*sizeof(std::uint32_t)),1,1};
        context_->UpdateSubresource(buffers_.input.Get(),0,&input_box,input.data(),0,0);
        context_->UpdateSubresource(buffers_.state.Get(),0,&state_box,state.data(),0,0);
        auto *srv=buffers_.srv.Get();auto *uav=buffers_.uav.Get();
        context_->CSSetShader(shader_.Get(),nullptr,0);
        context_->CSSetShaderResources(0,1,&srv);context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        context_->Dispatch(groups,1,1);
        ID3D11ShaderResourceView *no_srv=nullptr;ID3D11UnorderedAccessView *no_uav=nullptr;
        context_->CSSetShaderResources(0,1,&no_srv);context_->CSSetUnorderedAccessViews(0,1,&no_uav,nullptr);
        context_->CSSetShader(nullptr,nullptr,0);
        context_->CopyResource(buffers_.staging.Get(),buffers_.state.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context_->Map(buffers_.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"map completed readback");
        struct Unmap {ID3D11DeviceContext *context;ID3D11Buffer *buffer;~Unmap(){context->Unmap(buffer,0);}} unmap{context_.Get(),buffers_.staging.Get()};
        if(!mapped.pData)throw Error("D3D11 compute: successful readback has no data");
        std::memcpy(result.data(),mapped.pData,result.size()*sizeof(std::uint32_t));
        device_ready();
        // This is the sole publication to caller memory, after the whole GPU
        // transaction and readback succeed. No guest writes occur in this class.
        std::copy(result.begin(),result.end(),state.begin());
    }
};
}
std::unique_ptr<GpuDevice> make_d3d11_compute(std::string_view hlsl) {return std::make_unique<D3D11Compute>(hlsl);}
}
#else
namespace badman {
std::unique_ptr<GpuDevice> make_d3d11_compute(std::string_view) {
    throw psprecomp::Error("Direct3D 11 compute requires Windows");
}
}
#endif
